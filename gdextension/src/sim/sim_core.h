#pragma once
// Núcleo da simulação econômica (S5b do docs/economy/core_refactor_plan.md).
// C++ puro (só STL, via world_state.h/sim_types.h) — nada de godot_cpp.
//
// A classe `ogs::SimCore` recebe o `WorldState&` na construção e executa a
// ordem do dia: um prólogo (retrato, dinheiro, dia, obras, índices, preços de
// abertura) seguido de uma fase por etapa econômica. A fachada `EconomySim`
// (economy_sim.*) guarda UM `WorldState ws_` e UM `SimCore sim_core_{ws_}`,
// converte Godot <-> sim e loga os invariantes (o núcleo não emite push_error).
//
// S5b é fatiado: a cada commit um grupo de fases migra de `EconomySim` para
// cá. As fases que ainda não migraram continuam inline no `tick_core`.

#include "sim_labor.h"
#include "thread_pool.h"
#include "world_state.h"

#include <memory>

namespace ogs {

// Token seguro para id determinístico (letras/números; o resto vira '-').
inline std::string sane_token(const std::string& text) {
	std::string out;
	out.reserve(text.size());
	for (const char c : text) {
		const unsigned char u = static_cast<unsigned char>(c);
		const bool ok = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')
			|| (u >= '0' && u <= '9');
		out.push_back(ok ? c : '-');
	}
	while (!out.empty() && out.back() == '-') {
		out.pop_back();
	}
	return out.empty() ? std::string("X") : out;
}

// S16 — buffer de redução por cidade para as fases de renda/consumo. As
// contribuições de cada cidade aos acumuladores globais (`foreign_cash`,
// `consumed_total`, `consumed_today`) são gravadas em slots disjuntos
// (índice da cidade × produto) e reduzidas em ordem crescente de cidade numa
// passada serial — reproduz a ordem de soma do loop serial original.
struct CityAccumScratch {
	std::vector<double> foreign;          // por cidade (saídas p/ o exterior)
	std::vector<double> consumed_total;   // cidade × produto
	std::vector<double> consumed_today;   // cidade × produto
	std::size_t n_products = 0;
};

class SimCore {
public:
	explicit SimCore(WorldState& ws);

	// ------------------------------------------------------------------
	// S16 — paralelismo determinístico por cidade.
	// ------------------------------------------------------------------
	// Define o número de threads das fases por cidade: 1 = serial (default);
	// <= 0 = automático (min(núcleos - 2, 8)); > 1 = esse número. As fases
	// globais (trade, fiscal, câmbio, invariantes) seguem seriais.
	void set_sim_threads(int threads);
	int sim_threads() const { return sim_threads_; }

	// ------------------------------------------------------------------
	// Fases da ordem do dia (crescem a cada fatia de S5b).
	// ------------------------------------------------------------------
	// Prólogo: retrato para reversão, dinheiro antes, avanço do dia, obras de
	// porto, remoção de falidos/esgotados, índices e preços de abertura.
	void prologue();
	// 1. Chegada das cargas pedidas no dia anterior.
	void phase_arrival();
	// 2. Zera os fluxos do dia.
	void phase_reset();
	// 2b-G. Agricultura sazonal (sim/agriculture.cpp): plantio/fertilizante,
	// colheita DENTRO da janela (fora dela o fluxo é zero), perda de
	// armazenagem mensal e perecibilidade dos bens da cadeia. Roda antes da
	// produção: a safra do dia já está no estoque quando o comércio abre.
	void phase_agriculture();
	// D-125: eficiência de produção (transição sem produção, combustível do
	// equipamento e maturação da planta). Roda depois do reset e antes da
	// agricultura/produção: a safra e a produção do dia já usam a eficiência
	// atualizada.
	void phase_efficiency();
	// 2a. B1: demografia — cadência mensal (A6) por cidade. Move as coortes
	// etárias um mês (nascimentos, mortes e envelhecimento) e reaplica o perfil
	// que depende da idade. Laço SERIAL (determinístico).
	void phase_demography();
	// 2b. F4: manutenção/compra mensal da frota e zera a fila do dia. Roda
	// antes do comércio para combustível/veículos comprados já valerem hoje.
	void phase_fleet();
	// 3. Mercado de trabalho e salários.
	void phase_labor();
	// 4. Produção: cada construção declara insumo e produto.
	void phase_production();
	// 4c. Recursos brutos (minerais, soja, algodão e madeira): exportação.
	void phase_raw();
	// 5. Renda das famílias, impostos, transferências e consumo alimentar.
	void phase_income();
	// 5b. §19 W3: mercado de moradia (aluguel, construção e aperto). Roda
	// depois da comida (a POP já comprou a cesta) e antes de serviços/comércio:
	// o aluguel só usa a poupança acima da reserva de comida.
	void phase_housing();
	// F8. Serviços: demanda das classes, B2B, governo, exportação, produção dos
	// negócios, preço, entrada e saída — uma cidade por vez.
	void phase_services();
	// 6. Comércio de alimento (cesta de calorias) e dos demais bens.
	void phase_trade();
	// 7. Receita por construção, lucro, distribuição de capital e PIB.
	void phase_accounting();
	// 8. Preços reagem a estoque, oferta, demanda e custo de produção.
	void phase_prices();
	// 9. Limite diário único por mercado (depois de todas as regras de preço).
	void phase_clamp();
	// 9b. Redistribuição fiscal nacional + dividendos/repatriação dos grupos.
	void phase_fiscal();
	// 9c. Câmbio dinâmico: a balança comercial do dia move as moedas float.
	void phase_fx();
	// 9d. C2b1: taxa básica por moeda e emissão (compra de títulos do governo
	// pelo BC controlado), com o déficit do dia medido no caixa do governo.
	void phase_monetary();
	// 9e. C3 (D-107): crédito de empresa — juro, amortização, juros vencidos e
	// calote do banco agregado (perda no credor). Roda depois da política
	// monetária (a taxa do dia já existe) e antes dos invariantes.
	void phase_credit();
	// D-126: reacao deterministica a crise das firmas, servicos e usinas.
	void phase_crisis();
	// PARTE E (D-116): despacho por mérito, preço da rede e racionamento.
	// Antes do trabalho/produção: define o fator de energia de cada cidade
	// (indústria/serviços/insatisfação). `phase_power_settle` roda depois da
	// contabilidade e liquida as contas (consumidor → gerador, combustível,
	// O&M, salários e lucro do dono), sempre com soma zero.
	void phase_power();
	void phase_power_settle();
	// 10. Invariantes: se o tick corrompeu o estado, reverte o retrato.
	void phase_invariants();

