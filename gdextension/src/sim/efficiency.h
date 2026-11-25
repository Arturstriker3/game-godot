#pragma once
// D-125 — EFICIÊNCIA DE PRODUÇÃO (estilo HOI4). Regras puras sobre
// WorldState/Building, compartilhadas pelo SimCore (curva diária, produção,
// colheita) e pela fachada (API de troca de produto/modo e equipamento).
//
// Nada aqui lê arquivo: os parâmetros chegam pelo `EfficiencyParams` do
// WorldState, cujo default é o espelho do JSON de balanceamento
// `data/definitions/production/efficiency.json`.
#include "sim_types.h"
#include "world_state.h"

namespace ogs {

// Classe de eficiência da construção. `Ranch` (pecuária) usa a curva mais
// lenta; `Farm` cobre as lavouras (inclusive as sem plano sazonal).
enum class EfficiencyClass : int {
	kFactory = 0,
	kRefinery = 1,
	kMine = 2,
	kFarm = 3,
	kRanch = 4,
};

namespace eff {

// Classe da construção (indústria, refino, mina, lavoura ou pecuária).
EfficiencyClass classify(const WorldState& ws, const City& city,
	const Building& building);

// Classe de uma construção HIPOTÉTICA do template (mesma regra do `classify`,
// com os campos que o build vai gravar): usada pela IA para estimar a rampa
// antes de abrir a planta. `recipe` = -1 fora de receita; `input_product` = -1
// na extração primária.
EfficiencyClass classify_new(const WorldState& ws, const City& city,
	int output_product, int recipe, int input_product);

// Eficiência MÉDIA esperada nos próximos `days` dias de operação, partindo de
// `from` (planta nova = início da curva da classe). Repete o passo diário da
// fase de eficiência (`step`/`cap`) sem tocar no estado: é a expectativa
// determinística usada pelo gate de payback da IA para não valorar a planta
// pela capacidade cheia. `idle_days` = dias de transição sem produção (troca
// de produto); não contam receita nem maturação. `equipment_active` assume o
// trator/máquina ligado (diesel disponível) no futuro.
double mean_efficiency(const WorldState& ws, EfficiencyClass cls, double from,
	bool equipment_active, int days, int idle_days = 0);

// D-131 — média da curva D-125 no intervalo de dias de OPERAÇÃO [first_day,
// last_day] (0 = primeiro dia de produção, mesma convenção de
// `mean_efficiency`). Soma `days = last_day - first_day + 1` termos do MESMO
// laço diário (`step`/`cap`) partindo de `from`; `mean_between(0, days - 1)`
// é idêntico a `mean_efficiency(days)`. Intervalo vazio ou invertido devolve
// `from` (sem estimativa para um mês sem operação).
double mean_between(const WorldState& ws, EfficiencyClass cls, double from,
	bool equipment_active, int first_day, int last_day);

// Curva da classe no estado atual.
const EfficiencyCurve& curve(const WorldState& ws, EfficiencyClass cls);

// Teto da maturação: 1.0 com equipamento ativo; `cap_without` sem ele
// (lavoura/mina/pecuária). Indústria/refino não dependem de equipamento.
double cap(const WorldState& ws, const Building& building, EfficiencyClass cls);

// Passo diário da eficiência (linear até `mid` e depois até 1.0), já com o
// bônus do equipamento ativo.
double step(const WorldState& ws, const Building& building,
	EfficiencyClass cls, bool equipment_active);

// É a receita de refino de PETRÓLEO? (saída gasolina + insumo petróleo; o
// prefixo do id não vale — `gasoline_b` é etanol).
bool is_refinery(const WorldState& ws, const Building& building);

// Pecuária? (produto `meat`, declarado no núcleo; o JSON não lista a cesta).
bool is_ranch(const WorldState& ws, const Building& building);

// Mundo SEMEADO (ano-base): a planta já existe madura. `equipment` liga o
// trator/máquina das fazendas e minas do seed.
void mature(Building& building, bool with_equipment);

// Construção NOVA: eficiência inicial da classe, zero dias de operação e sem
// equipamento. Transição e modo nascem zerados.
void init_new(WorldState& ws, const City& city, Building& building);

// Dias de transição sem produção para trocar a construção para `product`
// (-1 = troca inválida, ex.: mina presa à jazida ou fazenda sem plano).
int transition_days(const WorldState& ws, const City& city,
	const Building& building, int product);

// Mesma família? (produtos do mesmo `goods.json.sector`).
bool same_family(const WorldState& ws, int left, int right);

// Fração do lote de petróleo do produto no modo da refinaria (0..2).
double mode_share(const WorldState& ws, int mode, int product);

// Rendimento efetivo do produto principal e do subproduto `slot` da receita.
// Fora da refinaria (ou no modo padrão) é o próprio dado da receita; na
// refinaria os modos trocam as frações (45/29/9, 38/36/9, 40/28/15).
double main_yield(const WorldState& ws, const Building& building,
	const Recipe& recipe);
double byproduct_yield(const WorldState& ws, const Building& building,
	const Recipe& recipe, int slot);

// Fator de colheita de (cidade, produto): média das eficiências das fazendas
// que produzem o bem, ponderada pela capacidade. 1.0 quando não há fazenda
// rastreada para o par (plano de dado sem construção).
double harvest_factor(const WorldState& ws, std::size_t city_index,
	int product);

} // namespace eff
} // namespace ogs
