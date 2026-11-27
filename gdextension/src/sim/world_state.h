#pragma once
// Estado da simulação econômica (S5a-2 do docs/economy/core_refactor_plan.md).
// C++ puro (só STL, via sim_types.h) — nada de godot_cpp. É o "WorldState" que
// o SimCore (sim_core.*) recebe e muta a cada tick; a fachada EconomySim
// (economy_sim.*) guarda UM `WorldState ws_` e converte Godot <-> sim.

#include "sim_constants.h"
#include "sim_types.h"
#include "urban_types.h"
#include "power.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace ogs {

// A2 — tabela de nomes -> índice inteiro. Só a fronteira de I/O (save, status,
// binding) vê strings; o caminho quente usa o índice.
//
// `names` fica em ORDEM LEXICOGRÁFICA: um mapa ordenado por índice tem, então,
// exatamente a mesma ordem de iteração do antigo std::map<std::string, ...> —
// somas em ponto flutuante dependem da ordem e não podem mudar.
struct NameTable {
	std::vector<std::string> names;               // índice -> nome
	std::unordered_map<std::string, int> lookup;  // nome -> índice

	void clear() {
		names.clear();
		lookup.clear();
	}
	int size() const { return static_cast<int>(names.size()); }
	// -1 quando o nome não está na tabela (ausência explícita).
	int find(const std::string& name) const {
		const auto found = lookup.find(name);
		return found == lookup.end() ? -1 : found->second;
	}
	const std::string& at(int index) const { return names[static_cast<std::size_t>(index)]; }
	// Acrescenta sem ordenar — SÓ no carregamento, antes de build().
	int intern(const std::string& name) {
		const auto found = lookup.find(name);
		if (found != lookup.end()) {
			return found->second;
		}
		const int index = static_cast<int>(names.size());
		names.push_back(name);
		lookup.emplace(names.back(), index);
		return index;
	}
	// Ordena e deduplica a partir do conjunto de nomes observado.
	void build(const std::vector<std::string>& source) {
		clear();
		std::vector<std::string> unique;
		unique.reserve(source.size());
		for (const std::string& name : source) {
			unique.push_back(name);
		}
		std::sort(unique.begin(), unique.end());
		unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
		names = std::move(unique);
		lookup.reserve(names.size() * 2 + 8);
		for (std::size_t index = 0; index < names.size(); ++index) {
			lookup.emplace(names[index], static_cast<int>(index));
		}
	}
};

// A6 — cadência por sistema, escalonada. Cada sistema que sai do diário
// declara um período em dias; a cidade `index` roda o sistema quando
// `(dia - index) % período == 0`. É determinístico (dia + índice bastam, sem
// cursor) e espalha o trabalho: em vez de as 834 cidades fecharem o mês no
// mesmo dia, ~28 por dia. `period <= 1` faz TODAS as cidades rodarem todo dia
// (interruptor de teste; não reproduz o calendário antigo).
inline bool cadence_due(int day, std::size_t index, int period) {
	if (period <= 1) {
		return true;
	}
	const int offset = static_cast<int>(index % static_cast<std::size_t>(period));
	return ((day - offset) % period + period) % period == 0;
}

// Períodos por classe de sistema (A6). Serializado no save (v15): mudar o
// período muda QUANDO cada cidade fecha o mês/semana, então é estado do mundo.
struct CadenceState {
	// Semanal: decisões de abertura/fechamento dos negócios de serviço e a
	// agenda da IA privada/governo.
	int weekly_period = 7;
	// Mensal: preço do solo/zona, fechamento do mês de moradia, fluxo mensal da
	// prefeitura e agenda da IA de grupos.
	int monthly_period = 30;
};

// ---------------------------------------------------------------------------
// D-125 — EFICIÊNCIA DE PRODUÇÃO (estilo HOI4). Espelho C++ dos defaults de
// balanceamento declarados em data/definitions/production/efficiency.json.
// NÃO são dado observado: são regras do jogo (ajustáveis pelo dono). O JSON é
// a fonte de balanceamento; `set_efficiency_params` sobrescreve estes campos e
// o bloco `definitions.efficiency` do save preserva o que foi aplicado.
//
// Curva por classe: linear em dias de operação, `start` no dia 1, `mid` em
// `mid_days` e 1.0 em `full_days` (>= mid_days). Equipamento multiplica o
// passo diário por `equipment_rate_boost` e eleva o teto de `cap_without`
// para 1.0.
// ---------------------------------------------------------------------------
struct EfficiencyCurve {
	EfficiencyCurve() = default;
	EfficiencyCurve(double first, double middle, double middle_days,
			double maturity_days)
		: start(first), mid(middle), mid_days(middle_days),
		  full_days(maturity_days) {}
	double start = 0.30;
	double mid = 0.90;
	double mid_days = 90.0;
	double full_days = 180.0;
};