	// ------------------------------------------------------------------
	// Helpers compartilhados com a fachada (públicos).
	// ------------------------------------------------------------------
	City* find_city(const std::string& id);
	const City* find_city(const std::string& id) const;
	// R1a - caixa unico regional. Liga/desliga a flag em `ws_.regional_cash` e
	// aplica a todas as cidades (ao ligar, `private_cash` nasce da soma dos
	// dois caixas; ao desligar, volta a 0). Chamado no fim de initialize_world
	// e pelo setter de definicoes; idempotente.
	void set_regional_cash(bool enabled);
	// Depois do load: a flag do save (ou a configuracao) manda; cidade que veio
	// sem `private_cash` e sincronizada a partir dos dois espelhos.
	void apply_regional_cash_after_load(bool saved_flag, bool has_saved_flag);
	// Teto físico de capacidade agrícola (t/dia) de uma cidade para um produto.
	// Dado de GEOGRAFIA: não é serializado no save — chega do GeographyService
	// e viaja no bloco `definitions` do snapshot. Produto fora de
	// [0, ws_.products) ou cidade ausente é ignorado; valor é limitado a >= 0.
	void set_land_capacity(const std::string& city_id, int product, double value);

	// ------------------------------------------------------------------
	// PARTE E (D-116) — ENERGIA (sim/power.cpp). API de DECLARAÇÃO (antes do
	// dia 1 / depois do load) e de OBRA (fila §15). Nada aqui inventa número:
	// payload vazio = sem energia (ausência declarada).
	// ------------------------------------------------------------------
	void set_power_definitions(const PowerParams& params,
		const std::map<std::string, PowerNationDef>& nations,
		const std::map<std::string, PowerCityDef>& cities, bool only_missing);
	// Sobrescreve parâmetros de regra pelo nome do campo (laboratório).
	void set_power_params(const std::map<std::string, double>& values);
	// Teste/depuração: força o estado de conexão de uma cidade (não é atalho
	// de jogo: o caminho de jogo é a obra `build_power_line`).
	void set_power_city(const std::string& city_id, int connection,
		double line_mw, double access_pct);
	// Obra de usina/linha na fila §15 (paga pelo dono conforme avança). Devolve
	// false com `err` quando a fonte/cidade não existe ou o potencial acabou.
	// `instant` = hook de teste/depuração (mesmo `set_instant_construction`):
	// a usina entra em operação na hora, com o custo integral já debitado.
	bool build_power_plant(const std::string& city_id, const std::string& fuel,
		double mw, const std::string& group_id, std::string* err,
		bool instant = false);
	bool build_power_line(const std::string& city_id, double mw,
		const std::string& group_id, std::string* err);
	// E5 (D-117): declara as interconexões do ano-base (dado derivado) e a obra
	// de uma linha nova entre duas REDES NACIONAIS. A obra paga pelo caixa do
	// dono (grupo ou governo da maior cidade conectada de `nation_a`), consome
	// material no mercado dessa cidade e, ao concluir, soma capacidade ao par.
	void set_power_interconnects(const std::vector<InterconnectDef>& defs,
		bool only_missing);
	bool build_interconnect(const std::string& nation_a,
		const std::string& nation_b, double mw, const std::string& group_id,
		std::string* err);

