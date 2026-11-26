// Módulo de domínio: comércio (S9 do docs/economy/core_refactor_plan.md).
// SimCore:: métodos movidos de sim_core.cpp — realocação byte-idêntica.
#include "sim_core.h"

#include "profiling.h"
#include "sim_constants.h"
#include "sim_labor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>

namespace ogs {

namespace {
// A2: mesma massa de terra comparando o ÍNDICE derivado (o laço de compradores
// compara centenas de pares por dia). Se algum dos dois não tiver índice — só
// possível em estado montado à mão — cai na comparação textual original.
inline bool same_landmass_city(const City& left, const City& right) {
	if (left.landmass_index >= 0 && right.landmass_index >= 0) {
		return left.landmass_index == right.landmass_index;
	}
	return left.landmass == right.landmass;
}

// A7: folga de entradas do anel guardadas no plano do comprador, ALÉM de
// `min(k_neighbors, reachable_sellers)`. O plano precisa ser um superconjunto do
// que a liquidação consome — inclusive quando alguns dos vendedores mais
// próximos se esgotam durante o dia e o `partial_sort` desce na lista. Cada
// entrada a mais custa varredura de anel, um haversine e ofertas (vendedor ×
// slots) na fase paralela; medido: 64 de folga davam 72 entradas por comprador
// (3× o necessário) e dobravam o custo. 8 cobre a escuridão típica do dia.
constexpr int kFoodPlanSlack = 8;

// A7b — orçamento de células da varredura do anel, por comprador e por dia.
// Limite de segurança: nenhum comprador visita mais células que a grade
// inteira. Com oferta escassa (fim do dia, porto bloqueando rotas), o anel
// tentava provar que não havia mais ninguém até o fim do mapa para cada
// comprador; 1.024 células (raio ~16, ~4.000 km) já cobrem o alcance
// econômico — além dele entra o mercado externo (M ofertas mais baratas do
// globo), que é anexado sempre.
constexpr int kFoodScanBudget = 1024;

// A7b — fronteira de retomada da varredura do anel de UM comprador do dia:
// onde o plano parou e quanto do orçamento de células sobrou. Fica aqui (e não
// em `FoodTradePlan`) para manter esta fase dentro de `trade.cpp`; é scratch do
// dia, não entra no save.
struct FoodScanTail {
	int radius = 0;  // primeiro raio NÃO varrido (fronteira de retomada)
	int budget = 0;  // células que sobraram do orçamento da varredura
};
}  // namespace

void SimCore::trade_food() {
	// Comércio genérico de alimento (cesta de calorias, B3) entre cidades do
	// mundo real: cidades com déficit compram das que têm excedente, escolhendo
	// o alimento que entregar a caloria mais barata (preço + frete por
	// tonelada-equivalente de grão), mais perto primeiro quando há empate. O
	// frete é serviço doméstico do comprador (conserva o dinheiro).
	//
	// Porto (docs/economy/port_design.md): a travessia entre massas de terra
	// passa obrigatoriamente por um porto da nação em cada ponta; a capacidade
	// diária do cais é consumida aqui e o excesso vira fila (atraso + demurrage).
	OGS_SIM_ZONE(kPhaseTradeFood);
	reset_port_usage();

	// A7: estado do dia compartilhado pelas três fases (preparo → cálculo
	// paralelo → liquidação serial). `sellable_ge` e as tabelas abaixo são as do
	// TOPO do dia: a fase de cálculo só as LÊ (nunca escreve em `ws_`), e a
	// liquidação reaplica o filtro de `sellable_ge` como hoje.
	std::vector<FoodBuyer> buyers;
	std::vector<double> sellable_ge;
	std::vector<int> city_port;
	std::vector<std::vector<int>> grid;
	std::vector<char> cell_has_seller;
	std::vector<std::vector<int>> offers_by_slot;
	std::vector<double> city_competitive;
	std::vector<double> city_fx;
	std::vector<double> city_to_port_dist;
	std::vector<int> sellers_in_landmass;
	std::vector<int> port_sellers_in_landmass;
	int port_sellers_total = 0;
	const std::size_t city_count = ws_.cities.size();
	const double cell = kTradeCellMapUnits;
	const int grid_w = std::max(1, static_cast<int>(std::ceil(kMapWidth / cell)));
	const int grid_h = std::max(1, static_cast<int>(std::ceil(kMapHeight / cell)));
	const bool exhaustive = city_count <= kTradeExhaustiveLimit;
	const int k_neighbors = std::max(1, ws_.trade_neighbors);
	const int k_external = std::max(0, ws_.trade_external);
	int sellers_left = 0;
	std::size_t shipments = 0;

	{
	OGS_SIM_ZONE(kPhaseTradeFoodPrep);

	// Posição alimentar de cada cidade em toneladas-equivalentes de grão:
	// (estoque local + em trânsito) × fator calórico.
	std::vector<double> food_ge(ws_.cities.size(), 0.0);
	for (std::size_t i = 0; i < ws_.cities.size(); ++i) {
		const City& city = ws_.cities[i];
		double ge = 0.0;
		for (int slot = 0; slot < static_cast<int>(ws_.food_products.size()); ++slot) {
			const int product = food_product(slot);
			ge += city.markets[product].stock * food_calories(product);
		}
		food_ge[i] = ge;
	}
	// Chegada em trânsito por cidade: uma passada única com o índice de
	// cidades. Antes, cada carga varria TODAS as cidades comparando strings
	// (O(cargas × cidades) por dia, ~440 mil comparações com 512 cargas e 858
	// cidades no cenário real).
	for (const Shipment& shipment : ws_.in_transit) {
		const auto found = ws_.city_lookup.find(shipment.to_city);
		if (found == ws_.city_lookup.end()) {
			continue;
		}
		const std::size_t i = static_cast<std::size_t>(found->second);
		food_ge[i] += shipment.amount * food_calories(shipment.product);
	}

	// Excedente vendável por cidade: o vendedor exporta o que sobra ACIMA da
	// própria necessidade de UM dia (não do buffer de cobertura). A fase 4 já
	// consome a refeição do dia; exigir a cobertura de 3 dias inteiros trava a
	// exportação — a produção visa exatamente `sold_prev × cover_days`, então o
	// estoque estaciona na cobertura e nada fica vendável (nenhuma cidade
	// exporta, e as deficitárias morrem de fome). Manter 1 dia preserva o buffer
	// mínimo e deixa o excedente fluir para quem tem déficit.
	sellable_ge.assign(ws_.cities.size(), 0.0);
	for (std::size_t i = 0; i < ws_.cities.size(); ++i) {
		sellable_ge[i] = std::max(0.0, food_ge[i] - ws_.cities[i].pop.need);
	}

	for (std::size_t i = 0; i < ws_.cities.size(); ++i) {
		// Cobertura-alvo em dias (como o mercado legado usa kCoverDays): a cidade
		// importa para manter um estoque de alguns dias, não só o déficit do dia.
		const double target = ws_.cities[i].pop.need * ws_.inventory_cover_days;
		const double gap = target - food_ge[i];
		if (gap > 1e-9) {
			buyers.push_back({static_cast<int>(i), &ws_.cities[i], gap});
		}
	}
	if (buyers.empty()) {
		return;
	}

	// Quantos vendedores existem no mundo hoje. Sem nenhum, nenhum comprador
	// pode ser atendido: sair aqui evita varrer a grade inteira por comprador
	// (medido em 650 cidades, dia 1: 1,4 s só provando que não havia oferta).
	int total_sellers = 0;
	for (std::size_t i = 0; i < sellable_ge.size(); ++i) {
		if (sellable_ge[i] > 1e-9) {
			++total_sellers;
		}
	}
	if (total_sellers == 0) {
		return;
	}

	// Compradores com MENOR COBERTURA RELATIVA primeiro (gap / necessidade):
	// sob escassez, todas as cidades convergem para a mesma fração atendida.
	// A ordem anterior era gap ABSOLUTO ascendente — ou seja, completava o
	// buffer de quem já estava quase cheio e deixava as megacidades (Baku,
	// Xangai, São Paulo) no fim da fila todos os dias; com a oferta mundial
	// abaixo da necessidade, elas nunca eram alcançadas (§19).
	std::stable_sort(buyers.begin(), buyers.end(),
		[](const FoodBuyer& left, const FoodBuyer& right) {
			const double left_need = std::max(1e-9, left.city->pop.need);
			const double right_need = std::max(1e-9, right.city->pop.need);
			return left.gap / left_need > right.gap / right_need;
		});

	// Índice espacial e ofertas por produto, montados UMA vez por dia.
	//
	// Antes, cada comprador varria TODAS as cidades e ordenava todas as ofertas
	// do mundo (frete por par) — O(cidades²), ~500 ms/dia com 650 cidades.
	// Agora o comprador avalia só os K vendedores mais próximos (grade) mais as
	// M ofertas mais baratas por caloria do globo, que representam o mercado
	// externo. São vendedores REAIS: o dinheiro continua indo para uma firma
	// existente, sem mercado agregado artificial.
	// Porto nacional de cada cidade (mesma nação e massa de terra), escolhido
	// uma vez por dia: o mais próximo é o de menor frete terrestre. É o porto
	// que a cidade usa para exportar/importar — `port_of` só aponta a sede.
	city_port.assign(city_count, -1);
	if (!ws_.ports.empty()) {
		// Baldes por (nação, massa de terra): a cidade só compara com os portos
		// que pode usar (porto nacional na própria massa), não com os ~138 do
		// mundo. Custa uma fração de uma passada cidades × portos.
		std::unordered_map<std::string, std::vector<int>> ports_by_region;
		ports_by_region.reserve(ws_.ports.size() * 2);
		for (std::size_t pi = 0; pi < ws_.ports.size(); ++pi) {
			const Port& port = ws_.ports[pi];
			ports_by_region[port.nation + '\n' + port.landmass]
				.push_back(static_cast<int>(pi));
		}
		for (std::size_t ci = 0; ci < city_count; ++ci) {
			const City& city = ws_.cities[ci];
			const auto found =
				ports_by_region.find(city.nation + '\n' + city.landmass);
			if (found == ports_by_region.end()) {
				continue;
			}
			int best_port = -1;
			double best_distance = 0.0;
			for (const int pi : found->second) {
				const double distance =
					distance_km(city.center, ws_.ports[static_cast<std::size_t>(pi)].center);
				if (best_port < 0 || distance < best_distance) {
					best_port = pi;
					best_distance = distance;
				}
			}
			city_port[ci] = best_port;
		}
	}
	grid.assign(static_cast<std::size_t>(grid_w * grid_h), std::vector<int>());
	// Bitmask de células com pelo menos um vendedor: quando a oferta do dia é
	// escassa (regra do porto deixa parte do mundo sem rota marítima), o anel
	// varria a grade inteira por comprador. A máscara evita tocar nas ~12 mil
	// listas vazias (cache miss por célula) sem mudar QUEM é candidato: o
	// filtro de `sellable_ge` dentro de scan_cell continua valendo.
	cell_has_seller.assign(static_cast<std::size_t>(grid_w * grid_h), 0);
	auto cell_index = [cell, grid_w, grid_h](const MapPoint& point) {
		const int ix = std::clamp(static_cast<int>(point.x / cell), 0, grid_w - 1);
		const int iy = std::clamp(static_cast<int>(point.y / cell), 0, grid_h - 1);
		return iy * grid_w + ix;
	};
	for (std::size_t si = 0; si < city_count; ++si) {
		const int index = cell_index(ws_.cities[si].center);
		grid[static_cast<std::size_t>(index)].push_back(static_cast<int>(si));
		if (sellable_ge[si] > 1e-9) {
			cell_has_seller[static_cast<std::size_t>(index)] = 1;
		}
	}

	// Ofertas por slot da cesta, ordenadas pelo custo por caloria do preço
	// (preço/fator, sem frete): as M primeiras formam o mercado externo.
	offers_by_slot.assign(ws_.food_products.size(), std::vector<int>());
	for (std::size_t si = 0; si < city_count; ++si) {
		if (sellable_ge[si] <= 1e-9) {
			continue;
		}
		for (int slot = 0; slot < static_cast<int>(ws_.food_products.size()); ++slot) {
			const int product = food_product(slot);
			const double factor = food_calories(product);
			if (factor <= 0.0) {
				continue;
			}
			const double price = ws_.cities[si].markets[product].price
				* competitiveness(ws_.cities[si].nation);
			const double sellable_tons = std::min(
				ws_.cities[si].markets[product].stock, sellable_ge[si] / factor);
			if (sellable_tons > 1e-9 && price > 0.0) {
				offers_by_slot[slot].push_back(static_cast<int>(si));
			}
		}
	}
	for (int slot = 0; slot < static_cast<int>(ws_.food_products.size()); ++slot) {
		const int product = food_product(slot);
		std::vector<int>& list = offers_by_slot[slot];
		// Dentro de um slot o fator calórico é o mesmo para todos: ordenar pelo
		// preço equivale a ordenar pelo custo por caloria (sem frete).
		std::stable_sort(list.begin(), list.end(), [this, product](int l, int r) {
			return ws_.cities[static_cast<std::size_t>(l)].markets[product].price
				* competitiveness(ws_.cities[static_cast<std::size_t>(l)].nation)
				< ws_.cities[static_cast<std::size_t>(r)].markets[product].price
				* competitiveness(ws_.cities[static_cast<std::size_t>(r)].nation);
		});
	}

	// Pré-calcula por cidade (uma vez por dia) os valores puros de nação usados
	// no laço quente de ofertas: competitividade e câmbio. Eram lookups de hash
	// por string DENTRO do laço por oferta (custo dominante em debug). São
	// funções puras do estado, que não muda durante o comércio (o câmbio roda
	// na fase fx, depois) — bit-idêntico.
	city_competitive.assign(city_count, 1.0);
	city_fx.assign(city_count, 1.0);
	for (std::size_t ci = 0; ci < city_count; ++ci) {
		const std::string& nation = ws_.cities[ci].nation;
		city_competitive[ci] = competitiveness(nation);
		city_fx[ci] = fx_rate(nation);
	}
	// Distância cidade → próprio porto (pré-calculada uma vez por dia): remove
	// 2 dos 3 haversines por candidato na travessia marítima do laço quente.
	city_to_port_dist.assign(city_count, 0.0);
	for (std::size_t ci = 0; ci < city_count; ++ci) {
		if (city_port[ci] >= 0) {
			city_to_port_dist[ci] = distance_km(
				ws_.cities[ci].center,
				ws_.ports[static_cast<std::size_t>(city_port[ci])].center);
		}
	}

	// Nunca vale a pena procurar mais vizinhos do que existem vendedores no mundo:
	// quando há menos ofertas que K, o anel para de expandir assim que todas
	// foram vistas (antes ele varria a grade inteira para provar isso, o que
	// custava ~1,4 s por dia com 650 cidades). `sellers_left` cai quando um
	// vendedor se esgota no meio do dia.
	sellers_left = total_sellers;

	// Vendedores alcançáveis por um comprador (regra do porto): na mesma massa
	// de terra sempre; entre massas, só se AS DUAS pontas têm porto. Contagens
	// iniciais do dia são um teto (só diminuem quando um vendedor esgota), então
	// nunca descartam um comprador que ainda poderia comprar. Sem isso, um
	// comprador sem rota possível varria a grade inteira (12 mil células) todos
	// os dias atrás de vendedores que nunca poderiam servi-lo.
	// A2: contagem por ÍNDICE de massa de terra (o laço de compradores roda uma
	// vez por cidade/dia; o hash de string saiu do caminho). A tabela é
	// construída a partir das próprias cidades, então todo índice é >= 0.
	const std::size_t landmass_count =
		static_cast<std::size_t>(ws_.landmass_table.size());
	sellers_in_landmass.assign(landmass_count, 0);
	port_sellers_in_landmass.assign(landmass_count, 0);
	port_sellers_total = 0;
	for (std::size_t si = 0; si < city_count; ++si) {
		if (sellable_ge[si] <= 1e-9) {
			continue;
		}
		const int landmass = ws_.cities[si].landmass_index;
		if (landmass < 0) {
			continue;
		}
		++sellers_in_landmass[static_cast<std::size_t>(landmass)];
		if (city_port[si] >= 0) {
			++port_sellers_in_landmass[static_cast<std::size_t>(landmass)];
			++port_sellers_total;
		}
	}
	}  // fim do preparo

	// A7 — oferta (vendedor × slot da cesta): frete (haversine + porto), tarifa,
	// competitividade, câmbio e custo entregue por tonelada-equivalente. Só
	// depende de valores ESTÁVEIS dentro de `phase_trade` (preço de mercado,
	// frete, tarifa, competitividade, câmbio), então vale para qualquer instante
	// do dia. A quantidade vendável NÃO entra aqui: ela é filtro, e o filtro de
	// hoje (`sellable_tons <= 1e-9`) é reaplicado na liquidação.
	const auto append_seller_offers = [&](const FoodBuyer& buyer, int si,
			std::vector<FoodOffer>& out) {
		if (si == buyer.index
				|| sellable_ge[static_cast<std::size_t>(si)] <= 1e-9) {
			return;
		}
		City& seller = ws_.cities[static_cast<std::size_t>(si)];
		double unit_freight = 0.0;
		if (same_landmass_city(seller, *buyer.city)) {
			unit_freight = leg_unit_freight_food(TransportMode::Truck,
				distance_km(seller.center, buyer.city->center));
		} else {
			// Travessia marítima só entre nações COM porto na massa de terra de
			// cada cidade: cidade→porto (caminhão) + porto→porto (navio) +
			// porto→cidade (caminhão). Sem porto de um dos lados, não há oferta.
			const int seller_port = city_port[static_cast<std::size_t>(si)];
			const int buyer_port_index =
				city_port[static_cast<std::size_t>(buyer.index)];
			if (seller_port < 0 || buyer_port_index < 0) {
				return;
			}
			const Port& sp = ws_.ports[static_cast<std::size_t>(seller_port)];
			const Port& bp = ws_.ports[static_cast<std::size_t>(buyer_port_index)];
			unit_freight =
				leg_unit_freight_food(TransportMode::Truck,
					city_to_port_dist[static_cast<std::size_t>(si)])
				+ leg_unit_freight_food(TransportMode::Ship,
					distance_km(sp.center, bp.center))
				+ leg_unit_freight_food(TransportMode::Truck,
					city_to_port_dist[static_cast<std::size_t>(buyer.index)]);
		}
		for (int slot = 0; slot < static_cast<int>(ws_.food_products.size()); ++slot) {
			const int product = food_product(slot);
			const double factor = food_calories(product);
			if (factor <= 0.0) {
				continue;
			}
			// Vendável deste produto, limitado pelo excedente total da cidade
			// (acima da cobertura) e pelo estoque físico.
			const double sellable_tons = std::min(
				seller.markets[product].stock,
				sellable_ge[static_cast<std::size_t>(si)] / factor);
			if (sellable_tons <= 1e-9) {
				continue;
			}
			const double price = seller.markets[product].price;
			if (price <= 0.0) {
				continue;
			}
			const double origin_anchor =
				price * city_competitive[static_cast<std::size_t>(si)];
			const double tariff_factor =
				tariff_rate_cities(*buyer.city, seller, product);
			const double unit_tariff =
				(origin_anchor + unit_freight) * tariff_factor;
			// Valor entregue em âncora: é ele que ordena a oferta (sem
			// round-trip de câmbio). A moeda do comprador só divide para
			// exibição — é fator comum, não muda a ordem.
			const double delivered_anchor =
				origin_anchor + unit_freight + unit_tariff;
			const double buyer_fx = city_fx[static_cast<std::size_t>(buyer.index)];
			const double delivered =
				delivered_anchor / std::max(1e-9, buyer_fx);
			const double unit_cost_per_ge = delivered_anchor / factor;
			out.push_back(FoodOffer{si, product, factor, price, origin_anchor,
				unit_freight, unit_tariff, delivered, unit_cost_per_ge});
		}
	};

	// A7 — varredura do anel. É função do DIA (posições, portos, massa de terra
	// e `sellable_ge > 1e-9` no TOPO); a parada (`count >= wanted`,
	// `cells_budget`, raios) é decisão do dia e roda na liquidação. Aqui
	// `wanted_count` só diz quanto o chamador quer acumular.
	// `overshoot_radii`: quantos raios varrer DEPOIS de atingir `wanted_count`.
	// O plano usa 1 para a liquidação sempre ter uma fronteira de raio após o
	// ponto em que os sobreviventes satisfazem `wanted`; a varredura de
	// emergência da liquidação usa 0 (para de onde o laço de hoje pararia).
	//
	// `start_radius`/`start_cells_budget` deixam RETOMAR a passada numa
	// fronteira de raio (A7b): o plano grava onde parou e com quanto orçamento,
	// e a liquidação continua de lá em vez de refazer o anel desde o raio 0.
	// `out` NÃO é limpo — as entradas já acumuladas contam no teste
	// `out.size() >= wanted_count`, exatamente como na passada única de hoje.
	// Devolve o orçamento de células que sobrou; grava em `next_radius` o
	// primeiro raio não varrido e em `complete` se a varredura cobriu tudo.
	const auto scan_ring = [&](const FoodBuyer& buyer, int wanted_count,
			int overshoot_radii, int start_radius, int start_cells_budget,
			int& next_radius, bool& complete, std::vector<FoodRingEntry>& out)
			-> int {
		const int buyer_port = city_port[static_cast<std::size_t>(buyer.index)];
		const int bx = std::clamp(
			static_cast<int>(buyer.city->center.x / cell), 0, grid_w - 1);
		const int by = std::clamp(
			static_cast<int>(buyer.city->center.y / cell), 0, grid_h - 1);
		// Raio máximo ÚTIL: distância de Chebyshev até o canto mais distante
		// da grade. Antes era max(grid_w, grid_h), ou seja, o anel continuava
		// varrendo células vazias muito além do mapa: com grade 157×79 e
		// menos de K vendedores no mundo, cada comprador percorria ~5,2 M
		// células (O(raio³) acumulado) — a rodada ficava minutos parada.
		//
		// Teto ECONÔMICO (regra do porto): sem porto só há caminhão, cujo
		// frete já supera o preço do alimento bem antes de 2.000 km (~8
		// células de 250 km). Com porto, o navio é 5× mais barato e o anel
		// cobre o globo. Sem esse teto, um comprador sem rota marítima
		// varria a grade inteira todos os dias atrás de vendedores que nunca
		// seriam competitivos.
		const int ring_radius_limit = buyer_port >= 0 ? 48 : 8;
		const int max_radius = std::min(std::max(
			std::max(bx, grid_w - 1 - bx), std::max(by, grid_h - 1 - by)),
			ring_radius_limit);
		// Orçamento de células da passada (ver `kFoodScanBudget`): na
		// retomada é o que sobrou do plano, nunca um orçamento novo.
		int cells_budget = std::min(grid_w * grid_h, start_cells_budget);
		auto scan_cell = [&](int ix, int iy, int radius) {
			if (ix < 0 || ix >= grid_w || iy < 0 || iy >= grid_h || cells_budget <= 0) {
				return;
			}
			--cells_budget;
			const std::size_t cell_offset =
				static_cast<std::size_t>(iy * grid_w + ix);
			if (cell_has_seller[cell_offset] == 0) {
				return;
			}
			for (const int si : grid[cell_offset]) {
				if (si == buyer.index || sellable_ge[static_cast<std::size_t>(si)] <= 1e-9) {
					continue;
				}
				// Sem porto, a cidade só compra dentro da própria massa de
				// terra: vendedores do outro lado do mar não são candidatos.
				if (buyer_port < 0
						&& !same_landmass_city(
							ws_.cities[static_cast<std::size_t>(si)],
							*buyer.city)) {
					continue;
				}
				// Distância calculada UMA vez por entrada do anel: o
				// `partial_sort` da liquidação reusa este valor (mesmos
				// números ⇒ mesma permutação).
				out.push_back(FoodRingEntry{si, radius,
					distance_km(ws_.cities[static_cast<std::size_t>(si)].center,
						buyer.city->center)});
			}
		};
		next_radius = start_radius;
		for (int radius = start_radius; radius <= max_radius && cells_budget > 0;
				++radius) {
			if (static_cast<int>(out.size()) >= wanted_count) {
				if (overshoot_radii <= 0) {
					break;
				}
				--overshoot_radii;
			}
			// Só o PERÍMETRO do anel (4 lados, cantos uma vez só): O(raio)
			// por anel em vez de O(raio²) com `continue` nas células internas.
			for (int ix = bx - radius; ix <= bx + radius; ++ix) {
				scan_cell(ix, by - radius, radius);
				if (radius > 0) {
					scan_cell(ix, by + radius, radius);
				}
			}
			for (int iy = by - radius + 1; iy <= by + radius - 1; ++iy) {
				scan_cell(bx - radius, iy, radius);
				if (radius > 0) {
					scan_cell(bx + radius, iy, radius);
				}
			}
			if (cells_budget > 0) {
				// Raio inteiro visitado dentro do orçamento: a retomada
				// começa no próximo.
				next_radius = radius + 1;
			}
		}
		// `false` só quando o teto de entradas do plano cortou o anel.
		complete = static_cast<int>(out.size()) < wanted_count;
		return cells_budget;
	};

	// A7b — onde a varredura do plano de cada comprador parou (raio e orçamento
	// restante), para a liquidação CONTINUAR a passada em vez de refazê-la.
	std::vector<FoodScanTail> scan_tails;

	{
	OGS_SIM_ZONE(kPhaseTradeFoodBuyers);
	{
	OGS_SIM_ZONE(kPhaseTradeFoodCalc);
	// Um plano só-leitura por comprador do dia. Redimensiona quando o número de
	// compradores muda; cada plano REUSA os vetores do dia anterior.
	if (food_plans_.size() < buyers.size()) {
		food_plans_.resize(buyers.size());
	}
	// Fronteira de retomada da varredura (A7b), por comprador do dia. É scratch
	// local desta chamada — cada índice é escrito por um único worker.
	scan_tails.resize(buyers.size());
	// Partição estática por comprador: cada worker escreve só no seu plano, e o
	// bloco de cálculo NÃO escreve em `ws_`. Mundos pequenos continuam seriais
	// (`parallel_for` já trata `< kThreadPoolMinCities`).
	pool().parallel_for(buyers.size(), [&](std::size_t begin, std::size_t end) {
		for (std::size_t bi = begin; bi < end; ++bi) {
			const FoodBuyer& buyer = buyers[bi];
			FoodTradePlan& plan = food_plans_[bi];
			FoodScanTail& tail = scan_tails[bi];
			tail.radius = 0;
			tail.budget = 0;
			plan.active = false;
			plan.exhaustive = exhaustive;
			plan.ring_complete = true;
			plan.ring.clear();
			plan.ext.clear();
			plan.ext_sorted.clear();
			plan.offers.clear();
			plan.universe.clear();

			// Rota possível? Na mesma massa de terra, caminhão direto; entre
			// massas, só com porto nas duas pontas. Comprador sem nenhum
			// vendedor alcançável não gera oferta: sair aqui evita varrer a
			// grade inteira por dia.
			const int buyer_port = city_port[static_cast<std::size_t>(buyer.index)];
			const int buyer_landmass = buyer.city->landmass_index;
			const int local_sellers = buyer_landmass >= 0
				? sellers_in_landmass[static_cast<std::size_t>(buyer_landmass)] : 0;
			const int local_port_sellers = buyer_landmass >= 0
				? port_sellers_in_landmass[static_cast<std::size_t>(buyer_landmass)] : 0;
			const int reachable_sellers = local_sellers + (buyer_port >= 0
				? port_sellers_total - local_port_sellers : 0);
			if (reachable_sellers <= 0) {
				continue;
			}
			plan.active = true;

			if (exhaustive) {
				// Mundo pequeno: todas as cidades vendedoras em ordem crescente,
				// sem partial_sort e sem mercado externo (como hoje).
				plan.ring.reserve(city_count);
				for (std::size_t si = 0; si < city_count; ++si) {
					if (static_cast<int>(si) != buyer.index && sellable_ge[si] > 1e-9) {
						plan.ring.push_back(
							FoodRingEntry{static_cast<int>(si), 0, 0.0});
					}
				}
			} else {
				// O plano guarda `min(k_neighbors, reachable)` entradas MAIS a
				// folga: um superconjunto do que a liquidação consome, mesmo que
				// parte dos vendedores se esgote durante o dia.
				const int wanted_max = std::min(k_neighbors, reachable_sellers);
				int next_radius = 0;
				tail.budget = scan_ring(buyer, wanted_max + kFoodPlanSlack,
					0 /* para na fronteira */, 0 /* desde o raio 0 */,
					kFoodScanBudget, next_radius, plan.ring_complete, plan.ring);
				tail.radius = next_radius;
				// Mercado externo: as M ofertas mais baratas por caloria de cada
				// alimento da cesta (o resto do mundo, do ponto de vista do
				// comprador).
				for (int slot = 0;
						slot < static_cast<int>(ws_.food_products.size()); ++slot) {
					const std::vector<int>& list =
						offers_by_slot[static_cast<std::size_t>(slot)];
					const int limit = std::min(k_external, static_cast<int>(list.size()));
					for (int i = 0; i < limit; ++i) {
						const int si = list[static_cast<std::size_t>(i)];
						if (si != buyer.index) {
							plan.ext.push_back(si);
						}
					}
				}
				plan.ext_sorted.assign(plan.ext.begin(), plan.ext.end());
				std::sort(plan.ext_sorted.begin(), plan.ext_sorted.end());
				plan.ext_sorted.erase(
					std::unique(plan.ext_sorted.begin(), plan.ext_sorted.end()),
					plan.ext_sorted.end());
			}

			// Universo do plano na MESMA ordem de entrada do laço de hoje:
			// crescente por índice de vendedor e, dentro dele, slots da cesta.
			plan.universe.reserve(plan.ring.size() + plan.ext_sorted.size());
			for (const FoodRingEntry& entry : plan.ring) {
				plan.universe.push_back(entry.seller);
			}
			plan.universe.insert(plan.universe.end(), plan.ext_sorted.begin(),
				plan.ext_sorted.end());
			std::sort(plan.universe.begin(), plan.universe.end());
			plan.universe.erase(
				std::unique(plan.universe.begin(), plan.universe.end()),
				plan.universe.end());
			for (const int si : plan.universe) {
				append_seller_offers(buyer, si, plan.offers);
			}
			// A7c: o plano já guarda as ofertas na ORDEM DE CUSTO do dia. O
			// `stable_sort` que a liquidação serial fazia por comprador roda aqui,
			// uma vez, na fase PARALELA e só-leitura (a liquidação só percorre e
			// filtra). É o mesmo `stable_sort` sobre a mesma entrada (vendedor
			// crescente, depois slots da cesta), agora sobre o universo do plano —
			// filtrar depois um prefixo de candidatos preserva a ordem relativa
			// dos empates, que é tudo o que o `stable_sort` de hoje usa para
			// desempatar (ver o comentário do caminho rápido da liquidação).
			std::stable_sort(plan.offers.begin(), plan.offers.end(),
				[](const FoodOffer& left, const FoodOffer& right) {
					return left.unit_cost_per_ge < right.unit_cost_per_ge;
				});
		}
	});
	}

	{
	OGS_SIM_ZONE(kPhaseTradeFoodSettle);
	// A7c: marcas de candidato do dia. Uma passada O(cidades) por dia; depois,
	// `food_candidate_stamp_[vendedor] == bi + 1` responde O(1) "é candidato
	// deste comprador?" sem `binary_search` por oferta.
	if (food_candidate_stamp_.size() < city_count) {
		food_candidate_stamp_.resize(city_count, 0);
	}
	std::fill_n(food_candidate_stamp_.begin(), city_count, 0);
	for (std::size_t bi = 0; bi < buyers.size(); ++bi) {
		if (sellers_left <= 0) {
			break;  // todo o excedente do dia já foi vendido
		}
		const FoodBuyer& buyer = buyers[bi];
		const FoodTradePlan& plan = food_plans_[bi];
		if (!plan.active) {
			continue;
		}
		double remaining = buyer.gap;  // toneladas-equivalentes de grão

		// 1-2. Replay do anel do plano: percorre as entradas na ordem exata de
		// varredura, pula o vendedor já esgotado AGORA, conta os sobreviventes e
		// repete o teste `count >= wanted` a cada fronteira de raio (o `wanted`
		// do momento já embute `sellers_left`). Depois, o `partial_sort` pelas
		// distâncias JÁ calculadas (mesma comparação, mesma permutação).
		if (plan.exhaustive) {
			food_candidates_.clear();
			food_candidates_.reserve(plan.ring.size());
			for (const FoodRingEntry& entry : plan.ring) {
				if (sellable_ge[static_cast<std::size_t>(entry.seller)] > 1e-9) {
					food_candidates_.push_back(entry.seller);
				}
			}
		} else {
			const int buyer_port = city_port[static_cast<std::size_t>(buyer.index)];
			const int buyer_landmass = buyer.city->landmass_index;
			const int local_sellers = buyer_landmass >= 0
				? sellers_in_landmass[static_cast<std::size_t>(buyer_landmass)] : 0;
			const int local_port_sellers = buyer_landmass >= 0
				? port_sellers_in_landmass[static_cast<std::size_t>(buyer_landmass)] : 0;
			const int reachable_sellers = local_sellers + (buyer_port >= 0
				? port_sellers_total - local_port_sellers : 0);
			const int wanted = std::min(k_neighbors,
				std::min(sellers_left, reachable_sellers));
			food_ring_live_.clear();
			int count = 0;
			int current_radius = -1;
			bool stopped = false;
			for (const FoodRingEntry& entry : plan.ring) {
				if (entry.radius != current_radius) {
					// Fronteira de raio: é AQUI que o dia decide parar.
					if (count >= wanted) {
						stopped = true;
						break;
					}
					current_radius = entry.radius;
				}
				if (sellable_ge[static_cast<std::size_t>(entry.seller)] > 1e-9) {
					food_ring_live_.push_back(entry);
					++count;
				}
			}
			if (!stopped && !plan.ring_complete) {
				// O plano foi cortado pelo teto na MESMA fronteira de raio em que
				// a varredura dele parou, então o teste de hoje vale aqui também.
				if (count >= wanted) {
					stopped = true;
				} else {
					// O dia precisa de mais entradas do que o plano guardou:
					// CONTINUA a passada única de hoje a partir da fronteira em
					// que ela parou (A7b) — mesmas células, mesma ordem, mesmo
					// `wanted` do momento e o orçamento de células que sobrou do
					// plano. As entradas já acumuladas contam no teste
					// `out.size() >= wanted` do topo do laço, e o raio 0 nunca é
					// refeito. Candidato fora do universo do plano é detectado
					// pelo `all_planned` abaixo.
					const FoodScanTail& tail = scan_tails[bi];
					int next_radius = tail.radius;  // saídas da varredura:
					bool complete = true;           // não usadas aqui
					scan_ring(buyer, wanted, 0 /* para onde o dia pararia */,
						tail.radius, tail.budget, next_radius, complete,
						food_ring_live_);
				}
			}
			if (static_cast<int>(food_ring_live_.size()) > k_neighbors) {
				std::partial_sort(food_ring_live_.begin(),
					food_ring_live_.begin() + k_neighbors, food_ring_live_.end(),
					[](const FoodRingEntry& left, const FoodRingEntry& right) {
						return left.distance_km < right.distance_km;
					});
				food_ring_live_.resize(static_cast<std::size_t>(k_neighbors));
			}
			food_candidates_.clear();
			food_candidates_.reserve(food_ring_live_.size());
			for (const FoodRingEntry& entry : food_ring_live_) {
				food_candidates_.push_back(entry.seller);
			}
		}
		// 3. União ascendente e única com o mercado externo pré-ordenado.
		std::sort(food_candidates_.begin(), food_candidates_.end());
		food_candidates_union_.clear();
		std::set_union(food_candidates_.begin(), food_candidates_.end(),
			plan.ext_sorted.begin(), plan.ext_sorted.end(),
			std::back_inserter(food_candidates_union_));

		// 4. Ofertas do dia, na ordem de custo — que o PLANO já tem.
		// O plano cobre o universo do comprador? (vendedor sem oferta nenhuma
		// simplesmente não tem oferta no plano — isso NÃO é plano cortado.) A
		// retomada da varredura pode trazer vendedor fora do universo do plano;
		// aí as ofertas são montadas pela MESMA função, na mesma ordem.
		bool all_planned = true;
		for (const int si : food_candidates_union_) {
			if (!std::binary_search(plan.universe.begin(), plan.universe.end(), si)) {
				all_planned = false;
				break;
			}
		}
		// `food_offers_live_` é a lista de ofertas do comprador na ordem em que o
		// laço serial de hoje as percorre, por PONTEIRO: o corpo da liquidação
		// (abaixo, byte a byte o de sempre) roda uma vez só, servindo aos dois
		// caminhos.
		food_offers_live_.clear();
		if (all_planned) {
			// Caminho normal (A7c): NÃO copia bloco nem ordena. `plan.offers` já
			// está na ordem de custo do dia (o `stable_sort` foi pago na fase
			// paralela); aqui é só percorrer e filtrar pela marca de candidato do
			// comprador — O(1) por oferta, sem `binary_search`.
			//
			// Bit-exatidão: hoje o vetor é montado na ordem de candidato
			// ascendente (vendedor crescente, slots da cesta) e ordenado por
			// `stable_sort` pelo custo. O plano guarda as ofertas do UNIVERSO do
			// comprador naquela mesma ordem ascendente de vendedor e recebeu o
			// MESMO `stable_sort`; como o conjunto de candidatos é um subconjunto
			// do universo e ambos estão em ordem ascendente de índice,
			// `candidatos` é uma SUBSEQUÊNCIA de `universo`. `stable_sort` é
			// estável e só compara custo: filtrar a lista já ordenada por custo
			// pela pertinência ao subconjunto preserva a ordem relativa de TODOS
			// os pares de ofertas (para custos diferentes a ordem vem do custo;
			// para custos iguais vem da entrada, e a sub-sequência mantém a
			// entrada). Logo o resultado é a mesma sequência que o `stable_sort`
			// de hoje produz sobre os candidatos.
			const int stamp = static_cast<int>(bi) + 1;
			for (const int si : food_candidates_union_) {
				food_candidate_stamp_[static_cast<std::size_t>(si)] = stamp;
			}
			food_offers_live_.reserve(plan.offers.size());
			for (const FoodOffer& offer : plan.offers) {
				if (food_candidate_stamp_[static_cast<std::size_t>(offer.seller_index)]
						== stamp) {
					food_offers_live_.push_back(&offer);
				}
			}
		} else {
			// Caminho lento (raro): monta as ofertas na ordem de candidato com a
			// MESMA função (mesmos números) e faz o `stable_sort` do dia — como
			// hoje. Nada de merge com o que o plano tinha em ordem de custo.
			food_offers_settle_.clear();
			food_offers_settle_.reserve(
				food_candidates_union_.size() * ws_.food_products.size());
			for (const int si : food_candidates_union_) {
				append_seller_offers(buyer, si, food_offers_settle_);
			}
			// Ordem de custo do dia: o `stable_sort` do laço serial original. O
			// empate preserva a ordem de entrada (vendedor crescente, depois slots
			// da cesta), que é a ordem de montagem acima.
			std::stable_sort(food_offers_settle_.begin(), food_offers_settle_.end(),
				[](const FoodOffer& left, const FoodOffer& right) {
					return left.unit_cost_per_ge < right.unit_cost_per_ge;
				});
			food_offers_live_.reserve(food_offers_settle_.size());
			for (const FoodOffer& offer : food_offers_settle_) {
				food_offers_live_.push_back(&offer);
			}
		}

		// 5. Liquidação: o laço de sempre (orçamento, frota, porto, pedidos).
		for (const FoodOffer* const offer_ptr : food_offers_live_) {
			const FoodOffer& offer = *offer_ptr;
			if (remaining <= 1e-9 || shipments >= kMaxShipmentsPerDay) {
				break;
			}
			const int si = offer.seller_index;
			City* seller = &ws_.cities[si];
			const int product = offer.product;
			const double factor = offer.factor;
			const double origin_anchor = offer.origin_anchor;
			const double unit_freight = offer.unit_freight;
			const double unit_tariff = offer.unit_tariff;
			const double unit_cost = origin_anchor + unit_freight + unit_tariff;
			// Vendável recalculado (o excedente total cai a cada embarque).
			const double sellable_tons = std::min(
				seller->markets[product].stock, sellable_ge[si] / factor);
			if (sellable_tons <= 1e-9) {
				continue;
			}

			// Teto de carga diária (capacidade de transporte), em ton-equiv.
			const double capacity_ge =
				buyer.city->simulation_population * kTradeCapacityPerCapita;
			double imported_ge = 0.0;
			for (int slot = 0; slot < static_cast<int>(ws_.food_products.size()); ++slot) {
				const int p = food_product(slot);
				imported_ge += buyer.city->imported[p] * food_calories(p);
			}
			const double room_ge = std::max(0.0, capacity_ge - imported_ge);
			const double amount_wanted_ge = std::min(
				remaining, std::min(sellable_tons * factor, room_ge));
			if (amount_wanted_ge <= 1e-9) {
				continue;
			}

			// Segurança alimentar: o governo subsidia a parte da importação que a
			// firma não consegue pagar (cidade sem produção/caixa). Conserva
			// dinheiro — firma + governo pagam exatamente o custo (carga + frete).
			// As FAMÍLIAS são as compradoras finais: quando o capital de giro da
			// firma e o orçamento público não cobrem a carga, a poupança das
			// famílias entra como pagamento. O alimento chega ao mercado e é
			// comprado por elas no dia seguinte, devolvendo o caixa à firma — é
			// o mesmo dinheiro, não uma transferência nova. Sem isso, uma cidade
			// com poupança alta e firma sem caixa (São Paulo, 28 M) ficava com
			// fome enquanto o dinheiro dormia na conta das famílias (§19).
			const double firm_cash = std::max(0.0, buyer.city->firm_liquidity());
			const double gov_budget = std::max(0.0, buyer.city->government_budget);
			const double savings = std::max(0.0, buyer.city->pop.savings);
			const double payable = firm_cash + gov_budget + savings;
			double amount_tons = amount_wanted_ge / factor;
			if (amount_tons * unit_cost > payable) {
				amount_tons = unit_cost > 0.0 ? payable / unit_cost : 0.0;
			}

			if (amount_tons <= 1e-9) {
				continue;
			}
			double ge = std::min(amount_tons * factor, remaining);
			amount_tons = ge / factor;

			// F4: frota livre + combustível limitam o embarque. Sem um dos dois,
			// a carga não sai (fica na fila da nação vendedora, que é a dona do
			// veículo); com frota parcial, sai proporcional. Portos/fila/
			// transbordo continuam sendo os existentes.
			const bool fleet_same_landmass =
				same_landmass_city(*seller, *buyer.city);
			TransportDemand transport = route_demand(*seller, *buyer.city,
				fleet_same_landmass ? -1 : city_port[static_cast<std::size_t>(si)],
				fleet_same_landmass ? -1 : city_port[buyer.index], amount_tons);
			bool fuel_limited = false;
			const double fleet_scale = fleet_capacity_scale(
				seller->nation, transport, fuel_limited);
			if (fleet_scale < 1.0) {
				const double queued_tons = amount_tons * (1.0 - fleet_scale);
				const auto fleet_found = ws_.fleets.find(seller->nation);
				if (fleet_found != ws_.fleets.end()) {
					fleet_found->second.queue_today += queued_tons;
					if (fuel_limited) {
						fleet_found->second.missed_no_fuel += queued_tons;
					} else {
						fleet_found->second.missed_no_fleet += queued_tons;
					}
				}
				if (fleet_scale <= 1e-9) {
					continue;
				}
				amount_tons *= fleet_scale;
				ge = amount_tons * factor;
				transport = route_demand(*seller, *buyer.city,
					fleet_same_landmass ? -1 : city_port[static_cast<std::size_t>(si)],
					fleet_same_landmass ? -1 : city_port[buyer.index], amount_tons);
			}

			const double total_cost = amount_tons * unit_cost;
			const double firm_share = std::min(total_cost, firm_cash);
			const double gov_share = std::min(total_cost - firm_share, gov_budget);
			const double savings_share = std::max(
				0.0, total_cost - firm_share - gov_share);
			const double cost = amount_tons * origin_anchor;
			const double freight = unit_freight * amount_tons;
			const double tariff_paid = unit_tariff * amount_tons;

			buyer.city->firm_pay(firm_share);
			buyer.city->government_budget -= gov_share;
			// B2: a parcela da importação paga pela poupança sai rateada pelos
			// quintis (a soma dos estratos acompanha o escalar).
			if (savings_share > 0.0) {
				charge_households(buyer.city->pop, savings_share);
			}
			seller->firm_receive(cost);
			buyer.city->service_receive(freight);
			buyer.city->freight_revenue += freight;
			// Tarifa: paga pelo comprador e recebida pelo próprio governo
			// (dinheiro conservado; receita publicada no status).
			buyer.city->government_budget += tariff_paid;
			buyer.city->tariff_revenue += tariff_paid;

			seller->markets[product].stock -= amount_tons;
			seller->markets[product].desired += amount_tons;
			seller->markets[product].demand += amount_tons;
			seller->markets[product].sold += amount_tons;
			seller->markets[product].effective += amount_tons;
			seller->revenue_by_product[product] += cost;
			seller->exported[product] += amount_tons;
			seller->export_value[product] += cost;

			buyer.city->imported[product] += amount_tons;
			buyer.city->import_value[product] += cost + freight;

			// A2: mesma expressão já calculada acima (`fleet_same_landmass`);
			// os dois lados são puros no laço, então o valor é idêntico.
			const bool same_landmass = fleet_same_landmass;

			// Pedido de comércio para os comboios 3D e o HUD. Na mesma massa de
			// terra vai de caminhão em linha reta; entre massas, o caminhão
			// termina no porto nacional, o navio liga os dois cais e outro
			// caminhão entrega na cidade (infraestrutura nacional: o porto serve
			// a nação toda, não só a cidade-sede).
			TradeOrder order;
			order.from_city = seller->id;
			order.to_city = buyer.city->id;
			order.from_nation = seller->nation;
			order.to_nation = buyer.city->nation;
			const double lane_km = distance_km(seller->center, buyer.city->center);
			order.scope = (seller->nation == buyer.city->nation
				|| lane_km <= kRegionalRadiusKm)
				? MarketScope::Regional : MarketScope::International;
			order.product = product;
			order.amount = amount_tons;
			order.origin_price = origin_anchor
				/ std::max(1e-9, city_fx[static_cast<std::size_t>(si)]);
			order.origin_price_anchor = origin_anchor;
			order.tariff = tariff_paid;
			order.delivered_price = offer.delivered;
			order.fx_rate = city_fx[static_cast<std::size_t>(buyer.index)];
			order.seller_fx_rate = city_fx[static_cast<std::size_t>(si)];
			order.buyer_currency = currency_code(buyer.city->nation);
			order.seller_currency = currency_code(seller->nation);
			// Cotação que forma o preço interno na ÂNCORA: valor CIF do BEM
			// (origem efetiva + tarifa), sem o frete-serviço. O frete fica de
			// fora de propósito — ele é um serviço doméstico do comprador (pago
			// ao `service_cash` dele, contado à parte em `import_value`), não
			// valor de mercado do bem. Incluí-lo levava qualquer importador
			// remoto ao teto de 3× o preço-base e triplicava o custo alimentar
			// em EGY/MRT/SDN (365 d, `economy_world_test`).
			buyer.city->import_price[product] = origin_anchor + unit_tariff;

			double total_transit = 0.0;
			double total_freight_cost = 0.0;
			double port_delay_days = 0.0;
			if (same_landmass) {
				Leg leg = make_leg(TransportMode::Truck, seller->id, buyer.city->id,
					seller->center, buyer.city->center, amount_tons);
				total_transit = leg.transit_hours;
				total_freight_cost = leg.cost;
				order.legs.push_back(std::move(leg));
			} else {
				const int seller_port_index = city_port[static_cast<std::size_t>(si)];
				const int buyer_port_index = city_port[buyer.index];
				// A oferta só entra na lista com os dois portos (ver o bloco de
				// ofertas); checar aqui é rede de segurança, não caminho normal.
				if (seller_port_index >= 0 && buyer_port_index >= 0) {
					Port& seller_port =
						ws_.ports[static_cast<std::size_t>(seller_port_index)];
					Port& buyer_port =
						ws_.ports[static_cast<std::size_t>(buyer_port_index)];
					// Cidade -> porto do vendedor (caminhão).
					if (distance_km(seller->center, seller_port.center) > 1.0) {
						Leg leg = make_leg(TransportMode::Truck, seller->id,
							seller_port.id, seller->center, seller_port.center, amount_tons);
						total_transit += leg.transit_hours;
						total_freight_cost += leg.cost;
						order.legs.push_back(std::move(leg));
					}
					// Porto -> porto (navio): a única travessia marítima.
					{
						Leg leg = make_leg(TransportMode::Ship, seller_port.id,
							buyer_port.id, seller_port.center, buyer_port.center, amount_tons);
						total_transit += leg.transit_hours;
						total_freight_cost += leg.cost;
						order.legs.push_back(std::move(leg));
					}
					// Porto do comprador -> cidade (caminhão).
					if (distance_km(buyer_port.center, buyer.city->center) > 1.0) {
						Leg leg = make_leg(TransportMode::Truck, buyer_port.id,
							buyer.city->id, buyer_port.center, buyer.city->center, amount_tons);
						total_transit += leg.transit_hours;
						total_freight_cost += leg.cost;
						order.legs.push_back(std::move(leg));
					}
					// Transbordo nos dois cais + fila do dia. O atraso de fila
					// atrasa a CHEGADA em dias inteiros; o demurrage é pago pelo
					// orçamento público do comprador ao setor de frete dele
					// (dinheiro conservado; sem verba, a fila só atrasa).
					total_transit += 2.0 * kPortHandlingHours;
					double demurrage = 0.0;
					port_delay_days +=
						port_charge(seller_port, amount_tons, demurrage);
					port_delay_days +=
						port_charge(buyer_port, amount_tons, demurrage);
					const double demurrage_paid = std::min(
						demurrage, std::max(0.0, buyer.city->government_budget));
					buyer.city->government_budget -= demurrage_paid;
					buyer.city->service_receive(demurrage_paid);
					buyer.city->freight_revenue += demurrage_paid;
					total_freight_cost += demurrage_paid;
					total_transit += port_delay_days * kHoursPerDay;
				}
			}
			order.transit_hours = total_transit;
			order.freight_cost = total_freight_cost;
			ws_.last_orders.push_back(std::move(order));

			const int arrival_day =
				ws_.day + 1 + static_cast<int>(std::ceil(port_delay_days));
			// F4: reserva os veículos até a chegada e debita o combustível.
			// `transport` já é o embarque do lote final (recalculado acima quando
			// a frota limitou a carga; senão, o cálculo inicial) — a 3ª chamada
			// era idêntica e só repetia o haversine. Bit-idêntico.
			fleet_reserve(seller->nation, transport);
			Shipment shipment;
			shipment.to_city = buyer.city->id;
			shipment.product = product;
			shipment.amount = amount_tons;
			shipment.arrival_day = arrival_day;
			shipment.fleet_nation = seller->nation;
			// Caminhões fazem só a coleta/entrega no cais e voltam no mesmo
			// dia; a travessia marítima fica com o navio. Sem isso, o caminhão
			// ficava preso até a chegada e um navio de 2.000 t exigia 80
			// caminhões ocupados por toda a viagem — nenhuma nação pequena
			// conseguia embarcar (ver phase_fleet).
			shipment.fleet_trucks = fleet_same_landmass ? transport.trucks : 0.0;
			shipment.fleet_ships = transport.ships;
			shipment.fleet_planes = transport.planes;
			shipment.diesel = transport.diesel;
			shipment.jet_fuel = transport.jet_fuel;
			ws_.in_transit.push_back(std::move(shipment));

			sellable_ge[si] -= ge;
			if (sellable_ge[si] <= 1e-9) {
				// Esgotou no meio do dia: sai da contagem usada para limitar a
				// expansão do anel dos próximos compradores.
				--sellers_left;
			}
			remaining -= ge;
			++shipments;
		}
		}
		}
	}
}
void SimCore::trade_goods() {
	// Comércio inter-cidades de TODOS os bens não alimentares (insumos e bens
	// industriais). A cesta alimentar continua no trade_food (validado);
	// aqui a demanda é a necessidade DIÁRIA das construções locais (receitas ou
	// insumo legado). Reaproveita a mesma malha de rotas/frete/portos do
	// comércio de alimento: caminhão na mesma massa de terra, navio entre
	// massas com porto nacional nas duas pontas.
	if (ws_.cities.empty() || ws_.products <= 0) {
		return;
	}
	OGS_SIM_ZONE(kPhaseTradeGoods);
	// Zonas acumuladoras por comprador (A7): zeram antes do laço do dia.
	OGS_SIM_PHASE_RESET(kPhaseTradeGoodsCand);
	OGS_SIM_PHASE_RESET(kPhaseTradeGoodsOffers);
	OGS_SIM_PHASE_RESET(kPhaseTradeGoodsSettle);
	const std::size_t city_count = ws_.cities.size();
	// Necessidade diária de insumo por cidade/produto (capacidade nominal,
	// não a produção do dia: o estoque-alvo é o que mantém a fábrica operando).
	std::vector<double> city_demand(city_count * static_cast<std::size_t>(ws_.products), 0.0);
	for (std::size_t ci = 0; ci < city_count; ++ci) {
		const City& city = ws_.cities[ci];
		for (const int bi : ws_.buildings_by_city[ci]) {
			const Building& building = ws_.buildings[static_cast<std::size_t>(bi)];
			const double capacity = building.capacity * building.output_scale;
			if (capacity <= 0.0) {
				continue;
			}
			if (building.recipe >= 0
					&& building.recipe < static_cast<int>(ws_.recipes.size())) {
				const Recipe& recipe =
					ws_.recipes[static_cast<std::size_t>(building.recipe)];
				for (int slot = 0; slot < recipe.input_count; ++slot) {
					const int product = recipe.inputs[slot];
					if (product < 0 || product >= ws_.products) {
						continue;
					}
					city_demand[ci * static_cast<std::size_t>(ws_.products)
						+ static_cast<std::size_t>(product)] +=
						capacity * recipe.input_qty[slot] / recipe.yield;
				}
			} else if (building.input_product >= 0
					&& building.input_product < ws_.products) {
				city_demand[ci * static_cast<std::size_t>(ws_.products)
					+ static_cast<std::size_t>(building.input_product)] +=
					capacity * building.input_per_output;
			}
		}
		(void)city;
	}
	// Trânsito a caminho da cidade, por produto.
	std::vector<double> in_transit(city_demand.size(), 0.0);
	for (const Shipment& shipment : ws_.in_transit) {
		const auto found = ws_.city_lookup.find(shipment.to_city);
		if (found == ws_.city_lookup.end()) {
			continue;
		}
		const std::size_t offset =
			static_cast<std::size_t>(found->second) * static_cast<std::size_t>(ws_.products)
			+ static_cast<std::size_t>(shipment.product);
		if (offset < in_transit.size()) {
			in_transit[offset] += shipment.amount;
		}
	}
	// Porto nacional de cada cidade (mesma nação + massa de terra). Sem porto,
	// a cidade só comercia por caminhão dentro da própria massa.
	std::vector<int> city_port(city_count, -1);
	if (!ws_.ports.empty()) {
		std::unordered_map<std::string, std::vector<int>> ports_by_region;
		ports_by_region.reserve(ws_.ports.size() * 2);
		for (std::size_t pi = 0; pi < ws_.ports.size(); ++pi) {
			const Port& port = ws_.ports[pi];
			ports_by_region[port.nation + '\n' + port.landmass]
				.push_back(static_cast<int>(pi));
		}
		for (std::size_t ci = 0; ci < city_count; ++ci) {
			const City& city = ws_.cities[ci];
			const auto found = ports_by_region.find(city.nation + '\n' + city.landmass);
			if (found == ports_by_region.end()) {
				continue;
			}
			int best_port = -1;
			double best_distance = 0.0;
			for (const int pi : found->second) {
				const double distance = distance_km(
					city.center, ws_.ports[static_cast<std::size_t>(pi)].center);
				if (best_port < 0 || distance < best_distance) {
					best_port = pi;
					best_distance = distance;
				}
			}
			city_port[ci] = best_port;
		}
	}
	// Grade espacial (~250 km por célula) para os K vizinhos.
	const double cell = kTradeCellMapUnits;
	const int grid_w = std::max(1, static_cast<int>(std::ceil(kMapWidth / cell)));
	const int grid_h = std::max(1, static_cast<int>(std::ceil(kMapHeight / cell)));
	std::vector<std::vector<int>> grid(static_cast<std::size_t>(grid_w * grid_h));
	auto cell_index = [cell, grid_w, grid_h](const MapPoint& point) {
		const int ix = std::clamp(static_cast<int>(point.x / cell), 0, grid_w - 1);
		const int iy = std::clamp(static_cast<int>(point.y / cell), 0, grid_h - 1);
		return iy * grid_w + ix;
	};
	for (std::size_t ci = 0; ci < city_count; ++ci) {
		grid[static_cast<std::size_t>(cell_index(ws_.cities[ci].center))]
			.push_back(static_cast<int>(ci));
	}
	const int k_neighbors = std::max(1, ws_.trade_neighbors);
	const int k_external = std::max(0, ws_.trade_external);
	std::size_t shipments = 0;
	{
		OGS_SIM_ZONE(kPhaseTradeGoodsLoop);
		// Itera só os produtos com demanda E oferta no dia (nunca 114 × cidades).
		for (int product = 0; product < ws_.products && shipments < kMaxShipmentsPerDay;
				++product) {
		if (food_calories(product) > 0.0) {
			continue;  // cesta alimentar: trade_food
		}
		struct Buyer {
			int city_index;
			double gap;
			double daily;
		};
		struct Seller {
			int city_index;
			double sellable;
			double price;
		};
		std::vector<Buyer> buyers;
		std::vector<Seller> sellers;
		for (std::size_t ci = 0; ci < city_count; ++ci) {
			const std::size_t offset = ci * static_cast<std::size_t>(ws_.products)
				+ static_cast<std::size_t>(product);
			const double daily = city_demand[offset];
			const Market& market = ws_.cities[ci].markets[product];
			const double stock = std::max(0.0, market.stock);
			if (daily > 1e-9) {
				const double target = daily * ws_.inventory_cover_days;
				const double gap = target - (stock + in_transit[offset]);
				if (gap > 1e-9) {
					buyers.push_back({static_cast<int>(ci), gap, daily});
				}
			}
			// Vendedor: estoque acima da própria necessidade de 1 dia.
			const double own_need = daily;
			const double sellable = stock - own_need;
			if (sellable > 1e-9 && market.price > 0.0) {
				sellers.push_back({static_cast<int>(ci), sellable,
					market.price * competitiveness(ws_.cities[ci].nation)});
			}
		}
		if (buyers.empty() || sellers.empty()) {
			continue;
		}
		std::stable_sort(buyers.begin(), buyers.end(),
			[](const Buyer& left, const Buyer& right) {
				return left.gap / std::max(1e-9, left.daily)
					> right.gap / std::max(1e-9, right.daily);
			});
		// Ofertas globais mais baratas: o "mercado externo" do comprador.
		std::vector<int> cheapest(sellers.size());
		for (std::size_t i = 0; i < cheapest.size(); ++i) {
			cheapest[i] = static_cast<int>(i);
		}
		std::stable_sort(cheapest.begin(), cheapest.end(),
			[&sellers](int left, int right) {
				return sellers[static_cast<std::size_t>(left)].price
					< sellers[static_cast<std::size_t>(right)].price;
			});
		for (Buyer& buyer : buyers) {
			if (shipments >= kMaxShipmentsPerDay) {
				break;
			}
			City& buyer_city = ws_.cities[static_cast<std::size_t>(buyer.city_index)];
			const std::size_t buyer_offset =
				static_cast<std::size_t>(buyer.city_index) * static_cast<std::size_t>(ws_.products)
				+ static_cast<std::size_t>(product);
			double remaining = std::max(0.0, buyer.gap);
			if (remaining <= 1e-9) {
				continue;
			}
			// Candidatos: K vizinhos vendedores + M ofertas mais baratas.
			std::vector<int> candidates;
			{
			OGS_SIM_ZONE_ADD(kPhaseTradeGoodsCand);
			const int bx = std::clamp(
				static_cast<int>(buyer_city.center.x / cell), 0, grid_w - 1);
			const int by = std::clamp(
				static_cast<int>(buyer_city.center.y / cell), 0, grid_h - 1);
			const int max_radius = std::min(8,
				std::max(std::max(bx, grid_w - 1 - bx), std::max(by, grid_h - 1 - by)));
			int cells_budget = std::min(grid_w * grid_h, 512);
			for (int radius = 0; radius <= max_radius && cells_budget > 0; ++radius) {
				if (static_cast<int>(candidates.size()) >= k_neighbors) {
					break;
				}
				for (int ix = bx - radius; ix <= bx + radius; ++ix) {
					for (int iy = by - radius; iy <= by + radius; ++iy) {
						if (ix != bx - radius && ix != bx + radius
								&& iy != by - radius && iy != by + radius) {
							continue;
						}
						if (ix < 0 || ix >= grid_w || iy < 0 || iy >= grid_h
								|| cells_budget <= 0) {
							continue;
						}
						--cells_budget;
						for (const int si : grid[static_cast<std::size_t>(iy * grid_w + ix)]) {
							if (si == buyer.city_index) {
								continue;
							}
							const std::size_t offset =
								static_cast<std::size_t>(si) * static_cast<std::size_t>(ws_.products)
								+ static_cast<std::size_t>(product);
							if (city_demand[offset] >= 0.0
									&& ws_.cities[static_cast<std::size_t>(si)]
										.markets[product].stock
										- city_demand[offset] > 1e-9) {
								candidates.push_back(si);
							}
						}
					}
				}
			}
			const int external_limit = std::min(k_external,
				static_cast<int>(cheapest.size()));
			for (int i = 0; i < external_limit; ++i) {
				const int si = sellers[static_cast<std::size_t>(
					cheapest[static_cast<std::size_t>(i)])].city_index;
				if (si != buyer.city_index) {
					candidates.push_back(si);
				}
			}
			std::sort(candidates.begin(), candidates.end());
			candidates.erase(std::unique(candidates.begin(), candidates.end()),
				candidates.end());
			}
			struct Offer {
				int seller_index;
				double unit_cost;      // âncora: origem + frete + tarifa (ordena)
				double unit_freight;
				double origin_anchor;  // origem efetiva na âncora
				double unit_tariff;
				double delivered;      // moeda do comprador (exibição)
			};
			std::vector<Offer> offers;
			{
			OGS_SIM_ZONE_ADD(kPhaseTradeGoodsOffers);
			offers.reserve(candidates.size());
			for (const int si : candidates) {
				City& seller = ws_.cities[static_cast<std::size_t>(si)];
				const std::size_t offset =
					static_cast<std::size_t>(si) * static_cast<std::size_t>(ws_.products)
					+ static_cast<std::size_t>(product);
				if (seller.markets[product].stock - city_demand[offset] <= 1e-9) {
					continue;
				}
				double unit_freight = 0.0;
				if (same_landmass_city(seller, buyer_city)) {
					unit_freight = leg_unit_freight(TransportMode::Truck,
						distance_km(seller.center, buyer_city.center));
				} else {
					const int seller_port = city_port[static_cast<std::size_t>(si)];
					const int buyer_port = city_port[static_cast<std::size_t>(buyer.city_index)];
					if (seller_port < 0 || buyer_port < 0) {
						continue;
					}
					const Port& sp = ws_.ports[static_cast<std::size_t>(seller_port)];
					const Port& bp = ws_.ports[static_cast<std::size_t>(buyer_port)];
					unit_freight =
						leg_unit_freight(TransportMode::Truck,
							distance_km(seller.center, sp.center))
						+ leg_unit_freight(TransportMode::Ship,
							distance_km(sp.center, bp.center))
						+ leg_unit_freight(TransportMode::Truck,
							distance_km(bp.center, buyer_city.center));
				}
				const double origin_anchor = seller.markets[product].price
					* competitiveness(seller.nation);
				const double tariff_factor =
					tariff_rate_cities(buyer_city, seller, product);
				const double unit_tariff =
					(origin_anchor + unit_freight) * tariff_factor;
				const double delivered_anchor =
					origin_anchor + unit_freight + unit_tariff;
				const double buyer_fx = fx_rate(buyer_city.nation);
				const double delivered =
					delivered_anchor / std::max(1e-9, buyer_fx);
				offers.push_back({si, delivered_anchor,
					unit_freight, origin_anchor, unit_tariff,
					delivered});
			}
			std::stable_sort(offers.begin(), offers.end(),
				[](const Offer& left, const Offer& right) {
					return left.unit_cost < right.unit_cost;
				});
			}
			{
			OGS_SIM_ZONE_ADD(kPhaseTradeGoodsSettle);
			for (const Offer& offer : offers) {
				if (remaining <= 1e-9 || shipments >= kMaxShipmentsPerDay) {
					break;
				}
				City& seller = ws_.cities[static_cast<std::size_t>(offer.seller_index)];
				const std::size_t seller_offset =
					static_cast<std::size_t>(offer.seller_index) * static_cast<std::size_t>(ws_.products)
					+ static_cast<std::size_t>(product);
				const double sellable = std::max(0.0,
					seller.markets[product].stock - city_demand[seller_offset]);
				if (sellable <= 1e-9) {
					continue;
				}
				// Capacidade de transporte do comprador (mesmo teto do alimento).
				double imported_total = 0.0;
				for (int p = 0; p < ws_.products; ++p) {
					imported_total += buyer_city.imported[p];
				}
				const double room = std::max(0.0,
					buyer_city.simulation_population * kTradeCapacityPerCapita
					- imported_total);
				double amount = std::min(remaining, std::min(sellable, room));
				if (amount <= 1e-9) {
					continue;
				}
				// Pagamento: firma doméstica (comprador) -> firma vendedora +
				// frete ao serviço do comprador. Governo cobre o que a firma não
				// puder (insumo essencial), conservando o dinheiro.
				const double unit_cost = offer.unit_cost;
				const double payable = std::max(0.0, buyer_city.firm_liquidity())
					+ std::max(0.0, buyer_city.government_budget);
				if (amount * unit_cost > payable) {
					amount = unit_cost > 0.0 ? payable / unit_cost : 0.0;
				}
				if (amount <= 1e-9) {
					continue;
				}
				// F4: frota livre + combustível da nação vendedora. Sem veículo
				// ou sem fuel a carga não sai (a fila do dia alimenta a reposição
				// do governo); com capacidade parcial, embarca proporcional.
				const bool fleet_same_landmass =
					same_landmass_city(seller, buyer_city);
				TransportDemand transport = route_demand(seller, buyer_city,
					fleet_same_landmass ? -1 : city_port[static_cast<std::size_t>(
						offer.seller_index)],
					fleet_same_landmass ? -1 : city_port[static_cast<std::size_t>(
						buyer.city_index)], amount);
				bool fuel_limited = false;
				const double fleet_scale = fleet_capacity_scale(
					seller.nation, transport, fuel_limited);
				if (fleet_scale < 1.0) {
					const double queued_tons = amount * (1.0 - fleet_scale);
					const auto fleet_found = ws_.fleets.find(seller.nation);
					if (fleet_found != ws_.fleets.end()) {
						fleet_found->second.queue_today += queued_tons;
						if (fuel_limited) {
							fleet_found->second.missed_no_fuel += queued_tons;
						} else {
							fleet_found->second.missed_no_fleet += queued_tons;
						}
					}
					if (fleet_scale <= 1e-9) {
						continue;
					}
					amount *= fleet_scale;
					transport = route_demand(seller, buyer_city,
						fleet_same_landmass ? -1 : city_port[static_cast<std::size_t>(
							offer.seller_index)],
						fleet_same_landmass ? -1 : city_port[static_cast<std::size_t>(
							buyer.city_index)], amount);
				}
				const double total_cost = amount * unit_cost;
				const double firm_share = std::min(total_cost,
					std::max(0.0, buyer_city.firm_liquidity()));
				const double gov_share = std::min(total_cost - firm_share,
					std::max(0.0, buyer_city.government_budget));
				const double paid = firm_share + gov_share;
				if (paid <= 1e-9) {
					continue;
				}
				const double goods_value = amount * offer.origin_anchor;
				const double freight = amount * offer.unit_freight;
				const double tariff_paid = amount * offer.unit_tariff;
				buyer_city.firm_pay(firm_share);
				buyer_city.government_budget -= gov_share;
				seller.firm_receive(goods_value);
				buyer_city.service_receive(freight);
				buyer_city.freight_revenue += freight;
				buyer_city.government_budget += tariff_paid;
				buyer_city.tariff_revenue += tariff_paid;

				seller.markets[product].stock -= amount;
				seller.markets[product].desired += amount;
				seller.markets[product].demand += amount;
				seller.markets[product].sold += amount;
				seller.markets[product].effective += amount;
				seller.revenue_by_product[product] += goods_value;
				seller.exported[product] += amount;
				seller.export_value[product] += goods_value;
				buyer_city.imported[product] += amount;
				buyer_city.import_value[product] += goods_value + freight;

				TradeOrder order;
				order.from_city = seller.id;
				order.to_city = buyer_city.id;
				order.from_nation = seller.nation;
				order.to_nation = buyer_city.nation;
				const double lane_km = distance_km(seller.center, buyer_city.center);
				order.scope = (seller.nation == buyer_city.nation
					|| lane_km <= kRegionalRadiusKm)
					? MarketScope::Regional : MarketScope::International;
				order.product = product;
				order.amount = amount;
				order.origin_price = offer.origin_anchor
					/ std::max(1e-9, fx_rate(seller.nation));
				order.origin_price_anchor = offer.origin_anchor;
				order.tariff = tariff_paid;
				order.delivered_price = offer.delivered;
				order.fx_rate = fx_rate(buyer_city.nation);
				order.seller_fx_rate = fx_rate(seller.nation);
				order.buyer_currency = currency_code(buyer_city.nation);
				order.seller_currency = currency_code(seller.nation);
				// F5: a cotação que forma o preço interno é o CIF do bem na
				// âncora (origem efetiva + tarifa), sem o frete-serviço — mesma
				// regra do comércio de comida.
				buyer_city.import_price[product] = offer.origin_anchor
					+ offer.unit_tariff;
				double total_transit = 0.0;
				double total_freight_cost = 0.0;
				double port_delay_days = 0.0;
				if (same_landmass_city(seller, buyer_city)) {
					Leg leg = make_leg(TransportMode::Truck, seller.id, buyer_city.id,
						seller.center, buyer_city.center, amount);
					total_transit = leg.transit_hours;
					total_freight_cost = leg.cost;
					order.legs.push_back(std::move(leg));
				} else {
					const int seller_port = city_port[static_cast<std::size_t>(
						offer.seller_index)];
					const int buyer_port = city_port[static_cast<std::size_t>(buyer.city_index)];
					if (seller_port >= 0 && buyer_port >= 0) {
						Port& sp = ws_.ports[static_cast<std::size_t>(seller_port)];
						Port& bp = ws_.ports[static_cast<std::size_t>(buyer_port)];
						if (distance_km(seller.center, sp.center) > 1.0) {
							Leg leg = make_leg(TransportMode::Truck, seller.id, sp.id,
								seller.center, sp.center, amount);
							total_transit += leg.transit_hours;
							total_freight_cost += leg.cost;
							order.legs.push_back(std::move(leg));
						}
						{
							Leg leg = make_leg(TransportMode::Ship, sp.id, bp.id,
								sp.center, bp.center, amount);
							total_transit += leg.transit_hours;
							total_freight_cost += leg.cost;
							order.legs.push_back(std::move(leg));
						}
						if (distance_km(bp.center, buyer_city.center) > 1.0) {
							Leg leg = make_leg(TransportMode::Truck, bp.id, buyer_city.id,
								bp.center, buyer_city.center, amount);
							total_transit += leg.transit_hours;
							total_freight_cost += leg.cost;
							order.legs.push_back(std::move(leg));
						}
						total_transit += 2.0 * kPortHandlingHours;
						double demurrage = 0.0;
						port_delay_days += port_charge(sp, amount, demurrage);
						port_delay_days += port_charge(bp, amount, demurrage);
						const double demurrage_paid = std::min(demurrage,
							std::max(0.0, buyer_city.government_budget));
						buyer_city.government_budget -= demurrage_paid;
						buyer_city.service_receive(demurrage_paid);
						buyer_city.freight_revenue += demurrage_paid;
						total_freight_cost += demurrage_paid;
						total_transit += port_delay_days * kHoursPerDay;
					}
				}
				order.transit_hours = total_transit;
				order.freight_cost = total_freight_cost;
				ws_.last_orders.push_back(std::move(order));
				const int arrival_day =
					ws_.day + 1 + static_cast<int>(std::ceil(port_delay_days));
				// F4: reserva os veículos até a chegada e debita o combustível.
				// `transport` já é o embarque do lote final (recalculado acima
				// quando a frota limitou a carga; senão, o cálculo inicial) — a
				// 3ª chamada era idêntica e só repetia o haversine. Bit-idêntico.
				fleet_reserve(seller.nation, transport);
				Shipment shipment;
				shipment.to_city = buyer_city.id;
				shipment.product = product;
				shipment.amount = amount;
				shipment.arrival_day = arrival_day;
				shipment.fleet_nation = seller.nation;
				// Caminhões de coleta/entrega voltam no mesmo dia (ver
				// trade_food); o navio fica reservado até a chegada.
				shipment.fleet_trucks = fleet_same_landmass ? transport.trucks : 0.0;
				shipment.fleet_ships = transport.ships;
				shipment.fleet_planes = transport.planes;
				shipment.diesel = transport.diesel;
				shipment.jet_fuel = transport.jet_fuel;
				ws_.in_transit.push_back(std::move(shipment));
				in_transit[buyer_offset] += amount;
				remaining -= amount;
				++shipments;
			}
			}
		}
		}
	}
}
void SimCore::phase_trade() {
	trade_food();
	trade_goods();

	OGS_SIM_PHASE_LAP(kPhaseTrade);
}
} // namespace ogs