struct EfficiencyParams {
	// Espelho EXATO de data/definitions/production/efficiency.json: se o JSON
	// não carregar, a simulação usa estes valores e não diverge do arquivo.
	EfficiencyCurve factory{0.30, 0.90, 90.0, 180.0};
	EfficiencyCurve refinery{0.30, 0.90, 90.0, 180.0};
	EfficiencyCurve mine{0.25, 1.0, 240.0, 240.0};
	EfficiencyCurve farm{0.20, 1.0, 365.0, 365.0};
	EfficiencyCurve ranch{0.20, 1.0, 540.0, 540.0};
	// Equipamento (trator / máquina de mineração).
	double equipment_cap_without = 0.85;
	double equipment_rate_boost = 1.5;
	double equipment_fuel_per_capacity_day = 0.0002;
	int equipment_fuel_product = -1;  // resolvido no set_goods (diesel)
	// Dias de transição sem produção na troca de produto.
	int transition_factory_days = 7;
	int transition_refinery_days = 7;
	int transition_military_days = 14;
	int transition_semiconductor_days = 30;
	int transition_mode_days = 3;
	// Retenção da eficiência antiga (mesma família = goods.json.sector).
	double retention_same_family = 0.60;
	double retention_other = 0.0;
	// Modos da refinaria (frações do lote de petróleo: gasolina, diesel, querosene).
	double mode_shares[3][3] = {
		{0.45, 0.29, 0.09},
		{0.38, 0.36, 0.09},
		{0.40, 0.28, 0.15},
	};
	int refinery_gasoline = -1;
	int refinery_diesel = -1;
	int refinery_jet = -1;
	// IA: só troca de modo quando o ganho cobre a transição.
	int ai_mode_cooldown_days = 30;
	int ai_mode_payback_days = 90;
	double ai_mode_min_gain = 0.05;
};

// B1 — estrutura etária observada por nação (age_structure_2026.json). É uma
// DEFINIÇÃO estática (não entra no save nem no tick): a demografia por coortes
// vem depois. Os cinco `band` seguem a ordem do arquivo:
//   0 = 0-14, 1 = 15-24, 2 = 25-44, 3 = 45-64, 4 = 65+.
// Os valores são frações da população; `valid` falso = sem dado para a nação.
struct AgeStructure {
	double band[5] = {};
	double frac_18_49 = 0.0;
	double births_per_1000 = 0.0;
	double deaths_per_1000 = 0.0;
	double tfr = 0.0;
	bool valid = false;
};

// C2a (D-103) — série MENSAL do CPI por nação. Só MEDIÇÃO: guarda o índice
// medido a cada 30 dias (o modelo tem 12 meses de 30 dias = anel de 13
// amostras, o que dá 12 meses de intervalo) para a inflação anual. O CPI é a
// cesta do G2 (ver `central_bank::cpi_by_nation`).
struct CpiHistory {
	double samples[13] = {};
	int sample_day[13] = {};
	int count = 0;
};

// ---------------------------------------------------------------------------
// D-126 — SOBREVIVÊNCIA EM CRISE. Espelho C++ dos defaults de balanceamento
// declarados em `data/definitions/economy/crisis.json`. NÃO são dado observado:
// são regras do jogo (rescisão, crédito de giro, deságio de liquidação,
// hibernação, leilão e redução de área). O JSON é a fonte de balanceamento;
// `set_crisis_params` sobrescreve e o bloco `definitions.crisis` do save v41
// preserva o que foi aplicado. A ordem da reação é fixa e não é parâmetro:
// corte -> demissão -> crédito -> liquidação -> hibernação -> leilão.
// ---------------------------------------------------------------------------
struct CrisisParams {
	bool enabled = true;
	int cut_days = 5;
	int layoff_days = 20;
	double layoff_share = 0.25;
	double layoff_min_jobs_share = 0.10;
	double severance_days = 60.0;
	int layoff_cooldown_days = 30;
	int credit_days = 30;
	double credit_working_days = 60.0;
	double credit_asset_share = 0.5;
	double credit_rate_add = 0.02;
	int credit_renegotiation_days = 15;
	double credit_penalty = 0.05;
	int credit_max_renegotiations = 1;
	int liquidate_days = 40;
	double liquidation_discount = 0.30;
	double liquidation_share = 0.5;
	int mothball_days = 50;
	double mothball_opex_share = 0.10;
	double mothball_efficiency_retention = 0.60;
	double reopen_cost_per_capacity = 20.0;
	int reopen_signal_days = 10;
	int mothball_grace_days = 60;
	int bankrupt_days = 60;
	double auction_discount = 0.30;
	double auction_state_share = 0.5;
	double farm_area_reduction = 0.25;
	int farm_area_reduction_days = 30;
	// D-126 (correção): margem de reserva de pico da REDE. O despacho E2/E3
	// não declara margem de reserva (o mérito só cobre a demanda do dia);
	// este default de balanceamento é o que define a capacidade FIRME que a
	// crise deve manter ativa: pico × (1 + margem). Usinas além desse prefixo
	// por custo marginal são excedentes e podem hibernar; as necessárias não.
	double power_reserve_margin = 0.15;
};