	// ------------------------------------------------------------------
	// G — AGRICULTURA SAZONAL (sim/agriculture.cpp, D-096).
	// API de DECLARAÇÃO (antes do dia 1, como set_land_capacity): o dado é
	// reaplicado no load pelo `AgricultureService` / bloco `definitions`.
	// ------------------------------------------------------------------
	// Declara (ou substitui) o plano de safra de (cidade, produto): área
	// colhida em ha, rendimento t/ha e 1..kMaxCropSeasons janelas em dia do
	// ano com a fração do orçamento anual. Cidade/produto ausentes, área ou
	// rendimento <= 0 ou nenhuma janela válida = SEM plano (a produção diária
	// da fazenda continua; nada de safra vazia).
	void set_crop_plan(const std::string& city_id, int product, double area_ha,
		double yield_t_ha, const std::vector<CropSeasonPlan>& seasons);
	// Estoque inicial (t) de (cidade, produto) no dia 1: ASSIGNA (não soma),
	// para a chamada ser idempotente. Vem do `ending_stocks_t` observado ou da
	// regra de 60 dias de consumo do próprio dado (nunca de um número
	// inventado). Valor negativo é ignorado.
	void set_crop_initial_stock(const std::string& city_id, int product,
		double tons);
	// Perda pós-colheita do produto (% do que entra) e a fatia de armazenagem
	// declarada (% do total, ou < 0 = sem dado). Produto desconhecido é
	// ignorado; percentuais ficam em [0, 100].
	void set_crop_loss(int product, double loss_pct, double storage_share_pct);
	// Prazo de prateleira (dias) do bem: o estoque parado perde metade em
	// `days` dias. `days <= 0` = sem dado (não decai) mas o bem entra na lista
	// da cadeia.
	void set_shelf_life(int product, int days);
	// Fertilizante nitrogenado (kg N/ha) da cultura na cidade. Só tem efeito
	// quando o plano daquele (cidade, produto) já foi declarado.
	void set_fertilizer_n(const std::string& city_id, int product,
		double n_kg_ha);
	// Qualidade do armazém 0..1 (hook de infraestrutura). Sem chamada, a
	// qualidade é DERIVADA da infra B5 do estado (`city_storage_quality`).
	void set_storage_quality(const std::string& city_id, double quality);
	// Refaz o índice esparso (cidade × produto) dos planos ativos. Chamado
	// pelos setters e no fim do load; o tick só LÊ.
	void rebuild_crop_indexes();
	// Plano sazonal ativo para (índice de cidade, produto)? A produção diária
	// da fazenda é SUPRIMIDA nesse caso (sem contagem dupla).
	bool crop_planned_at(std::size_t city_index, int product) const;
	// D-096 — a fazenda sazonal TRABALHA hoje? `true` para todo par sem plano
	// (comportamento anterior) e, com plano, o bit de plantio/colheita do dia:
	// fora das duas janelas a equipe é liberada e o par não paga folha/opex.
	bool crop_working_at(std::size_t city_index, int product) const;
	// D-099: valor (a `price`) da safra AINDA NÃO ENTREGUE do plano ativo de
	// (cidade, produto) — lavoura em pé + parcela da colheita em curso que
	// falta entregar. 0 para par sem plano. Ativo da fazenda na carência de
	// falência; não cria dinheiro (é avaliação, não venda).
	double crop_standing_value(const std::string& city_id,
		int product, double price) const;
	// D-099 (folha da safra): fator 0..1 aplicado à folha+opex de uma fazenda
	// primária com plano quando o custo diário cheio excede o que a safra
	// (estoque + lavoura em pé, na fatia da firma) paga em kLossDaysBeforeClose
	// dias. 1.0 para todo o resto. A folha devida é REDUZIDA (nada é criado).
	double crop_liquidity_factor(const City& city, const Building& building,
		double daily_cost) const;
	// Qualidade do armazém efetiva da cidade (explícita ou derivada da infra).
	static double city_storage_quality(const City& city);

	// ------------------------------------------------------------------
	// G2 — ARMAZENAGEM (sim/storage.cpp, D-100). API de DECLARAÇÃO (antes do
	// dia 1, como os set_* da geografia/agricultura): capacidade por cidade,
	// tipo e dono e o mapeamento produto→tipo do JSON da G2. O estado (estoque
	// guardado e capacidade instalada) é do save v25.
	// ------------------------------------------------------------------
	// ASSIGNA a capacidade (t) do armazém (cidade, tipo, dono). `scope`: 0 =
	// governo, 1 = privado. Tipo inválido/cidade ausente é ignorado; tipo de
	// estrutura (cofre/paiol) fica com capacidade 0 nesta parte.
	void set_storage_capacity(const std::string& city_id, int type, int scope,
		double tons);
	// D-101: ASSIGNA o estoque GUARDADO (t) de um produto no armazem
	// (cidade, tipo, dono) - a semente do dia 1 da reserva estrategica do
	// JSON (petroleo publico, graos publicos). So vale para tipo operacional
	// que aceita o produto do mapeamento; cofre/paiol nao guardam estoque.
	// Nada e criado nem destruido: o valor substitui o que existe naquele
	// produto e o chamador preserva o total (conservacao).
	void set_storage_stock(const std::string& city_id, int type, int scope,
		int product, double tons);
	// Override do tipo de um produto (carregador do JSON de mapeamento). Sem
	// chamada vale a tabela declarada por nome em storage.cpp.
	void set_storage_product_type(int product, int type);
	void rebuild_storage_indexes();
	// Fase diária: pulmão região→cidade, perdas, manutenção e liberação com
	// custo de acesso. Roda logo depois de `phase_agriculture`.
	void phase_storage();
	// A2: `full=false` é o caminho de todo tick (só o índice por cidade;
	// cidades/nações/tarifas só mudam no carregamento).
	void rebuild_indexes(bool full = true);
	double money_total() const;
	// C4 (D-110): M0 = money_total − depósitos à vista (M1 = money_total).
	double m0_total() const;
	void init_pop_needs(Pop& pop) const;
	static double extraction_efficiency(const Deposit& deposit);
	double base_price(int product) const;
	// D-092: fração da necessidade que a demanda compra ao preço corrente
	// (elasticidade-preço por classe). 1.0 até o base; cai como
	// (preço/base)^-elasticidade acima dele, com piso declarado.
	static double price_demand_scale(double price, double base,
		double elasticity);
	int food_product(int slot) const;
	double food_calories(int product) const;
	// Multiplicador do opex pela profundidade da jazida (1.0 sem reserva).
	static double depth_opex_factor(const Deposit& deposit);
	// D-086 — produção relativa do ESTÁGIO de uma mina (0,05..1,0). 1,0 para
	// construção que não é mina (fazenda, indústria, produto sem jazida).
	// A curva é linear na reserva que o estágio ainda pode extrair:
	// `clamp(stage_remaining / stage_reserve, piso, 1)`.
	static double mine_stage_factor(const City& city, const Building& building);
	// true quando a construção é uma mina/poço de estágio: extração primária de
	// produto com jazida rastreada na cidade.
	static bool is_mine_building(const City& city, const Building& building);
	// Capacidade adicionada pelo próximo aprofundamento (t/dia).
	static double mine_deepen_capacity(const Building& building);
	// Custo (XGS) e prazo (dias) do aprofundamento que abriria
	// `building.stage + 1`. A profundidade e a qualidade da jazida entram no
	// custo; o estágio novo faz o custo crescer (mais fundo = mais caro).
	double mine_deepen_cost(const City& city, const Building& building) const;
	static int mine_deepen_days(const Building& building);
	// Abre o estágio `stage` numa mina: ele reivindica uma fatia da reserva
	// RESTANTE da jazida e começa cheio (produção 100% enquanto ela durar).
	static void set_mine_stage(City& city, Building& building, int stage);

