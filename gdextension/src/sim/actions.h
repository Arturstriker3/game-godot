#pragma once
// A5 — ações do mercado de construção como módulo C++ puro (STL, sem godot-cpp).
//
// `apply_invest`, `apply_build` e `apply_group_invest` nasceram na fachada
// `EconomySim` devolvendo `Dictionary`. Aqui a mesma lógica roda sobre
// `WorldState`/`SimCore` e devolve structs; a fachada só converte o resultado
// em `Dictionary` na borda (as chaves são escritas nas MESMAS condições do
// original). Equivalência bit a bit: os corpos foram copiados verbatim.
#include <string>

namespace ogs {

class SimCore;
struct WorldState;
struct Building;

// Resultado de uma expansão de capacidade (instantânea ou por projeto).
struct InvestResult {
	double invested = 0.0;
	double capacity_added = 0.0;
	std::string project_id;
	std::string error;
};

// Resultado de uma obra nova do estado/privada.
struct BuildResult {
	bool created = false;
	double cost = 0.0;
	bool zone_pending = false;
	std::string project_id;
	std::string error;
};

// Resultado de uma obra nova de grupo (nacional/estrangeiro, com JV opcional).
struct GroupInvestResult {
	bool created = false;
	double cost = 0.0;
	double cash = 0.0;
	double local_share = 0.0;
	std::string owner;
	std::string project_id;
	std::string error;
};

// D-086 — resultado de um APROFUNDAMENTO de mina (obra física §15 que abre o
// estágio seguinte e repõe a produção).
struct DeepenResult {
	bool started = false;
	double cost = 0.0;
	double capacity_added = 0.0;
	int stage = 0;
	int duration_days = 0;
	std::string project_id;
	std::string error;
};

// Construção de uma cidade pelo id (-1 se não existe). Antes era um método da
// fachada; a fachada continua expondo `EconomySim::find_building` delegando.
Building* find_building(WorldState& ws, const std::string& city_id,
		const std::string& id);

// §15: custo ainda não pago das obras de uma cidade para um dono ("state",
// "private" ou o id de um grupo). Base do limite de comprometimento: o dono não
// inicia obra nova cujo custo, somado ao já comprometido, estoura o caixa atual.
double committed_construction(const WorldState& ws, const std::string& city_id,
		const std::string& owner);

// Expansão de capacidade de uma construção existente (§15: vira projeto físico
// quando a construção não é instantânea).
InvestResult action_invest(WorldState& ws, SimCore& core,
		const std::string& city_id, const std::string& building_id,
		double amount, bool instant_construction);

// Obra nova do estado/privada (§15/§19: projeto físico ou prédio instantâneo).
BuildResult action_build(WorldState& ws, SimCore& core,
		const std::string& city_id, const std::string& building_id,
		const std::string& resource, int input_product, int output_product,
		double capacity, double jobs, double wage, double opex,
		double input_per_output, const std::string& owner, double cost,
		const std::string& recipe_id, bool instant_construction,
		bool bootstrap_construction);

// Obra nova de um grupo (F9: FDI, joint venture e produção primária).
GroupInvestResult action_group_invest(WorldState& ws, SimCore& core,
		const std::string& group_id, const std::string& city_id,
		const std::string& building_id, const std::string& resource,
		int output_product, double capacity, const std::string& recipe_id,
		const std::string& partner_group, bool instant_construction);

// D-086 — APROFUNDAR/EXPANDIR mina: obra física §15 que abre o estágio
// seguinte da mina, repõe a produção e sobe a capacidade, consumindo a reserva
// restante da jazida. Pago pelo DONO da mina (Estado, grupo nacional/estrangeiro
// ou firma privada). Sem reserva, sem estágio novo.
DeepenResult action_deepen_mine(WorldState& ws, SimCore& core,
		const std::string& city_id, const std::string& building_id,
		bool instant_construction);

// B5 — OBRA DE INFRAESTRUTURA da região/estado: projeto físico §15 que sobe UM
// nível (estrada/energia/água) da região da cidade. Pago pelo GOVERNO NACIONAL
// (caixa do governo da cidade, que é a cota equalizada do tesouro nacional).
// `infra_type`: 0 estrada, 1 energia, 2 água. Custo = kInfraCostPerCapitaPerLevel
// × população da região; prazo kInfraBuildDays. Nível limitado a kInfraMaxLevel.
struct InfraResult {
	bool started = false;
	double cost = 0.0;
	int infra_type = -1;
	int next_level = 0;
	int duration_days = 0;
	std::string project_id;
	std::string error;
};
InfraResult action_infra_build(WorldState& ws, SimCore& core,
		const std::string& city_id, int infra_type, bool instant_construction);

// B5 — CONCESSÃO de mina/fazenda estatal a grupo (nacional/estrangeiro) com
// royalty sobre a produção. Transferência de dinheiro conservada (comprador
// paga o valor contábil ao governo). `royalty_rate` <= 0 usa kConcessionRoyaltyRate.
struct ConcedeResult {
	bool ok = false;
	double price = 0.0;
	double royalty = 0.0;
	std::string owner;
	std::string error;
};
ConcedeResult action_concede(WorldState& ws, SimCore& core,
		const std::string& city_id, const std::string& building_id,
		const std::string& group_id, double royalty_rate);

// G2 — OBRA de armazém agregado (fila §15: obra, prazo, manutenção). `type` é
// o StorageType (0..7); `scope` 0 = governo, 1 = privado. O custo é
// kStorageCostPerTon[type] × tons, pago pelo dono (governo ou firma). Obra
// instantânea (laboratório) entrega a capacidade no ato; senão vira projeto
// físico e a capacidade entra na conclusão.
struct StorageBuildResult {
	bool started = false;
	double cost = 0.0;
	double capacity_added = 0.0;
	int type = -1;
	int scope = 0;
	int duration_days = 0;
	std::string project_id;
	std::string error;
};
StorageBuildResult action_storage_build(WorldState& ws, SimCore& core,
		const std::string& city_id, int type, int scope, double tons,
		bool instant_construction);

// G2 — compra (`buy`) ou liberação (`release`) de estoque guardado. Compra tira
// do mercado local e paga ao vendedor; liberação devolve ao mercado local e o
// comprador paga o dono (governo) — ou recebe de graça em `relief` (socorro).
// Toneladas e dinheiro são conservados; nenhum caixa novo é criado.
struct StorageTradeResult {
	bool ok = false;
	double tons = 0.0;
	double value = 0.0;
	double free_tons = 0.0;
	std::string error;
};
StorageTradeResult action_storage_trade(WorldState& ws, SimCore& core,
		const std::string& city_id, int type, int scope, int product,
		double tons, bool buy, bool relief);

} // namespace ogs