// ---------------------------------------------------------------------------
// D-131 — CÉREBRO ECONÔMICO DA IA (fase A: previsão de caixa + preço
// esperado). Espelho C++ dos defaults declarados em
// `data/definitions/economy/ai_brain.json`. NÃO são dado observado: são
// regras de balanceamento declaradas. `set_ai_brain_params` sobrescreve e o
// bloco `definitions.ai_brain` do save v42 preserva o que foi aplicado.
// `enabled=false` (default) mantém as decisões da IA bit a bit como antes.
// ---------------------------------------------------------------------------
struct AiBrainParams {
	// D-131 fase C: o default do JOGO (ai_brain.json) é ligado; este default
	// compilado continua `false` como fallback de save/mundo sem o bloco. O
	// caminho desligado por override (`enabled=false`) é o que garante a
	// equivalência bit a bit nos testes.
	bool enabled = false;
	// R1 — previsão de fluxo de caixa mensal.
	int month_days = 30;
	int months_max = 12;
	int trough_months_max = 6;
	double capital_share = 0.35;
	// R2 — concorrência. `scope_food`/`scope_other` são o enum declarado do
	// JSON (`region` = 0, `world` = 1); o save grava os nomes e o loader
	// converte de volta. A janela da média móvel de demanda é `demand_growth_days`.
	int scope_food = 0;
	int scope_other = 1;
	int demand_growth_days = 30;
	// R3 — preço esperado (média móvel + castigo por excesso).
	int trend_days = 30;
	int history_days = 60;
	double trend_min = 0.5;
	double trend_max = 2.0;
	double elasticity = 0.35;
	double price_floor = 0.60;
	// R4 — exposição e anti-manada (fase B; os defaults já entram no save).
	double max_good_share = 0.35;
	double max_sector_share = 0.60;
	int max_goods_per_agent = 8;
	int max_new_plants_per_good_30d = 3;
	double max_new_capacity_share_30d = 0.25;
	double food_multiplier = 2.0;
	// R5 — vínculo com a D-126.
	int block_loss_days = 30;
	// G2 (spec G2_bootstrap_industrial.md §3): piso de demanda potencial na
	// visão da IA. `enabled` é o default compilado desligado; o jogo liga pelo
	// ai_brain.json. Com `enabled=false` o snapshot não executa o bloco.
	bool potential_enabled = false;
	double potential_discount = 0.5;
	double potential_cap = 1.0;
	// G2 (perf): o piso por cidade e recalculado no dia 0 e a cada N dias
	// (`potential_demand.recompute_days` no ai_brain.json). Entre recalculos
	// vale o valor cacheado -- mensal de proposito, o piso nao acompanha a
	// populacao dia a dia. 1 reproduz o calculo diario antigo.
	int potential_recompute_days = 30;
	// G2 (perf): varredura mensal escalonada do investidor. Com o cerebro
	// ligado, a cidade/grupo `i` so avalia candidatos quando
	// `(dia - i) % scan_interval_days == 0` (offset deterministico, sem
	// cursor no save). Entre varreduras nao ha candidato novo; obras em
	// andamento seguem normais. `enabled=false` ignora este campo e mantem a
	// cadencia antiga (semanal/mensal do CadenceState), bit a bit.
	int scan_interval_days = 30;
};

// ---------------------------------------------------------------------------
// G2 — BOOTSTRAP INDUSTRIAL (spec G2_bootstrap_industrial.md §2).
//
// Definição estática vinda de `data/definitions/economy/bootstrap_industry.json`
// pelo `set_bootstrap_industry` antes de `initialize_world`. NÃO entra no save
// (como `age_structures`): o mundo que nasce já carrega as plantas semeadas.
// `enabled=false`/arquivo ausente = vetor vazio = zero build (mundo idêntico).
// ---------------------------------------------------------------------------
struct BootstrapProducer {
	std::string nation;  // ISO3 do dado; "outros" = residual declarado
	double share = 0.0;
};

struct BootstrapGood {
	std::string id;            // id do bem em goods.json
	std::string source;        // citação da participação real
	std::string params_source; // declared:balancing quando não há fonte
	std::vector<BootstrapProducer> producers;
};

struct BootstrapIndustry {
	bool enabled = false;
	bool loaded = false;
	double budget_share = 0.50;
	double seed_fraction = 0.10;
	double capacity_min = 0.05;
	double capacity_max = 20.0;
	int max_cities_per_nation = 1;
	// Insumos primários usados no critério de cidade (§2.3 regra 4).
	std::vector<std::string> score_inputs;
	// Bens na ordem declarada do JSON (ordem da semeadura).
	std::vector<BootstrapGood> goods;
};

// Diagnóstico da semente (status; não vai ao save).
struct BootstrapPlanRow {
	std::string good;
	std::string nation;
	std::string city;
	double capacity = 0.0;
	double cost = 0.0;
	int recipe = -1;
	bool seeded = false;
	std::string reason;  // "" | "no_city" | "budget" | "no_recipe"
};

struct WorldState {
	int day = 0;
	CadenceState cadence;
	// D-125: parâmetros de balanceamento da eficiência de produção (defaults
	// declarados no JSON de produção; o save preserva o que foi aplicado).
	EfficiencyParams efficiency;
	// D-126: parâmetros de sobrevivência em crise (defaults declarados no JSON
	// de crise; o save preserva o que foi aplicado).
	CrisisParams crisis;
	// D-131: parâmetros do cérebro econômico da IA (fase A; o save v42
	// preserva o que foi aplicado).
	AiBrainParams ai_brain;
	// G2: bootstrap industrial (definição estática; não vai ao save) e o
	// relatório da semente para o status.
	BootstrapIndustry bootstrap;
	std::vector<BootstrapPlanRow> bootstrap_rows;
	double bootstrap_shortfall = 0.0;
	double bootstrap_unseeded_share = 0.0;
	int bootstrap_plants = 0;