	// F7 calibração: TFP efetiva da cidade (fator explícito ou derivado do
	// PIB PPP per capita real).
	double effective_productivity(const City& city) const;
	// F7: escolaridade das vagas por setor industrial (fallback da receita).
	static void education_mix_for_sector(const std::string& sector,
		double* mix);
	// F9: âncoras ILO interpoladas no log do índice de renda.
	void sector_anchor_values(double income, double* out) const;
	// F9: decompõe o emprego do dia em agro/indústria/serviços + informalidade.
	void update_sector_employment(City& city);
	// F7 calibração: recalcula os estratos de renda a partir da escolaridade.
	void refresh_strata(Pop& pop) const;
	// ------------------------------------------------------------------
	// B2 — ESTRATOS DE RENDA (sim/strata.cpp).
	// ------------------------------------------------------------------
	// Semeia os 5 quintis de uma cidade: população (20% cada), composição
	// educacional, dispersão salarial, participação no capital, posse de
	// moradia e patrimônio. `shares` (5) = quintis de renda observados (soma
	// ~1); `nullptr` = proxy lognormal do Gini declarado. Idempotente.
	void seed_strata(City& city, const double* shares);
	// Recalcula a população dos quintis a partir da população da cidade
	// (demografia muda o total; o mix de quintis é preservado).
	void rescale_strata_population(Pop& pop) const;
	// Reparte a renda do dia nos quintis (salário × composição educacional ×
	// dispersão, capital/aluguel pelos donos, transferência per capita). Soma
	// exatamente os totais da cidade (conservação por construção).
	void refresh_strata_income(City& city);
	// B7: calcula a renda de mercado dos quintis (com penalidade informal) e o
	// imposto total (só a parcela FORMAL do salário paga). Chamado no início da
	// fase de renda, antes de transferências/disponível/gasto em serviços.
	void refresh_strata_taxes(City& city);
	// Mobilidade mensal determinística entre quintis adjacentes (A6).
	void phase_strata();
	// Patrimônio mensal: poupança + imóvel (posse × preço §19) + participações.
	void update_strata_wealth(City& city);
	// Gini de renda dos quintis (0..1); 0 se não há estrato semeado.
	double strata_gini(const Pop& pop) const;
	// Taxa de poupança efetiva do estrato hoje (quintil × ciclo de vida).
	void refresh_saving_rates(City& city);
	// Cobrança/credito de dinheiro das famílias (fases posteriores à renda:
	// aluguel, importação, serviços) rateado pela poupança dos quintis — a
	// soma dos estratos acompanha `pop.savings` por construção.
	void charge_households(Pop& pop, double amount) const;
	void credit_households(Pop& pop, double amount) const;

	// ------------------------------------------------------------------
	// B3 — REGIÃO (estados inteiros), bloco simplificado do interior.
	// ------------------------------------------------------------------
	// Semeia/reaplica a demografia do interior (5 coortes da estrutura
	// etária nacional). Idempotente; sem dado para a nação NÃO faz nada.
	void seed_region_demography(City& city);
	// Deriva os totais do interior a partir das coortes (region_pop = soma).
	void apply_region_demography(City& city);
	// Semeia os 5 quintis do interior: 20% de pessoas cada; `shares` (5) são
	// os quintis de renda observados (ou nullptr = 20% iguais — ausência
	// explícita). Idempotente.
	void seed_region_strata(City& city, const double* shares);
	// Recalcula o mix de emprego do interior a partir de `urban_share`
	// (fórmula declarada em sim_constants.h). Sem dado, mix neutro.
	void recompute_region_mix(City& city);
	// Semeia o bloco inteiro (demografia + estratos + mix) e reconcilia a
	// população exibida com o invariante urbano + interior.
	void seed_region(City& city);
	// Reconcilia `city.population` com o invariante B3:
	// urbano (simulation_population) + interior (region.region_pop).
	void reconcile_city_population(City& city);
	// Fase diária da região (cadência A6): consumo diário, emprego/salário/
	// renda semanais e estratos/poupança mensais. Implementada em region.cpp.
	void phase_region();

	// ------------------------------------------------------------------
	// B4 — MIGRAÇÃO (êxodo rural → polo urbano + cidade → cidade na mesma
	// nação). Fase MENSAL (cadência A6, mesma agenda da B3): cada cidade roda no
	// seu dia. Conserva pessoas (Σ urbano + Σ região invariante) e dinheiro
	// (poupança acompanha o migrante). Implementada em migration.cpp.
	// ------------------------------------------------------------------
	void phase_migration();

