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

} // namespace ogs