	// Estado do cenário de validação de duas cidades.
	bool active = false;
	// true quando o demo foi inicializado a partir do mundo real
	// (initialize_world), em vez do cenário AGRIA/PORTUS. Ativa o comércio
	// genérico entre cidades no lugar dos blocos específicos de validação.
	bool world_mode = false;
	// R1a: flag `rework.regional_cash` (data/definitions/economy/rework.json).
	// true = firm_cash+service_cash formam UM caixa privado por regiao
	// (`City::private_cash`). Default false = comportamento anterior.
	bool regional_cash = false;
	// R1a (medicao): modo do sub-teto externo por setor, espelhado em
	// `City::external_subceiling`. Default `off` = sem teto por fatia; quem
	// limita o pagamento externo e o sub-saldo do setor. O teto por fatia
	// (t160-t163) segue como experimento, nao e serializado (configuracao lida
	// do rework.json).
	int external_subceiling = 0;
	// R1b - TESOURO NACIONAL + ORCAMENTO REGIONAL (flag
	// `rework.national_treasury`).
	//
	// Ligada (so com `regional_cash` tambem ligada): a arrecadacao de impostos
	// municipais (IPTU + taxas locais; o imposto de renda do modelo e rebatido
	// 100%, ver kTransferShare) passa a entrar no Tesouro da nacao
	// (`nation_treasury`) em vez do caixa da cidade. A equalizacao fiscal e o
	// repasse municipal morrem. O dinheiro NAO e transferido para a regiao:
	// cada regiao recebe apenas um LIMITE mensal de gasto
	// (`regional_budget_*`), proporcional a populacao com piso minimo; todo
	// gasto publico debita o Tesouro ate esse limite e o que nao for gasto
	// permanece no Tesouro.
	//
	// Desligada (default): nenhuma chave nova no save, nenhum numero muda.
	bool national_treasury = false;
	// Saldo do Tesouro por nacao (XGS). `std::map` pela ordem deterministica do
	// save/status. So tem valores com a flag ligada.
	std::map<std::string, double> nation_treasury;
	// Parametros declarados do orcamento regional (rework.json):
	//   share                 fracao do saldo do Tesouro disponibilizada como
	//                         limite no mes (1 = todo o saldo);
	//   floor_fraction        piso por regiao como fracao do rateio igualitario
	//                         (peso = max(pop_share, floor/n_regioes));
	//   works_share           fatia do limite reservada a obras
	//                         (`CityGov::cash`); o resto vai para servicos;
	//   maintenance_share     teto da manutencao dentro do limite de servicos.
	double regional_budget_share = 1.0;
	double regional_budget_floor_fraction = 0.5;
	double regional_budget_works_share = 0.35;
	double regional_budget_maintenance_share = 0.5;
	// O primeiro mes foi aberto (sweep do caixa publico + dotacao inicial)?
	// Impede que um segundo apply da mesma configuracao zere o mes em curso.
	bool regional_budget_started = false;
	// Somas acumuladas (so diagnostico/teste; nao entram em decisao):
	// arrecadado no Tesouro e gasto publico debitado do Tesouro. O saldo do
	// Tesouro e o unico caixa publico sob a flag.
	double treasury_collected_total = 0.0;
	double treasury_spent_total = 0.0;
	// Configuracao invalida (treasury ON sem regional_cash ON): mensagem para
	// o log/status; a flag e tratada como desligada.
	std::string treasury_config_error;
	// Parâmetros de balanceamento do cenário demo (configuráveis pelo
	// laboratório; os padrões preservam o comportamento do jogo).
	double natural_unemployment = 0.04;
	double service_wage_adjust = 0.10;
	double service_wage_max = 1.0;
	// F7: rigidez salarial (fração máxima de ajuste por dia) e piso de salário
	// mínimo por nação (0 = desligado). Ganchos de política.
	double wage_rigidity = 0.01;
	// F7 calibração: folga de custo de vida não modelado no gate de renda →
	// nível de consumo. A cesta de needs.json cobre só os bens comerciáveis;
	// moradia/energia/serviços não têm preço no núcleo, então o nível exige uma
	// folga. Sem ela qualquer renda acima da cesta liberava luxury no mundo
	// inteiro (832/834 cidades). Calibrada no economy_world_test.
	double income_basket_margin = 2.30;
	std::unordered_map<std::string, double> minimum_wages;
	// B1: estrutura etária por tag de nação (age_structure_2026.json ->
	// set_age_structure). `std::map` para a ordem determinística do dump.
	std::map<std::string, AgeStructure> age_structures;
	// B7: definição estática por tag de nação (informality.json e
	// health_access.json -> set_informality/set_health_access). NÃO entram no
	// save (como `age_structures`): a fração inicial é semeada no dia 0 e a
	// referência de saúde só é lida na semeadura/formalização.
	std::map<std::string, double> informal_rates;
	std::map<std::string, double> health_access;
	// ------------------------------------------------------------------
	// G — AGRICULTURA SAZONAL (save v24). Ver gdextension/src/sim/agriculture.h
	// e docs/economy/agriculture_seasonal.md.
	//
	// DEFINIÇÃO (dado): `crop_plans` (área × rendimento e janelas por cidade/
	// produto), `crop_loss_pct`/`crop_storage_share_pct` (perda pós-colheita e
	// a fatia de armazenagem), `crop_shelf_days` (prateleira) e `agri_chain`
	// (os 12 bens da cadeia, cujo produto é resolvido no catálogo). Tudo chega
	// pelo AgricultureService e viaja no bloco `definitions` do snapshot.
	//
	// ESTADO (save): dentro de `CropPlan::state` (ano/entregue/fertilizante).
	// ------------------------------------------------------------------
	std::vector<CropPlan> crop_plans;
	// ------------------------------------------------------------------
	// G2 — ARMAZENAGEM (save v25). Ver gdextension/src/sim/storage.{h,cpp}.
	//
	// `storage_sites` é o armazém AGREGADO por (cidade, tipo, dono): capacidade
	// e estoque guardado (governo × privado). O estoque de mercado continua em
	// `City::markets`; o silo/tanque/gás/pátio ficam na região e a câmara
	// fria/armazém geral na cidade. `storage_product_type` resolve o tipo de
	// cada bem pelo catálogo (nomes de goods.json) e aceita override do dado
	// via `set_storage_product_type` (JSON da G2 quando entrar na base).
	// ------------------------------------------------------------------
	std::vector<StorageSite> storage_sites;
	std::vector<int> storage_product_type;
	bool storage_index_ready = false;
	int storage_products = 0;
	// Chave do mês da perda de armazenagem (derivada; 0 = nunca aplicado).
	int storage_month_key = 0;
	// Índice derivado por chave `cidade|produto` -> posição em `crop_plans`
	// (só leitura depois da carga; não serializado).
	std::map<std::string, int> crop_plan_lookup;
	// Índice derivado: `crop_planned[cidade × products + produto] != 0` quando
	// existe plano sazonal ativo — a produção diária da fazenda é SUPRIMIDA
	// nesse caso (não contar a mesma tonelada duas vezes).
	std::vector<char> crop_planned;
	// Índice derivado do DIA (não serializado): `crop_working[cidade × products
	// + produto] != 0` quando o par COM plano está dentro de uma janela de
	// plantio ou de colheita. Fora delas a fazenda sazonal fica parada — sem
	// equipe, folha ou opex — e não acumula dia de prejuízo na entressafra
	// (D-096: a folha cheia sem safra falia 324 fazendas em 365 d, 85/85
	// primárias no dia 75). Recalculado por `phase_agriculture` a partir do
	// calendário e de `day`; nunca entra no save.
	std::vector<char> crop_working;
	bool crop_index_ready = false;
	// Perda pós-colheita por produto (% do que entra) e a fatia de armazenagem
	// declarada (-1 = sem dado). Indexados pelo produto (-1 se fora).
	std::vector<double> crop_loss_pct;          // % (0..100)
	std::vector<double> crop_storage_share_pct; // % (0..100) ou -1
	// Dias de prateleira por produto (0 = sem dado: NÃO decai).
	std::vector<int> crop_shelf_days;
	// Fração diária de perda por perecibilidade (derivada da meia-vida, para o
	// tick não pagar `pow`).
	std::vector<double> crop_shelf_daily;
	// Bens da cadeia com prateleira publicada, na ordem do arquivo
	// (product_shelf_life.json). Vazio = pereccibilidade desligada.
	std::vector<int> agri_chain;
	// Índice do bem `fertilizer` no catálogo (-1 = não existe).
	int fertilizer_product = -1;
	// Chave de mês do livro de perdas (derivada; 0 = nunca aplicado).
	int agri_month_key = 0;
	// Acumulados desde o dia 0 (derivados, não serializados) — diagnósticos do
	// status/teste: perecibilidade, perda de armazenagem e perda de entrada.
	double agri_perishable_total[kMaxProducts] = {};
	double agri_storage_total[kMaxProducts] = {};
	double agri_entry_total[kMaxProducts] = {};
	// Memória de cálculo do dia (derivada, não serializada): quantas safras
	// entregaram hoje e quantas toneladas brutas entraram. Serve ao status e ao
	// teste de sazonalidade.
	double agri_delivered_today = 0.0;
	int agri_harvests_today = 0;
	// B5 (D-089) - gancho de LABORATÓRIO (set_balance): penalidade MÁXIMA de
	// acesso (estrada 0) usada por `SimCore::infra_access_multiplier`. Padrão =
	// kInfraAccessPenaltyMax; só o A/B da infra a zera para isolar o frete.
	// Não é persistido (knob de calibração, como income_basket_margin).
	double infra_access_penalty = kInfraAccessPenaltyMax;
	// D-105 (C2b1-fix) + C4 (D-110) + D-111: juro da taxa básica no ENVELOPE
	// de crédito da safra (D-099, `SimCore::crop_liquidity_factor`). LIGADO por
	// padrão desde a D-111: a C4 criou o credor (banco público da nação) que
	// recebe o juro (`bank.cash`/`bank.harvest_interest`), então não é mais
	// custo sem recebedor. Sem banco público o valor não é cobrado (D-105).
	// Ligado, o envelope vira `asset / (60 · (1 + r · 60/365))`;
	// declarado em `data/definitions/economy/monetary_policy.json`
	// (`harvest_credit.apply_policy_rate`) e conferido por
	// `tools/c2b1_monetary_test.gd`. Não é persistido (parâmetro de regra, como
	// infra_access_penalty).
	bool harvest_credit_rate = true;
	double inventory_cover_days = 3.0;
	double inventory_recovery = 0.5;
	// Índice espacial do comércio de alimento: por comprador, só os K
	// vendedores mais próximos entram na disputa, além de M ofertas
	// representativas do mercado externo (as mais baratas por caloria no globo).
	// Mundos pequenos (<= kTradeExhaustiveLimit) mantêm a varredura completa
	// para preservar exatamente o comportamento validado no cenário real.
	int trade_neighbors = 12;
	int trade_external = 8;
	std::vector<City> cities;
	std::vector<Building> buildings;
	// §15: obras de construção em andamento (projeto físico). Avançam no prólogo
	// do tick conforme materiais + capacidade do setor de construção.
	std::vector<ConstructionProject> construction_projects;
	// §19 (W0): solo urbano, moradia e prefeitura por cidade. Vetores paralelos
	// a `cities` (mesmo índice), carregados pelo set_urban. Vazios em
	// saves v12 e anteriores e no cenário sintético AGRIA/PORTUS.
	std::vector<CityLand> land;
	std::vector<CityHousing> housing;
	std::vector<CityGov> city_gov;
	// F8: catálogo de serviços e negócios (um vetor global; índices por cidade
	// e por setor são reconstruídos quando o conjunto muda).
	bool services_loaded = false;
	double service_government_share = 0.002;
	bool sector_loaded = false;
	std::vector<SectorAnchor> sector_anchors;
	// Demanda de trabalho agrícola: t/dia × base / TFP^expoente.
	double agri_labor_per_ton = 55.0;
	double agri_labor_exponent = 1.4486;
	double agri_production_band_min = 0.9;
	double agri_production_band_max = 1.1;
	// Autoconsumo máximo das famílias agrícolas, como fração da produção
	// local disponível — o resto da produção vai ao mercado.
	double agri_own_consumption_share = 0.5;
	// Indústria: trabalhadores por unidade de valor adicionado (XGS/dia).
	double industry_labor_per_va = 3.0;
	double industry_labor_exponent = 1.4486;
	std::vector<ServiceSector> service_sectors;
	std::vector<ServiceBusiness> service_businesses;
	std::vector<std::vector<int>> service_by_city;
	std::vector<std::vector<int>> service_by_sector;
	int next_service_business_id = 1;
	int service_bankruptcies = 0;
	// Índices derivados do mundo demo (ver rebuild_indexes).
	std::unordered_map<std::string, int> city_lookup;
	std::vector<std::vector<int>> buildings_by_city;
	// A2: tabelas de nomes -> índice (ver NameTable). Preenchidas no
	// carregamento por rebuild_indexes(true); o tick não as reconstrói.
	NameTable nation_table;
	NameTable landmass_table;
	// A2: tarifa resolvida por índices inteiros. Chave = (comprador+1)<<42 |
	// (vendedor+1)<<21 | (produto+1); -1 codifica o curinga '*'. Preenchida em
	// rebuild_tariff_lookup(); quando `size()` não bate com `tariff_lookup`
	// (alguma linha não resolvível), tariff_rate cai no caminho textual.
	std::unordered_map<std::uint64_t, double> tariff_by_index;
	// F4: cidades por nação (ordem de índice) para a frota/compra do governo.
	std::unordered_map<std::string, std::vector<int>> cities_by_nation;
	std::vector<Shipment> in_transit;
	std::vector<std::string> invariant_violations;
	// Portos do cenário real (modo mundo) e pedidos de comércio do último tick,
	// para a camada visual (minimapa e comboios 3D).
	std::vector<Port> ports;
	std::vector<TradeOrder> last_orders;
	int bankruptcies = 0;
	// D-126: contadores do dia/partida (status e teste; persistidos no v41).
	int crisis_layoffs = 0;
	double crisis_jobs_cut = 0.0;
	double crisis_severance_paid = 0.0;
	double crisis_loans = 0.0;
	int crisis_renegotiations = 0;
	double crisis_penalty_paid = 0.0;
	int crisis_liquidations = 0;
	double crisis_liquidated_t = 0.0;
	double crisis_liquidated_value = 0.0;
	int crisis_mothballs = 0;
	int crisis_reopens = 0;
	double crisis_reopen_paid = 0.0;
	int crisis_auctions = 0;
	double crisis_auction_value = 0.0;
	int crisis_auctions_unsold = 0;
	int crisis_farm_area_reductions = 0;
	int crisis_power_auctions = 0;
	double crisis_power_auction_value = 0.0;
	// D-099: composição das falências do mundo para o diagnóstico do 365 d
	// (contadores de RUNTIME, não serializados; zeram no load). Por produto
	// (`output_product`) e quantas eram primárias COM plano de safra.
	int bankrupt_by_output[kMaxProducts] = {};
	int bankrupt_planned_primary = 0;
	int depleted_mines = 0;
	double transport_cash = 0.0;
	double foreign_cash = 0.0;
	// Setor de construção: recebe o capital investido em expansão.
	double construction_cash = 0.0;
	double produced[kMaxProducts] = {};
	double consumed_total[kMaxProducts] = {};
	double produced_today[kMaxProducts] = {};
	double consumed_today[kMaxProducts] = {};
	// Produtos ativos (set_goods). 21 por padrão (cenário de validação).
	int products = 21;
	std::vector<std::string> product_names;
	std::unordered_map<std::string, int> product_lookup;
	// Receitas industriais e índice por id (recipes.json).
	std::vector<Recipe> recipes;
	std::unordered_map<std::string, int> recipe_lookup;
	// Cesta alimentar data-driven (needs.json); default = 5 culturas validadas.
	std::vector<int> food_products;
	std::vector<double> food_factors;
	// F3: tabela de necessidades por nível (needs.json), substitutos,
	// equivalência e consumo institucional do governo. `needs_loaded`
	// falso = comportamento de 1 nível (cenário AGRIA/PORTUS e saves antigos).
	std::vector<NeedsLevel> needs_levels;
	bool needs_loaded = false;
	int needs_food_level = -1;
	// Decaimento diário que perde exatamente durable_replacement_per_year em
	// 365 dias: 1 - (1 - r)^(1/365).
	double durable_decay = 0.0;
	std::vector<char> product_durable;  // por produto (goods.json durable)
	std::vector<int> government_products;
	std::vector<double> government_per_capita_day;
	// Bootstrap industrial já executado neste mundo.
	bool industry_seeded = false;
	// F4/F0: frota por nação (ordem determinística em fleet_order) e índices
	// dos bens da frota (diesel/jet_fuel/truck/cargo_ship/cargo_plane/spare_parts).
	std::unordered_map<std::string, Fleet> fleets;
	std::vector<std::string> fleet_order;
	int fleet_product_diesel = -1;
	int fleet_product_jet = -1;
	int fleet_product_truck = -1;
	int fleet_product_ship = -1;
	int fleet_product_plane = -1;
	int fleet_product_parts = -1;
	// F5: moedas por code/nacao, ordem determinística e tarifas declaradas.
	std::vector<Currency> currencies;
	std::unordered_map<std::string, int> currency_by_code;
	std::unordered_map<std::string, int> nation_currency;
	std::vector<Tariff> tariffs;
	std::unordered_map<std::string, double> tariff_lookup;
	// F9: grupos/empresas, política de IDE, incentivos, confiança e fluxos.
	std::vector<Group> groups;
	std::unordered_map<std::string, int> group_lookup;
	bool groups_seeded = false;
	std::vector<FdiPolicy> fdi_policies;
	std::unordered_map<std::string, int> fdi_lookup;
	std::vector<SectorIncentive> incentives;
	std::unordered_map<std::string, int> incentive_lookup;
	std::unordered_map<std::string, double> confidence;
	std::unordered_map<std::string, int> confidence_penalty_day;
	std::unordered_map<std::string, GroupFlow> group_flows;
	// C2a (D-103) — BANCO CENTRAL POR MOEDA (estado + medição).
	// Política do MUNDO: padrão monetário (central_bank::Standard; 0 =
	// fiduciário, o início declarado pelo plano) e a moeda de reserva global
	// (default USD, também declarado pelo plano — não é dado observado).
	int monetary_standard = 0;
	std::string reserve_currency = "USD";
	// Reserva de ouro do BC por CÓDIGO de moeda (t de ouro fino). Vem do dado
	// real da G2/D-101 (`strategic_reserves.json`) por `set_central_bank_gold`;
	// é ativo REAL (não entra na identidade financeira). `declared` falso =
	// nenhuma reserva declarada ainda (save v26, mundo sem a G2 aplicada).
	std::map<std::string, double> reserve_gold_tonnes;
	bool reserve_gold_declared = false;
	double reserve_gold_unallocated = 0.0;  // nação sem moeda na tabela
	// Série mensal do CPI por nação (medição) e o dia da última amostra.
	std::map<std::string, CpiHistory> cpi_history;
	int cpi_last_observe_day = -1;
	// C2b1 (D-104) — POLÍTICA MONETÁRIA: um estado por MOEDA (a mesma entidade
	// por moeda da C2a), a dívida pública por nação e o M0 criado no dia.
	std::map<std::string, MonetaryState> monetary;
	std::map<std::string, double> gov_bonds;
	// C6 (D-114): títulos públicos emitidos no resgate de um banco SEM banco
	// central (dolarizado/sem moeda própria, ou moeda sem estado monetário
	// salvo). Não há BC para carregar o papel: ele fica em poder do SETOR
	// EXTERIOR, pago pela posição externa (`foreign_cash`). O título é o claim
	// do credor e entra em `Ledger.external_claims`, fechando a identidade da
	// C1 (sem ele, `financial_sum = −valor`). Save v34.
	std::map<std::string, double> external_bonds;
	// C2b2 (D-106) — CÂMBIO: estado por moeda (paridade, banda, reservas,
	// conversão do ouro), o anel de eventos do status e os parâmetros de regra
	// (largura da banda, fatia de defesa, cobertura do padrão ouro; default de
	// jogo em data/definitions/economy/exchange_policy.json). Save v29.
	std::map<std::string, FxState> fx;
	std::vector<FxEvent> fx_events;
	double fx_band_width = 0.0;        // 0 = default (kBandWidthDefault)
	double fx_defence_share = 0.0;     // 0 = default (kDefenceShareDefault)
	double fx_gold_reserve_ratio = 0.0;// 0 = default (kGoldReserveRatioDefault)
	// C2c (D-108) — REGIME REAL por moeda (IMF AREAER 30/04/2026), ancora
	// (moeda ou cesta), emissor e trava de uniao monetaria. Save v31; mundo
	// sem o dado continua com o regime que veio no save (nada e inventado).
	std::map<std::string, FxRegimeInfo> fx_regime;
	std::string fx_regime_source;              // ex.: "IMF AREAER 2026 (30/04/2026)"
	std::vector<std::string> fx_no_policy_nations;  // dolarizados/sem moeda propria
	double emission_today = 0.0;
	// C4 (D-110): dinheiro BANCÁRIO criado no dia (variação dos depósitos na
	// fase de crédito; positiva ao emprestar/dividendar, negativa ao receber
	// principal/juro e no calote). O BC só cria M0: a conservação do dia vira
	// `money_after = money_before + emission_today + bank_money_today`.
	double bank_money_today = 0.0;
	// D-126: soma ASSINADA do dinheiro bancário criado/destruído (M1) em todos
	// os dias VÁLIDOS desde o dia 0. `money_total` pode crescer legitimamente
	// pelo crédito de giro; os guardas de conservação comparam o fim com
	// `inicial + emissão + bank_money_delta_total`, não com o inicial puro.
	double bank_money_delta_total = 0.0;
	// C3 (D-107) - CRÉDITO DE EMPRESA: banco agregado por nação e as regras do
	// crédito. `bank_credit_enabled` é ESTADO (vai ao save: pode ser ligado por
	// comando A3); os parâmetros abaixo são REGRA (vêm de
	// data/definitions/economy/bank_credit.json, como os do câmbio/emissão) e
	// 0 significa "usar o default declarado em sim/bank.h". LIGADO no mundo
	// novo desde a D-111 (a C4 criou o credor); save antigo sem o bloco do
	// banco continua carregando desligado.
	std::map<std::string, Bank> banks;
	bool bank_credit_enabled = true;	double bank_spread = 0.0;         // 0 = kSpreadDefault (2 p.p.)
	double bank_leverage_cap = 0.0;   // 0 = kLeverageCapDefault (1,0)
	double bank_interest_cover = 0.0; // 0 = kInterestCoverDefault (2,0)
	int bank_default_days = 0;        // 0 = kDefaultDaysDefault (30)
	double bank_repay_share = 0.0;    // 0 = kRepayShareDefault (0,5)
	// C6 (D-114) - CRISE BANCARIA: regras declaradas (bank_crisis.json, espelho
	// em sim/bank_crisis.h), o fundo de seguro por nacao e o anel de eventos.
	// `enabled` e estado (save v34); o resto e REGRA (0 = default declarado).
	// O fundo entra no `money_total` (premio/pagamento conservam dinheiro).
	BankCrisisPolicy bank_crisis;
	std::map<std::string, DepositInsurance> deposit_insurance;
	std::vector<BankCrisisEvent> bank_crisis_events;  // anel de 64
	int bank_crisis_events_total = 0;                // contador total (save)
	// C5 (D-112) - BOLSA: uma praça por nação e um papel por grupo listado.
	// `stock_enabled` é ESTADO (vai ao save); os demais são REGRA (0 = default
	// declarado em sim/stock.h). O canal de investimento (q de Tobin) é uma
	// alavanca de laboratório para o A/B do aceite, não um número de jogo.
	std::map<std::string, StockExchange> stock_exchanges;
	std::map<std::string, StockTicker> stock_tickers;
	bool stock_enabled = true;
	bool stock_investment_channel = true;
	double stock_risk_premium = 0.0;   // 0 = kRiskPremiumDefault (4 %)
	double stock_payout_ratio = 0.0;   // 0 = kPayoutRatioDefault (0,5)
	double stock_step = 0.0;           // 0 = kStepDefault (0,35/semana)
	double stock_expectation_k = 0.0;  // 0 = kExpectationKDefault (5 %/dia)
	// Caixa do governo por nação na ABERTURA do dia: a medição do déficit
	// (runtime; não vai ao save).
	std::map<std::string, double> gov_cash_open;
	// Setor de cada produto (goods.json → sector). Vazio = "primary".
	std::vector<std::string> product_sectors;
	// ------------------------------------------------------------------
	// PARTE E / D-116 — ENERGIA (save v35). Ver sim/power.{h,cpp}.
	// `power_cities` é paralelo a `cities`; `power_plants` guarda o parque
	// inicial (semeado do dado) e as usinas construídas; `power_grids` é a
	// rede por nação (só existe onde há operador nacional). `power_cash` é o
	// caixa agregado do setor elétrico (dinheiro do mundo, soma zero).
	// ------------------------------------------------------------------
	std::vector<PowerCityState> power_cities;
	std::vector<PowerPlant> power_plants;
	std::map<std::string, PowerGridState> power_grids;
	// E5 (D-117): interconexões entre redes nacionais (ano-base + obras).
	std::vector<InterconnectState> interconnects;
	bool power_interconnects_ready = false;
	bool power_ready = false;
	double power_cash = 0.0;
	// REGRA (não persistida): espelho de data/definitions/energy/power_params.json.
	PowerParams power_params;
	// Preços-base dos bens (data-driven: set_goods/initialize_world).
	double base_prices[kMaxProducts] = {
		40.0, 60.0, 55.0, 42.0, 15.0, 25.0, 3000.0, 10.0, 200.0, 400.0,
		15.0, 5000.0, 6.0, 60.0, 30.0, 45.0, 35.0, 45.0, 70.0, 25.0, 500.0};
	// Retrato do tick reaproveitado entre dias (ver tick_core).
	Snapshot snapshot_buffer;
};

} // namespace ogs