	// ------------------------------------------------------------------
	// B5 — INFRAESTRUTURA DA REGIÃO (estrada, energia, água). Multiplicadores
	// derivados da cobertura (0..kInfraMaxLevel), implementados em region.cpp.
	// Nenhum cria dinheiro: o custo de acesso é frete (vai ao transporte), a
	// produtividade só escala o salário já pago pelo caixa da firma e o êxodo
	// só escala o fluxo de PESSOAS (conservado).
	// ------------------------------------------------------------------
	// Nível total (0..kInfraTypeCount*kInfraMaxLevel) dos três tipos.
	static int infra_total_level(const Region& region);
	// Multiplicador de custo de acesso região↔polo (>= 1; cai com a estrada).
	// `penalty_max` é a penalidade máxima (estrada 0): o padrão é a constante de
	// balanceamento e o WorldState só a sobrescreve no A/B de laboratório.
	static double infra_access_multiplier(const Region& region,
		double penalty_max = kInfraAccessPenaltyMax);
	// Multiplicador de produtividade do trabalho primário (>= 1; 1 na base).
	static double infra_productivity_multiplier(const Region& region);
	// Multiplicador de êxodo (>= 1; 1 com infra máxima, > 1 com infra baixa).
	static double infra_exodus_multiplier(const Region& region);
	// Nível atual de um tipo de infra (0..kInfraMaxLevel); -1 se tipo inválido.
	static int infra_level(const Region& region, int type);
	// Sobe UM nível do tipo (saturado em kInfraMaxLevel). Devolve o novo nível.
	static int infra_level_up(Region& region, int type);

	// ------------------------------------------------------------------
	// B7 — INFORMALIDADE, SERVIÇOS INFORMAIS, MORTALIDADE E INSATISFAÇÃO
	// (sim/informality.cpp). O DADO é a taxa informal por nação e a cobertura
	// de saúde (definições no WorldState); o resto é MODELO declarado.
	// ------------------------------------------------------------------
	// Fração informal INICIAL da nação (dado ou âncora de renda ILO).
	double nation_informal_base(const City& city) const;
	// Cobertura de saúde de referência da nação (dado ou kHealthAccessDefault).
	double nation_health_access(const City& city) const;
	// Semeia a informalidade (urbano + interior) e a referência de saúde a
	// partir do dado nacional. Idempotente (guarda `informality_seeded`).
	void seed_informality(City& city);
	// Recalcula o agregado `informal_share` a partir dos quintis (peso por
	// pessoa). Preserva a soma/determinismo.
	void refresh_informal_aggregate(Pop& pop) const;
	void refresh_informal_aggregate(Region& region) const;
	// Formalização mensal (cadência A6) do trabalho informal, urbano e interior.
	void formalize_informality(City& city);
	// Serviços informais agregados (sem firma): desvia uma fatia do gasto
	// doméstico em serviços para os trabalhadores informais (crédito nos
	// quintis). Devolve a receita desviada (o chamador reduz o pool formal).
	double compute_informal_services(City& city);
	void compute_region_informal_services(City& city);
	// Mortalidade mensal ligada a fome/saúde (urbano e interior). Preenche
	// `death_band` (mortes efetivas por faixa, já moduladas), atualiza os
	// contadores (deaths_expected/total, excess_deaths_*, famine_months) e
	// devolve as mortes base da tabela. O urbano usa `satisfaction_prev`; a
	// região usa a satisfação fresca do dia.
	double mortality_month(Pop& pop, const City& city, double* death_band) const;
	double region_mortality_month(Region& region, const City& city,
		double* death_band) const;
	// Insatisfação (medição 0..1) por quintil/cidade/região, calculada no fim
	// do dia (fase `phase_social`).
	void compute_dissatisfaction(City& city);
	// Fase social no fim do dia: insatisfação (medição pura, sem dinheiro/pessoas).
	void phase_social();

	// ------------------------------------------------------------------
	// B6 — PATRIMÔNIO IMOBILIÁRIO DA REGIÃO (sim/property.cpp). Terra
	// produtiva, moradia do interior e anel periurbano da cidade-polo.
	// Cadência MENSAL (A6). Valor de ativo NÃO é dinheiro: o único fluxo é o
	// aluguel do anel (débito e crédito dentro da região). D-095,
	// docs/economy/B6_region_property.md.
	// ------------------------------------------------------------------
	// Fase mensal: valoriza a terra (renda primária × infra B5 × commodities),
	// fecha preço/aluguel da moradia e do anel, cobra o aluguel do anel
	// (conservado), calcula a receita potencial do imposto e publica a
	// distribuição por quintil + patrimônio. Laço SERIAL determinístico.
	void phase_property();
	// Semeia os hectares e o Gini fundiário (dado + regra de rateio aplicada no
	// GDScript). Idempotente: só age quando `property_ready` é falso — um save
	// v23 carrega o estado gravado. Sem hectares > 0 não faz nada (ausência).
	void seed_region_property(City& city, double land_ha, double land_gini,
		bool gini_observed);
	// Valor de mercado dos ativos da região hoje (terra + moradia + anel).
	double region_property_value(const Region& region) const;
	// Vagas (pessoas/mês) do anel periurbano quando o polo está cheio: fração
	// declarada da população urbana. 0 sem dado de propriedade (B4 preservada
	// em save/cenário sem a B6).
	double ring_vacancy_people(const City& city) const;
	// Custo de moradia DIÁRIO por pessoa de quem vai para o anel (insumo do
	// diferencial de utilidade da B4). 0 sem dado.
	double ring_home_cost_per_day(const City& city) const;
	// Índice de commodities da região (preço ÷ base D0 dos primários da
	// cidade-polo, ponderado pela capacidade); 1.0 sem produtor primário.
	double region_commodity_index(const City& city) const;
	// Multiplicador de valor da terra pela infra B5 (>= 1; 1 sem cobertura).
	static double property_infra_multiplier(const Region& region);
	// Gini de uma distribuição por quintil (0..1); 0 sem massa.
	static double quintile_gini(const double* values, int count);
	// Crédito/débito no caixa da região repartidos pela renda/poupança dos
	// quintis (B3). Públicos para a B6 cobrar o aluguel do anel com a MESMA
	// regra de repartição do resto da região (o par soma zero: dinheiro
	// conservado).
	void region_credit(Region& region, double amount);
	void region_charge(Region& region, double amount);

