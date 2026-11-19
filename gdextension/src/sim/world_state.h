#pragma once
// Estado da simulação econômica (S5a-2 do docs/economy/core_refactor_plan.md).
// C++ puro (só STL, via sim_types.h) — nada de godot_cpp. É o "WorldState" que
// o SimCore (sim_core.*) recebe e muta a cada tick; a fachada EconomySim
// (economy_sim.*) guarda UM `WorldState ws_` e converte Godot <-> sim.

#include "sim_constants.h"
#include "sim_types.h"
#include "urban_types.h"

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

struct WorldState {
	int day = 0;
	CadenceState cadence;

	// Estado do cenário de validação de duas cidades.
	bool active = false;
	// true quando o demo foi inicializado a partir do mundo real
	// (initialize_world), em vez do cenário AGRIA/PORTUS. Ativa o comércio
	// genérico entre cidades no lugar dos blocos específicos de validação.
	bool world_mode = false;
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
	// F4: frota por nação (ordem determinística em fleet_order) e índices
	// dos bens da frota (fuel/truck/cargo_ship/cargo_plane/spare_parts).
	std::unordered_map<std::string, Fleet> fleets;
	std::vector<std::string> fleet_order;
	int fleet_product_fuel = -1;
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
	// Setor de cada produto (goods.json → sector). Vazio = "primary".
	std::vector<std::string> product_sectors;
	// Preços-base dos bens (data-driven: set_goods/initialize_world).
	double base_prices[kMaxProducts] = {
		40.0, 60.0, 55.0, 42.0, 15.0, 25.0, 3000.0, 10.0, 200.0, 400.0,
		15.0, 5000.0, 6.0, 60.0, 30.0, 45.0, 35.0, 45.0, 70.0, 25.0, 500.0};
	// Retrato do tick reaproveitado entre dias (ver tick_core).
	Snapshot snapshot_buffer;
};

} // namespace ogs