	// F7: fecha o dia do mercado de trabalho de uma cidade.
	double labor_day(City& city);
	// F7 — custo diário da cesta completa de um nível de necessidade aos preços
	// que a cidade pratica, por substituto não-durável mais barato.
	double level_basket_cost(const City& city, int level_index) const;
	// F7 comida — custo efetivo da necessidade alimentar do dia: estoque local
	// na proporção da compra mais o resíduo ao CIF conhecido.
	void food_basket_cost(const City& city, double need,
		double& local_cost, double& import_cost, double& per_ge) const;
	// F3/F7: substituto preferido da categoria (sinal de desejo no mercado).
	bool category_top_market(City& city, const NeedsCategory& category,
		Market*& market, double& factor, double& durable_scale);
	// F3: registra o desejo da categoria sem comprar (sinal de escassez).
	void register_category_demand(City& city,
		const NeedsCategory& category, double want_ge);
	// F3: consome uma categoria de necessidade e debita o orçamento do dia.
	// S16: grava as compras no scratch de redução por cidade (não no acumulador
	// global) para a fase de renda rodar paralela e determinística.
	double consume_category(City& city, const NeedsCategory& category,
		double want_ge, double& budget, double& spend,
		CityAccumScratch& scratch, std::size_t city_index,
		double elasticity);
	// F3: compra institucional do governo (paper/computer) no dia.
	void consume_government(City& city,
		CityAccumScratch& scratch, std::size_t city_index);
	// F7 calibração: classes de consumo a partir do Gini e da cesta básica.
	void refresh_income_classes(City& city) const;
	// F5: câmbio/tarifas consultados pela fase de serviços.
	const Currency* currency_for(const std::string& nation) const;
	double competitiveness(const std::string& nation) const;
	double fx_rate(const std::string& nation) const;
	const char* currency_code(const std::string& nation) const;
	// Refaz os índices code->moeda e nação->moeda depois de carregar a lista.
	void rebuild_currency_indexes();
	// Atualiza as taxas pela balança comercial do dia (moedas float).
	void tick_fx();
	double tariff_rate(const std::string& buyer_nation,
		const std::string& seller_nation, int product) const;
	// A2: mesma tarifa resolvida por índices de nação (caminho quente do
	// comércio) e o caminho textual original (fallback quando alguma linha não
	// é resolvível por índice).
	double tariff_rate_at_nation(int buyer_nation, int seller_nation,
		int product) const;
	double tariff_rate_text(const std::string& buyer_nation,
		const std::string& seller_nation, int product) const;
	// A2: tarifa no laço quente — índices quando a tabela está completa, senão
	// o caminho textual (mesmo resultado sempre).
	double tariff_rate_cities(const City& buyer_city, const City& seller_city,
		int product) const;
	int product_index(const std::string& product) const;
	const char* product_name(int product) const;
	// F8: serviços.
	int service_sector_index(const std::string& sector) const;
	void rebuild_service_indexes();
	double city_food_reserve(const City& city) const;
	bool service_open(City& city, int sector, double capacity,
		double capital);
	void service_close(ServiceBusiness& business);
	void seed_services();

	// ------------------------------------------------------------------
	// S6b — helpers de load/setup movidos da fachada.
	// ------------------------------------------------------------------
	// Refaz a tabela `buyer|seller|product` -> taxa (set_tariff, save).
	void rebuild_tariff_lookup();
	// F7: preenche população, escolaridade, participação e salários iniciais.
	void apply_labor_profile(City& city);
	// B1: semeia as coortes etárias da cidade a partir da estrutura nacional
	// (age_structure_2026.json). Idempotente; sem dado para a nação não faz NADA
	// (a cidade fica no perfil paramétrico legado — ausência explícita).
	void seed_demography(City& city);
	// B1: deriva população total, idade ativa, aposentados, crianças, reserva
	// recrutável e os fatores de necessidade de saúde/educação das coortes.
	// Chama seed_demography; sem dado, retorna sem tocar em nada.
	void apply_demography(City& city);
	// F9: semeia os grupos a partir dos ativos privados (idempotente).
	void seed_groups();
	// Id de receita -> índice no `recipe_lookup` (-1 se não existir).
	int recipe_index(const std::string& recipe_id) const;

	// ------------------------------------------------------------------
	// §15 — construção como projeto físico (sim/construction.cpp).
	// ------------------------------------------------------------------
	// Calcula materiais/prazo e empilha a obra. Chamado pela fachada (build,
	// grupo e expansão). `expand_existing` soma capacidade a um prédio existente
	// em vez de criar um novo.
	void add_construction_project(ConstructionProject& project);
	// Parcela de TERRA de um projeto (XGS): land_price da zona × área que a
	// obra ocupa. HOUSING: unidades × m²/unidade do padrão (ZONE_RES);
	// BUILDING urbano (receita/insumo declarado) e expansão: vagas ×
	// kUrbanFactoryM2PerWorker (ZONE_IND), limitada a
	// kConstructionLandShareMax do orçamento; fazenda/mina (solo rural,
	// sem zona) e URBAN_INFRA (a obra É a abertura do solo) = 0. Sem preço de
	// terra na cidade/zona devolve 0 (ausência explícita).
	double project_land_cost(const ConstructionProject& project) const;
	// Avança as obras do dia: materiais comprados do mercado, capacidade do
	// setor de construção, pagamento incremental do dono e conclusão.
	void tick_construction();
	// Índice de uma obra em andamento pelo id do prédio final (-1 se não há).
	int project_index(const std::string& building_id) const;
	// Marca a obra `strategic` (bootstrap industrial) pelo id do prédio final.
	void mark_project_strategic(const std::string& building_id);

	// ------------------------------------------------------------------
	// Geometria do mapa e frete (S5b).
	// ------------------------------------------------------------------
	// Capacidade/custo/tempo de cada modal (calibragem do porto).
	static const ModeSpec& spec(TransportMode mode);
	static const char* scope_name(MarketScope scope);
	/** Cria um trecho e calcula viagens, tempo e custo. */
	Leg make_leg(TransportMode mode, const std::string& from, const std::string& to,
		const MapPoint& from_point, const MapPoint& to_point, double load_tons) const;
	double distance_km(const MapPoint& a, const MapPoint& b) const;

	// ------------------------------------------------------------------
	// Porto (S5b): capacidade, fila e cobrança.
	// ------------------------------------------------------------------
	const Port* nearest_port(const std::string& nation,
		const std::string& landmass, const MapPoint& from) const;
	Port* find_port(const std::string& id);
	int city_index_near(const std::string& nation,
		const std::string& landmass, const MapPoint& from) const;
	void init_port_capacities();
	void reset_port_usage();
	double port_charge(Port& port, double tons, double& demurrage_out);

	// ------------------------------------------------------------------
	// Frota nacional (F4 / S5b).
	// ------------------------------------------------------------------
	void init_fleets();
	double nation_sim_population(const std::string& nation) const;
	double nation_income_index(const std::string& nation) const;
	bool nation_food_secure(const std::string& nation) const;
	TransportDemand route_demand(const City& seller,
		const City& buyer, int seller_port, int buyer_port, double tons) const;
	double fleet_capacity_scale(const std::string& nation,
		const TransportDemand& demand, bool& fuel_limited) const;
	void fleet_reserve(const std::string& nation,
		const TransportDemand& demand);
	double fleet_fuel_price(TransportMode mode) const;
	double leg_unit_freight(TransportMode mode,
		double distance_km_value) const;
	double leg_unit_freight_food(TransportMode mode,
		double distance_km_value) const;
	// Compra de frota: a fachada faz o parse de String e as validações de erro;
	// o núcleo compra no mercado interno da nação e credita a frota.
	double fleet_buy_core(const std::string& nation, int product_index,
		double amount, double budget_share, double& spent_out,
		double& unfilled_out);

	// F7: atualiza os salários por nível depois do lucro do dia (mercado
	// apertado/folgado, margem das firmas, âncora de renda do país, piso de
	// salário mínimo e rigidez). Chamado na fase de contabilidade.
	void update_wages(City& city, double building_margin);
	// F9: propriedade e grupos.
	int group_index(const std::string& id) const;
	Group* find_group(const std::string& id);
	const Group* find_group(const std::string& id) const;
	// Decompõe o dono de uma construção: kind 0 state, 1 national, 2 foreign,
	// 3 private (legado). Preenche grupo e nação de origem.
	int owner_kind(const std::string& owner, std::string* group_id,
		std::string* origin_nation) const;
	int fdi_mode(const std::string& nation, const std::string& sector) const;
	double sector_tax_holiday(const std::string& nation,
		const std::string& sector) const;
	double sector_subsidy(const std::string& nation,
		const std::string& sector) const;
	double nation_confidence(const std::string& nation) const;
	// Credita `amount` no capital das famílias da nação, ponderado pelo estrato
	// dono (topo), e devolve o total efetivamente creditado.
	double credit_nation_households(const std::string& nation, double amount);
	// Imposto corporativo + retenção: move o lucro do grupo da cidade para o
	// caixa dele (ou para o orçamento, se estatal). Chamado na contabilidade.
	void route_group_profit(int group_index, const std::string& host_nation,
		double profit, double revenue, City& city, double& firm_surplus);

	// Dinheiro antes do tick (para a fase de invariantes, ainda na fachada).
	double money_before() const { return money_before_; }
	// Preços de abertura (para a fase de clamp, ainda na fachada).
	const std::vector<std::vector<double>>& open_prices() const { return open_prices_; }

	// Invariantes rígidos do tick: devolve a lista de violações (vazia se o
	// estado está sadio). A fachada é quem loga (`push_error`).
	std::vector<std::string> check_invariants(double money_before) const;
	// Aplica um retrato salvo, revertendo o tick corrompido.
	void restore_state(const Snapshot& in);

private:
	WorldState& ws_;
	double money_before_ = 0.0;
	// S16: número de threads configurado. 0 = automático (default,
	// min(núcleos-2, 8)); > 0 = esse número (1 = serial). O pool persistente é
	// criado sob demanda com o número efetivo de threads. Nunca usado fora do
	// tick (o writer já segura o mutex da fachada).
	int sim_threads_ = 0;
	std::unique_ptr<ThreadPool> pool_;
	// Número efetivo de threads (resolve o modo automático).
	int effective_threads() const;
	// Devolve o pool atual, recriando-o se o número de threads mudou.
	ThreadPool& pool();
	// S16: simula o dia de serviços de uma cidade (passos 1-8, sem abrir/fechar
	// negócios); grava a saída de divisas da exportação em foreign_slots[sector]
	// e a poupança acima da reserva de comida (calculada NO TOPO do dia) em
	// savings_surplus_out para a 2ª passada.
	void services_day(City& city, double* foreign_slots,
		double& savings_surplus_out);
	// S16: abertura/fechamento de negócios em 2ª passada serial (ordem fixa
	// cidade -> setor), usando o savings_surplus do TOPO do dia (o serial lia a
	// reserva de comida antes de o passo 6 reduzir estoques — bit-idêntico).
	void services_open_close(City& city, double savings_surplus);
	// Preço de abertura por cidade/produto, capturado no prólogo e usado pela
	// fase de clamp (limite diário único). Reaproveitado entre dias.
	std::vector<std::vector<double>> open_prices_;

	// Retrato do estado para reverter o tick se um invariante rígido falhar.
	void snapshot_state(Snapshot& out) const;
	// Aplica obras de expansão de porto cujo prazo venceu neste dia.
	void tick_port_expansion();
	// Libera os veículos de um pedido que chegou.
	void fleet_release(const std::string& nation,
		const TransportDemand& demand);
	// Escolhe a receita de menor custo unitário corrente para um produto.
	int pick_recipe(const City& city, int output_product) const;
	// A7 — comércio de alimento paralelo determinístico. O laço de compradores
	// foi partido em duas metades: (a) CÁLCULO paralelo e só-leitura, que grava
	// um `FoodTradePlan` por comprador (anel de vizinhos + mercado externo +
	// ofertas já ordenadas por custo) e (b) LIQUIDAÇÃO serial na ordem de
	// sempre, que consome os planos e é a única que escreve em `ws_`. Os planos
	// são um superconjunto do que o dia consome (`sellable_ge` só diminui), então
	// o resultado é bit-idêntico ao serial com qualquer número de threads.
	struct FoodRingEntry {
		int seller = -1;
		int radius = 0;          // anel em que a entrada foi vista
		double distance_km = 0.0;  // calculada UMA vez, reusada no partial_sort
	};
	struct FoodOffer {
		int seller_index = -1;
		int product = 0;
		double factor = 0.0;
		double price = 0.0;          // preço na âncora XGS (mercado do vendedor)
		double origin_anchor = 0.0;  // preço × competitividade float do vendedor
		double unit_freight = 0.0;   // frete em âncora
		double unit_tariff = 0.0;    // tarifa em âncora
		double delivered = 0.0;      // entregue na moeda do comprador (exibição)
		double unit_cost_per_ge = 0.0;  // âncora por tonelada-equivalente de grão
	};
	struct FoodBuyer {
		int index = -1;
		City* city = nullptr;
		double gap = 0.0;  // toneladas-equivalentes de grão
	};
	struct FoodTradePlan {
		bool active = false;      // false: comprador sem vendedor alcançável
		bool exhaustive = false;  // mundo pequeno: todas as cidades vendedoras
		bool ring_complete = true;  // false só se o anel foi cortado pelo teto
		// Entradas do anel na ordem exata de varredura (topo do dia).
		std::vector<FoodRingEntry> ring;
		// Mercado externo na ordem de append (com repetidos) e ordenado/único.
		std::vector<int> ext;
		std::vector<int> ext_sorted;
		// Ofertas do universo `únicos(ring ∪ ext)` JÁ na ordem de custo do dia
		// (`stable_sort` por `unit_cost_per_ge` feito na fase paralela). Empate
		// preserva a ordem de entrada: índice de vendedor crescente e, dentro
		// dele, os slots da cesta — a ordem de candidato do `stable_sort` antigo.
		std::vector<FoodOffer> offers;
		// Scratch do plano: união ascendente do anel (índices quaisquer) com o
		// mercado externo, usada só na fase de cálculo.
		std::vector<int> universe;
	};
	// Planos por comprador do dia (índice = posição em `buyers`, que é
	// reordenado a cada dia), reaproveitados entre dias. NÃO entram no save.
	std::vector<FoodTradePlan> food_plans_;
	// Scratch serial da liquidação (reaproveitado entre compradores/dias).
	std::vector<FoodRingEntry> food_ring_live_;
	std::vector<int> food_candidates_;
	std::vector<int> food_candidates_union_;
	// A7c: ofertas do dia em ordem de percorrimento, por PONTEIRO — o caminho
	// normal aponta para `FoodTradePlan::offers` (já ordenado em paralelo, sem
	// copiar blocos); o caminho lento aponta para `food_offers_settle_`.
	std::vector<const FoodOffer*> food_offers_live_;
	std::vector<FoodOffer> food_offers_settle_;
	// A7c: marca O(1) de pertinência ao conjunto de candidatos do comprador do
	// dia: `food_candidate_stamp_[vendedor] == bi + 1`. Zerada uma vez por dia
	// (O(cidades)), nunca no laço de ofertas.
	std::vector<int> food_candidate_stamp_;
	// 6. Comércio de alimento (cesta de calorias) entre cidades.
	void trade_food();
	// 6b. Comércio dos demais bens (insumos industriais), pela mesma malha.
	void trade_goods();
};

} // namespace ogs
