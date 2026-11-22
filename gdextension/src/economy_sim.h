#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>

#include <atomic>
#include <cstddef>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "sim/ai/ai_phase.h"
#include "sim/bank.h"
#include "sim/commands.h"
#include "sim/sim_types.h"
#include "sim/sim_core.h"
#include "sim/world_state.h"

namespace godot {

// S5a: os tipos POD (City, Building, ...) agora vivem em `ogs`
// (sim/sim_types.h); este using os traz para o escopo da fachada sem renomear
// nenhuma referência existente.
using namespace ogs;

/**
 * Núcleo econômico do jogo, exposto ao Godot via GDExtension.
 *
 * O mundo é formado por cidades: cada nação possui uma ou mais cidades, e cada
 * cidade tem população, produção e demanda próprias. A necessidade de trigo é
 * resolvida entre cidades por um mercado em três camadas — regional (cidades
 * vizinhas do mesmo país), nacional (resto do país) e internacional — e a carga
 * só chega ao destino após o transporte físico.
 *
 * Tipos de transporte, com velocidade e capacidade distintas:
 *   caminhão - velocidade mediana, carga pequena
 *   navio    - velocidade baixa e constante, carga enorme
 *   avião    - velocidade alta, carga média (emergência de estoque)
 */
class EconomySim : public RefCounted {
	GDCLASS(EconomySim, RefCounted)

public:
	EconomySim();
	~EconomySim() override;

	/** Inicializa o cenário de validação de duas cidades com população agregada. */
	void initialize_scenario();

	/**
	 * Inicializa o modelo demo a partir dos dados reais do cenário (cidades,
	 * portos e construções), em vez do cenário de validação AGRIA/PORTUS. Este é
	 * o passo do contrato único: o ciclo completo roda sobre o mundo real.
	 */
	void initialize_world(const Array& cities, const Array& ports, const Array& buildings);

	/** Avança um dia do cenário de validação. */
	Dictionary tick_day();

	/**
	 * Executa o dia do cenário de validação sem montar o resumo (nenhum Variant
	 * é criado). É a metade "cara" do tick: o jogo roda em WorkerThreadPool e
	 * chama get_status() na main thread quando o trabalho termina.
	 */
	void tick_core();

	/**
	 * Roda o dia (tick + IA). A4: NÃO monta/publica o Dictionary do mundo; no
	 * fecho semanal captura o retrato de trabalho das nações (S17) e pronto.
	 * O jogo chama `get_day_summary()` na main thread depois do join; o status
	 * completo é consultado sob demanda (tools/testes/goldens).
	 *
	 * É o que a thread do WorkerThreadPool chama (ver tick_scheduler.gd): nenhum
	 * Variant cruza a fronteira da thread (nem o retorno).
	 */
	void tick_and_publish();

	/** Liga/desliga a medição por fase do tick (custo desprezível). */
	void set_profiling(bool enabled);

	/**
	 * S16: define o número de threads das fases por cidade do tick (<= 0 =
	 * automático = min(núcleos - 2, 8), o DEFAULT; > 0 = esse número; 1 =
	 * serial). As fases globais seguem seriais. Determinístico: o resultado
	 * não muda.
	 */
	void set_sim_threads(int threads);

	/**
	 * Marca o status publicado como desatualizado. Qualquer método que muda o
	 * estado fora do tick (investir, construir, choque de preço, aptidão) tem de
	 * chamar isto: senão get_status() devolve o snapshot do início do dia e
	 * a UI mostra caixa/capacidade antigos até o próximo tick.
	 */
	void status_invalidate();

	// ------------------------------------------------------------------
	// A3 - fila de comandos (docs/economy/refactor/A3_commands.md).
	//
	// Toda mutação do jogador/IA vira um `ogs::Command` e entra na fila. A fila
	// é aplicada no INÍCIO do dia (`tick_core`, antes do prólogo) na ordem de
	// emissão. Os métodos que devolvem um resultado (invest, build, ...)
	// aplicam na hora quando o dia NÃO está em andamento - senão o chamador
	// leria um retorno vazio e os testes/IA quebram; esse é o único caminho
	// síncrono e está documentado. Com um dia em andamento (thread do tick), a
	// ação só entra na fila: nada toca o `ws_` fora do escritor único.
	// ------------------------------------------------------------------

	/** Aplica a fila pendente agora (início do dia ou barreira de leitura). */
	void apply_commands();

	/** Nº de comandos ainda não aplicados. */
	int pending_command_count() const;

	/**
	 * Log serializável (Array de Dictionary, pronto para JSON.stringify):
	 * {type, day, apply_day, seq, s: [...], v: [...], i: [...], b} — as listas
	 * `s`/`v`/`i` só aparecem quando têm algum slot não vazio. `day` é o dia da
	 * emissão e `apply_day` o dia em que o comando foi aplicado (diferem em 1
	 * quando o comando esperou na fila). Só o que já foi aplicado.
	 */
	Array get_command_log() const;
	void clear_command_log();
	/** true quando o log parou de crescer (teto de 200 mil comandos). */
	bool command_log_truncated() const;

	/**
	 * Replay: aplica um log (no formato de get_command_log()) sobre o mundo
	 * carregado, replicando o relógio - antes de cada comando avança até o dia
	 * em que ele foi APLICADO (`apply_day`); no fim avança até `target_day`.
	 * @returns true quando o log inteiro foi aplicado.
	 */
	bool replay_commands(const Array& entries, int target_day);

	/** Microssegundos por fase do último tick (vazio quando desligado). */
	Dictionary get_profile() const;
	// Pré-aquece os DOIS slots do status (só tools/testes: o jogo não monta o
	// status no tick desde a A4). O primeiro dia publicado em um slot frio paga
	// a criação de TODOS os dicionários por cidade (medido no cenário real:
	// 60 ms contra 17 ms num dia quente). Aqui isso vira custo de carga.
	void warm_status();

	/**
	 * Estado do mundo inteiro (cidades, nações, portos, pedidos).
	 *
	 * A4: consulta SOB DEMANDA — tools/testes/goldens. O tick não a monta mais.
	 * O dicionário é REAPROVEITADO entre chamadas (ver StatusCache): cada
	 * chamada reescreve os valores nos mesmos contêineres. Contrato: o
	 * resultado é uma fotografia viva, válida até a próxima chamada; quem
	 * precisa reter o valor entre dias deve copiar (`duplicate(true)`).
	 */
	Dictionary get_status();

	/**
	 * Detalhe completo de UMA cidade sob demanda (mesmo formato do bloco por
	 * cidade publicado pelo `get_status()`): id, nome, nação, escalares,
	 * `markets`, `pop`, `services`, `buildings`, `deposits`, fluxos de comércio
	 * e emprego setorial. Devolve `{}` quando a cidade não existe.
	 *
	 * Converte direto do `WorldState` num slot próprio (não reusa o cache do
	 * status diário): é a porta de acesso do detalhe pesado quando o status
	 * diário fica leve (S7-c). Barato por contrato — chamado sob demanda por
	 * cidade, nunca no laço de todas as cidades.
	 */
	Dictionary get_city_status(const String& city_id);

	// ------------------------------------------------------------------
	// A4 - consultas sob demanda (docs/economy/refactor/A4_status_query.md).
	//
	// O tick não monta mais o Dictionary do mundo inteiro: ele publica um
	// resumo leve (`get_day_summary`) e o resto chega por consulta, lida
	// direto do `WorldState` no fecho do dia. `get_status()` continua
	// existindo, mas só para tools/testes/goldens. Nenhuma consulta muda a
	// simulação nem o save.
	// ------------------------------------------------------------------

	/**
	 * Resumo leve do dia (sem cidades/nações/mercados):
	 * day, calendar_day, money_total, caixas, contadores, violações de
	 * invariante, catálogo de produtos e os TOTAIS do mundo (soma das
	 * cidades, na ordem das cidades). É o que o jogo publica todo dia.
	 */
	Dictionary get_day_summary();

	/**
	 * Colunas por cidade (uma `PackedFloat64Array`/`PackedStringArray` por
	 * campo, indexada pela posição em `ws_.cities`): id, nome, nação, moeda,
	 * centro e os MESMOS doubles do bloco por cidade do `get_status()`.
	 * Cacheada por dia; é a fonte dos modos de mapa, HUD e histórico.
	 */
	Dictionary get_city_columns();

	/** Colunas por nação (179 posições), mesmos valores do `nations[]`. */
	Dictionary get_nation_columns();

	/**
	 * Linhas leves de TODAS as cidades (Array de Dictionary, mesmo formato e
	 * mesmos valores do bloco `cities[]` do status). Sob demanda: as telas de
	 * dados usam quando estão abertas. Nunca no tick.
	 */
	Array get_city_rows();

	/** Linhas agregadas de TODAS as nações (mesmo formato de `nations[]`). */
	Array get_nation_rows();

	/** Linha leve de UMA cidade (mesmo formato do `cities[]` do status). */
	Dictionary get_city_info(const String& city_id);

	/** Ficha de UMA nação (mesmo formato do bloco `nations[]` do status). */
	Dictionary get_nation_status(const String& nation_tag);

	/** Pedidos de comércio do último dia (mesmo formato de `trade_orders`). */
	Array get_trade_orders();

	/** Portos do mundo (mesmo formato de `ports` do status). */
	Array get_ports();

	/** Moedas carregadas, no MESMO formato de `currencies` do status. */
	Array get_currency_rows();

	/** Catálogo de produtos (mesmo Array de `products` do status). */
	Array get_products();

	/** Bloco `light` do status (tabelas planas usadas pelas telas). */
	Dictionary get_light_columns();

	/**
	 * Ranking top-N de cidades por `field` (nome de coluna, ex.: "population",
	 * "production", "gdp"). `count <= 0` devolve todas. Ordenado decrescente,
	 * desempate por id; cada linha: {id, name, nation, value}.
	 */
	Array get_city_ranking(const String& field, int count);

	/** Ranking top-N de nações por `field` (ex.: "population", "need"). */
	Array get_nation_ranking(const String& field, int count);

	/** Versão dos dados publicados (A4): sobe a cada dia e a cada mutação que
	 * invalida o status. A UI usa para saber se o retrato em cache está velho. */
	int get_data_version() const;

	/** Choque de preço em um produto (id de goods.json, ex.: "wheat"). */
	void price_shock(const String& product, double multiplier);

	/**
	 * Define a aptidão territorial de um produto para uma cidade.
	 * @param city_id Identificador da cidade.
	 * @param product id de goods.json (ex.: "wheat", "iron").
	 * @param value Aptidão entre 0 e 1 (0 impede a produção primária).
	 */
	void set_suitability(const String& city_id, const String& product, double value);

	/**
	 * Teto físico de capacidade agrícola (t/dia) de uma cidade para um
	 * produto, derivado da geografia (área cultivável × produtividade FAO).
	 *
	 * Igual à aptidão, é DADO DE GEOGRAFIA e não entra no save: é declarado
	 * pelo `GeographyService` (e viaja no bloco `definitions` do snapshot). Só
	 * alimenta a decisão da IA — não muda preço, produção nem o status, por
	 * isso não invalida o cache publicado. Produto desconhecido, cidade
	 * ausente ou valor negativo não são erro: a chamada é ignorada/limitada.
	 * @param city_id Identificador da cidade.
	 * @param product id de goods.json (ex.: "wheat").
	 * @param value Capacidade em t/dia (limitada a >= 0).
	 */
	void set_land_capacity(const String& city_id, const String& product,
		double value);

	/**
	 * G (D-096) — declara o PLANO DE SAFRA de (cidade, produto): área colhida
	 * (ha), rendimento (t/ha) e 1..4 janelas em DIA DO ANO (1..365/366, dia 0
	 * = 2027-01-01) com a fração do orçamento anual de cada safra.
	 *
	 * `seasons` é um Array de Dictionary `{plant_start, plant_end,
	 * harvest_start, harvest_end, share}`. Sem janela de colheita válida o
	 * plano é recusado (a produção diária da fazenda continua) — nada de safra
	 * vazia. Sobrescrever o plano preserva o ESTADO da safra já registrado (o
	 * service redeclara o plano no load do jogo).
	 * @param city_id Identificador da cidade.
	 * @param product id de goods.json (os 5 primários de comida).
	 * @param area_ha Área colhida (dado FAOSTAT, rateada entre as cidades).
	 * @param yield_t_ha Rendimento do dado (FAOSTAT).
	 * @param seasons Janelas de plantio/colheita + share.
	 */
	void set_crop_plan(const String& city_id, const String& product,
		double area_ha, double yield_t_ha, const Array& seasons);

	/**
	 * G — estoque inicial (t) de (cidade, produto) no dia 1: ASSIGNA (não
	 * soma), para a chamada ser idempotente. Vem do `ending_stocks_t`
	 * observado (USDA PSD) ou da regra de 60 dias de consumo do próprio dado.
	 */
	void set_crop_initial_stock(const String& city_id, const String& product,
		double tons);

	/**
	 * G — perda pós-colheita do produto (% do que entra) e a fatia de
	 * armazenagem declarada (% do total; < 0 = sem dado, o núcleo usa a fração
	 * declarada da perda total).
	 */
	void set_crop_loss(const String& product, double loss_pct,
		double storage_share_pct);

	/**
	 * G — prazo de prateleira (dias) do bem: o estoque parado perde metade em
	 * `days` dias (regra declarada no núcleo). `days <= 0` = sem dado (não
	 * decai), mas o bem entra na lista da cadeia.
	 */
	void set_shelf_life(const String& product, int days);

	/**
	 * G — fertilizante nitrogenado (kg N/ha) da cultura na cidade. Só tem
	 * efeito depois do `set_crop_plan` daquele (cidade, produto).
	 */
	void set_fertilizer_n(const String& city_id, const String& product,
		double n_kg_ha);

	/**
	 * G — qualidade do armazém 0..1 (hook de infraestrutura; a Parte B5/E pode
	 * declarar). Sem chamada, a qualidade é DERIVADA da infra do estado
	 * (estrada/energia/água).
	 */
	void set_storage_quality(const String& city_id, double quality);

	/**
	 * G — ficha da agricultura sazonal, montada SOB DEMANDA (testes e UI):
	 * por nação, o estoque por cultura, os dias de consumo, a próxima colheita,
	 * a produção do ano e a perda do mês. `{}` quando não há plano declarado.
	 */
	Dictionary get_agriculture_status() const;

	/**
	 * G2 — declara a capacidade (t) de um armazém agregado (cidade, tipo,
	 * dono). Idempotente: ASSIGNA, como os setters de geografia/agricultura.
	 * @param type "silo", "cold", "fuel_tank", "gas", "bulk_yard", "general",
	 *   "vault" ou "magazine".
	 * @param owner "government"/"state" ou "private".
	 */
	void set_storage_capacity(const String& city_id, const String& type,
		const String& owner, double tons);
	/** G2 — override do tipo de armazém de um produto (JSON da G2). */
	void set_storage_product_type(const String& product, const String& type);
	/**
	 * D-101 — ASSIGNA o estoque guardado (t) de um produto no armazem
	 * (cidade, tipo, dono): semente do dia 1 da reserva estrategica do JSON.
	 * So para tipo operacional que aceita o produto do mapeamento; o valor e
	 * limitado a capacidade do site. Nao cria nem destroi toneladas.
	 */
	void set_storage_stock(const String& city_id, const String& type,
		const String& owner, const String& product, double tons);
	/** G2 — obra de armazém pela fila §15 (paga pelo dono). */
	Dictionary storage_build(const String& city_id, const String& type,
		const String& owner, double tons);
	/** G2 — compra (`buy`) ou liberação (`relief` = socorro sem pagamento). */
	Dictionary storage_trade(const String& city_id, const String& type,
		const String& owner, const String& product, double tons, bool buy,
		bool relief);
	/**
	 * G2 — camada NACIONAL de leitura (sem mercado nacional): por nação, a
	 * reserva do governo e o estoque privado por produto, a capacidade, os
	 * dias de consumo cobertos e o IPC/preço médio ponderados pela cesta dos
	 * quintis. Montado sob demanda (testes/UI).
	 */
	Dictionary get_storage_status() const;
	// ------------------------------------------------------------------
	// PARTE E (D-116) — ENERGIA ELÉTRICA (save v35). O DADO (acesso, perdas,
	// parque, potencial e PVOUT mensal) vem de data/definitions/energy/; a
	// REGRA vem de power_params.json. Nada aqui inventa número: parque/potencial
	// ausente = cidade sem dado (não se simula blecaute).
	// ------------------------------------------------------------------
	/**
	 * Declara a regra e o dado de energia. `nations` = {tag: {access_pct,
	 * access_urban_pct, access_rural_pct, loss_pct}}; `cities` = {city_id:
	 * {access_pct, loss_pct, park: {fonte: MW}, cap: {fonte: MW},
	 * solar_month: [12]}}. `params` vazio = regra declarada padrão.
	 * `only_missing` (default true) não sobrescreve o estado de um save.
	 * Devolve o número de cidades com definição aplicada.
	 */
	int set_power_definitions(const Dictionary& nations,
		const Dictionary& cities, const Dictionary& params,
		bool only_missing);
	/** Sobrescreve parâmetros de regra pelo nome do campo (laboratório). */
	int set_power_params(const Dictionary& values);
	/** Teste/depuração: força o estado de conexão de uma cidade. */
	bool set_power_city(const String& city_id, int connection, double line_mw,
		double access_pct);
	/**
	 * Obra de usina pela fila §15 (paga pelo dono: grupo, ou o Estado quando
	 * `group_id` vazio). `fuel` = "hydro", "solar", "wind", "coal", "gas",
	 * "oil", "nuclear", "biomass" ou "other".
	 */
	Dictionary build_power_plant(const String& city_id, const String& fuel,
		double mw, const String& group_id);
	/** Obra de LINHA: leva a cidade (isolada/parcial) a conectada. */
	Dictionary build_power_line(const String& city_id, double mw,
		const String& group_id);
	/**
	 * E5 (D-117): declara as interconexões do ano-base. `links` = array de
	 * `{a, b, importer, capacity_mw}` (par canônico; `capacity_mw` na escala do
	 * dado). Só entram pares com rede NACIONAL nos dois lados; `only_missing`
	 * (default true) preserva o que veio de um save v36.
	 * Devolve o número de linhas semeadas.
	 */
	int set_power_interconnects(const Array& links, bool only_missing);
	/**
	 * E5: obra de interconexão entre duas redes NACIONAIS (mesmo padrão da
	 * linha de cidade): paga `capex_per_mw_line` × MW do caixa do dono,
	 * entra na fila §15 e soma capacidade ao par ao concluir.
	 * `group_id` vazio = governo da maior cidade conectada de `nation_a`.
	 */
	Dictionary build_interconnect(const String& nation_a,
		const String& nation_b, double mw, const String& group_id);
	/**
	 * Leitura da rede (testes/UI): por nação (demanda, servido, % servida,
	 * preço, racionamento, geração por fonte, perdas e membros por estado) e
	 * por cidade (estado de conexão, linha, acesso, perda efetiva, demanda,
	 * servido, fator da indústria/serviços, insatisfação e conta da indústria).
	 */
	Dictionary get_power_status() const;
	/**
	 * C1 (D-102) — BALANÇO POR AGENTE (leitura). Devolve, sob demanda:
	 *   * `world` — balanço agregado de famílias, firmas, governo e exterior;
	 *   * `by_nation` — o mesmo por nação (país das cidades);
	 *   * `invariant` — soma mundial das posições financeiras (0), resíduo de
	 *     caixa contra o `money_total` e a base monetária (passivo do emissor);
	 *   * `firms.solvency` — contagem de plantas líquidas/ilíquidas/insolventes.
	 * Nada aqui move dinheiro; o status diário publica o bloco `balance` com o
	 * resumo do mundo.
	 */
	Dictionary get_balance_status() const;
	/**
	 * C2a (D-103) — BANCO CENTRAL POR MOEDA (leitura, sob demanda / A4).
	 * Devolve:
	 *   * `monetary_policy` — padrão monetário do mundo e moeda de reserva;
	 *   * `invariant` — a moeda do dia atribuída às moedas (M0) + os buckets
	 *     sem nação = `money_total`; `ok` é a checagem cruzada;
	 *   * `currencies` — por moeda: regime, independência, M0/M1/M2, ouro do
	 *     cofre (t e valor), reservas em moeda de reserva (0), títulos (0) e o
	 *     patrimônio financeiro do BC;
	 *   * `cpi` — por nação: índice da cesta (o mesmo IPC do G2) e a inflação
	 *     de 12 meses lida da série mensal (ausência declarada com `has_12m`
	 *     falso quando ainda não há 12 meses);
	 *   * `unallocated` — caixas sem nação atribuível, declarados.
	 * Nada aqui emite, fixa juros ou intervém (C2b); o ouro é ativo REAL e não
	 * entra na identidade financeira.
	 */
	Dictionary get_central_bank_status() const;
	/**
	 * C2a (D-103) — declara a RESERVA DE OURO do banco central por nação (t de
	 * ouro fino), agregada por moeda. A fonte é o dado real da G2/D-101
	 * (`strategic_reserves.json`, World Gold Council); o núcleo não lê arquivo.
	 * Idempotente: reaplica o mesmo dicionário (load). Nação sem moeda na
	 * tabela cai em `gold_unallocated_tonnes` (ausência declarada).
	 * @param tonnes_por_nacao `{ISO3: toneladas}`.
	 * @returns toneladas declaradas (0 = nada aplicado).
	 */
	double set_central_bank_gold(const Dictionary& tonnes_by_nation);
	/**
	 * C2b1 (D-104) — escolhe a POLÍTICA de uma moeda. `independence` é
	 * `no_data` (ausência declarada; resolve no default do jogo),
	 * `independent` (regra de Taylor própria) ou `controlled` (o governo fixa o
	 * juro e a emissão monetiza o déficit medido do dia). Idempotente; aplica no
	 * dia seguinte, no fecho da política monetária.
	 * @returns true se a moeda existe e o nome é válido.
	 */
	bool set_central_bank_policy(const String& currency,
		const String& independence);
	/**
	 * C2b2 (D-106) — escolhe o REGIME CAMBIAL de uma moeda, pela fila A3
	 * (aplica no início do dia seguinte): `floating`, `peg` ou `band`.
	 * `parity` = unidades locais por 1 unidade da moeda de reserva (0 = o
	 * `base_rate` do dado do F5, nunca um número inventado); `band_width` =
	 * ±fração da banda (negativo = default de jogo).
	 * @returns true se a moeda existe e o nome é válido (o comando entrou na fila).
	 */
	bool set_exchange_regime(const String& currency, const String& regime,
		double parity, double band_width, const String& nation = String());
	/**
	 * C2c (D-108) — carrega os REGIMES REAIS do IMF AREAER 2026
	 * (`data/definitions/economy/fx_regimes_2026.json`): regime por moeda,
	 * âncora (moeda ou cesta), fonte da âncora, emissor e a trava de moeda de
	 * união. Chamado ANTES de `set_currencies`, como as demais definições.
	 * @returns número de moedas com regime declarado.
	 */
	int set_fx_regimes(const Dictionary& data);
	/**
	 * D-108(e) — sucessão de moeda extinta (BGN -> EUR a 1,95583; ver a tabela
	 * `kSuccessions` em economy_sim.cpp). Converte a paridade local, funde o
	 * estado da moeda no destino e preserva a conservação (dinheiro é âncora:
	 * não muda). Idempotente: sem a moeda no estado, devolve 0.
	 * @returns quantidade convertida (unidades locais) ou 0.
	 */
	double migrate_currency(const String& from, const String& to,
		double units_per_target);
	/**
	 * C2b2 (D-106) — escolhe o PADRÃO MONETÁRIO do mundo, pela fila A3:
	 * `fiat` (default), `bretton_woods` ou `gold`. Nunca é automático.
	 * @returns true se o nome é válido.
	 */
	bool set_monetary_standard(const String& standard);
	/**
	 * C2b2 (D-106) — declara as RESERVAS em moeda de reserva (unidades de
	 * âncora) por moeda. É declaração explícita (lab/teste/cenário): o
	 * repositório não tem fonte de reservas cambiais por país e o default é 0.
	 * Marca a moeda como `funded` (a defesa medida passa a existir).
	 * @returns total declarado (âncora).
	 */
	double set_central_bank_reserves(const Dictionary& anchor_by_currency);
	/**
	 * C2b2 (D-106) — reservas iniciais do CENÁRIO de estresse: N dias de
	 * importação MEDIDA do próprio mundo (regra declarada, não dado de país).
	 * O default do jogo não chama isto.
	 * @returns total em âncora.
	 */
	double seed_central_bank_reserves(int days);
	/** C2b2 (D-106) — parâmetros de regra (banda, fatia de defesa, cobertura). */
	void set_exchange_policy(double band_width, double defence_share,
		double gold_reserve_ratio);
	/**
	 * C2b2 (D-106) — compra/venda de OURO FÍSICO do cofre contra o mercado do
	 * produto ouro (sem teto econômico de preço, D-091). `buy` = o cofre compra
	 * do mercado; false = vende ao mercado. Ouro não é criado nem destruído.
	 * @returns `{ok, tons, value, city, error}`.
	 */
	Dictionary central_bank_gold_trade(const String& currency, double tonnes,
		bool buy);
	/**
	 * C2b2 (D-106) — conversão do padrão Bretton Woods/ouro: o cofre compra
	 * ouro do mercado ao PREÇO FIXO do padrão e paga com as reservas.
	 */
	Dictionary central_bank_gold_convert(const String& currency, double tonnes);
	// ------------------------------------------------------------------
	// C3 (D-107) — CRÉDITO DE EMPRESA (banco agregado como credor).
	// Desenho e contrato: docs/economy/C3_group_balance.md.
	// ------------------------------------------------------------------
	/** Liga/desliga a mecânica de crédito (default do jogo = desligado). */
	Dictionary set_bank_credit(bool enabled);
	/**
	 * Regras do crédito (0 = default de `sim/bank.h` / JSON declarado):
	 * spread anual sobre a taxa básica, teto de alavancagem, cobertura de
	 * juros, dias de insolvência até o calote e fração do excedente
	 * amortizada por dia.
	 */
	void set_bank_policy(double spread, double leverage_cap,
		double interest_cover, int default_days, double repay_share);
	/**
	 * Capital pago pelo DONO do banco (C4/D-110). `role` = "public" (Tesouro,
	 * default), "private" (poupança das famílias) ou "foreign" (posição do
	 * exterior). Não cria dinheiro: sai do dono e vira capital do banco.
	 * @returns `{ok, paid, nation, role, error}`.
	 */
	Dictionary set_bank_capital(const String& nation, double amount,
		const String& role = String("public"));
	/**
	 * C4 (D-110) — definições de `banks.json`: nomes (fictícios/genéricos),
	 * presença do banco estrangeiro e país de origem. Cria/renomeia os bancos
	 * e renormaliza as participações; quando o dado não declara banco
	 * estrangeiro, o papel é fundido no privado conservando valores.
	 */
	void set_bank_definitions(const Dictionary& definitions);
	/**
	 * Pede crédito para um grupo pela fila A3. Limitado pelo crédito
	 * disponível (alavancagem e cobertura de juros).
	 * @returns `{ok, granted, group, error}`.
	 */
	Dictionary group_borrow(const String& group_id, double amount);
	/**
	 * C3 (D-107) — status A4 do crédito: política, totais, uma linha por
	 * banco/nação, os grupos endividados e o invariante dos dois lados.
	 */
	Dictionary get_bank_status() const;
	/**
	 * C6 (D-114) — regras da crise bancária (laboratório/teste):
	 * `enabled`, `capital_min`, `capital_cut_floor`, `liquidity_min`
	 * (RESERVADO: nenhuma regra o consome), `loss_scale`, `loss_ema_days`,
	 * `smoothing`,
	 * `run_threshold`, `run_share`, `flight_to_quality`, `lolr_spread`,
	 * `lolr_haircut`, `lolr_max_assets`, `insured_share`, `premium_daily`,
	 * `fund_target_ratio`, `capital_flight_share`, `treasury_debt_rescue`,
	 * `takeover_public` e os pesos `w_solvency`/`w_liquidity`/`w_losses`/
	 * `w_contagion`/`w_market`. `enabled` é estado (vai ao save); o resto é
	 * regra (0 = default declarado em `sim/bank_crisis.h`).
	 * @returns os valores efetivos.
	 */
	Dictionary set_bank_crisis(const Dictionary& config);
	/**
	 * C6 (D-114) — choque de perda de crédito no banco (teste/estresse):
	 * baixa `loss` da carteira e da dívida dos tomadores na mesma proporção,
	 * sem mover dinheiro. `loss_share` é a fração da carteira (0,30 = 30 %).
	 * @returns `{ok, bank, written, share}`.
	 */
	Dictionary bank_shock(const String& bank_id, double loss,
		double loss_share = 0.0);
	/**
	 * C5 (D-112) — alavancas de configuração da bolsa (laboratório): `enabled`,
	 * `investment_channel`, `risk_premium`, `payout_ratio`, `step`,
	 * `expectation_k`. `enabled` é estado (vai ao save); os parâmetros são regra
	 * (0 = default declarado em `sim/stock.h`). Não é feature de jogador.
	 * @returns `{enabled, investment_channel}`.
	 */
	Dictionary set_stock_exchange(const Dictionary& config);
	/**
	 * C5 (D-112) — status A4 da bolsa: política, totais, uma linha por praça
	 * (índice, confiança e sinais medidos) e um papel por grupo listado
	 * (preço, fundamental, q de Tobin, dividendos e carteiras).
	 */
	Dictionary get_stock_status() const;
	/** G2 — mapeamento produto→tipo de armazém em vigor (tabela declarada +
	 *  overrides do JSON). `{produto: tipo}`; usado pelo StorageService. */
	Dictionary get_storage_product_types() const;

	/**
	 * Declara a reserva mineral do território de uma cidade para um produto
	 * (D-086: referência de profundidade, não um limite físico — o recurso nunca
	 * acaba). As minas da cidade debitam `remaining_kt` como contabilidade, mas
	 * a reserva zerada NÃO para a produção nem bloqueia obra.
	 * Idempotente: se o produto já tem reserva, mantém o saldo restante (um
	 * save carregado não é "reabastecido") e só atualiza qualidade/profundidade.
	 * @param product id de goods.json (ex.: "iron", "gold").
	 * @param reserve_kt Reserva inicial em quilotoneladas (1 kt = 1000 t).
	 * @param quality Qualidade 0..1 (eficiência = 0,5 + 0,5 × qualidade).
	 * @param depth_m Profundidade média em metros (encarece o opex).
	 * @returns true se a reserva foi criada agora.
	 */
	bool set_deposit(const String& city_id, const String& product,
		double reserve_kt, double quality, double depth_m);

	/**
	 * Define o índice de renda externa de uma cidade (multiplicador sobre a
	 * renda-base de serviços/exportações não modelados).
	 *
	 * A nação recebe renda do resto do mundo proporcional ao seu PIB PPP per
	 * capita REAL: o `GeographyService` calcula, a partir de `nations.csv`
	 * (gdp_ppp_bn/population), um índice normalizado para que a média mundial
	 * ponderada por população fique em 1,0 — ou seja, a renda média continua
	 * `kExternalIncomePerCapita`. Nações ricas sem terra (SAU/QAT/ISL)
	 * importam mais; nações pobres recebem o piso (10% da média) e continuam
	 * frágeis. O valor é pago pelo setor externo (`foreign_cash`), então
	 * `money_total` continua conservado.
	 * @param city_id Identificador da cidade.
	 * @param index Multiplicador (0 = sem renda externa; 1 = média mundial).
	 *              Limitado a [0, 8] para conter dados corrompidos.
	 */
	void set_external_income_index(const String& city_id, double index);

	/**
	 * F7 calibração: produtividade nacional (TFP) de uma cidade, derivada do
	 * PIB PPP per capita REAL já carregado pelo `GeographyService`
	 * (`external_income_index`, `source: derived:gdp_ppp`).
	 *
	 * Por padrão (índice armazenado = 0) a TFP é DERIVADA do índice de renda:
	 * `clamp(external_income_index, 0,25, 4,0)` — EUA ~3,6, DEU ~3,0, BRA ~0,9,
	 * IND ~0,47, NGA ~0,38 da média mundial. O setter permite que o laboratório
	 * declare um fator explícito; `index <= 0` volta à derivação.
	 *
	 * A TFP multiplica o VALOR ADICIONADO POR TRABALHADOR usado na âncora
	 * salarial (parcela do trabalho × valor adicionado × TFP ÷ vagas
	 * preenchidas), de modo que a folha reflita a produtividade do país. Não
	 * cria dinheiro: o pagamento continua limitado ao caixa da firma
	 * (`pay_scale`), então a diferença aparece na distribuição entre salário e
	 * lucro — países pobres com salário baixo e lucro concentrado nos donos,
	 * ricos com folha maior.
	 * @param city_id Identificador da cidade.
	 * @param index Fator explícito (1 = média mundial; <= 0 = derivar).
	 *              Limitado a [0,05, 8].
	 */
	void set_productivity_index(const String& city_id, double index);

	/**
	 * F7: sobrescreve o perfil de trabalho/educação de uma cidade (hook de
	 * teste/administração e migração). Chaves aceitas (todas opcionais):
	 * `working_age_share` (0..1), `retiree_share` (0..1), `participation`
	 * (array de 3 frações, ou número único) e `education` (array de 3 frações
	 * que somam 1, básico/intermediário/superior). Sem chave, o valor derivado
	 * do índice de renda é mantido. Recalcula a força de trabalho por nível e
	 * reinicializa os salários no âncora do país; não move a população.
	 */
	void set_labor_profile(const String& city_id, const Dictionary& profile);

	/**
	 * F7: salário mínimo por nação (piso diário, em XGS). `value <= 0` desliga
	 * o piso (padrão do jogo). Gancho de política: a UI/governo pode definir.
	 */
	void set_minimum_wage(const String& nation, double value);

	/**
	 * F7: rigidez salarial (fração máxima de ajuste por dia, 0..0,25).
	 * Padrão 0,01 (1%/dia). Gancho de política/sindicatos.
	 */
	void set_wage_rigidity(double value);

	/**
	 * Define o teto de demanda externa (t/dia) de um produto para uma cidade.
	 * É a demanda LIMITADA do resto do mundo para bens brutos (minerais, soja,
	 * algodão e madeira): a fase 4d compra no máximo este teto por dia, e a
	 * produção acima dele vira estoque, o que derruba o preço (fase 8). O teto
	 * vem da participação da nação no mercado mundial — reserva esgotada em ~25
	 * anos (GeographyService.apply_to_sim). Sem teto declarado (0), o bem não é
	 * exportado (comportamento legado dos bens sem depósito/aptidão).
	 * @param product id de goods.json (ex.: "coal", "soy").
	 * @param ceiling_t_per_day Teto diário em toneladas (>= 0).
	 */
	void set_external_demand(const String& city_id, const String& product,
		double ceiling_t_per_day);

	/**
	 * F8: carrega os setores de serviço (services.json → `sectors`). Pode ser
	 * chamado antes ou depois do mundo: as definições são só catálogo; os
	 * mercados por cidade existem sempre (preço-base default até o dado chegar).
	 * Um arquivo com contagem diferente de kMaxServiceSectors é recusado
	 * (a publicação é indexada por posição).
	 */
	void set_services(const Variant& services);

	/**
	 * F9: carrega a estrutura de emprego setorial (sector_employment.json).
	 * Determinística: a mesma entrada gera a mesma fração por cidade. Chame
	 * antes (ou depois) de criar o mundo — os valores são recalculados todo dia
	 * em update_sector_employment. Sem chamada, vale o espelho compilado
	 * das cinco âncoras (testes).
	 */
	void set_sector_employment(const Variant& data);

	/**
	 * B1: carrega a estrutura etária por nação (age_structure_2026.json).
	 * DEFINIÇÃO estática no WorldState: `SimCore::seed_demography` semeia as
	 * coortes das cidades a partir dela, `apply_labor_profile` deriva idade
	 * ativa/aposentados/children e o fluxo mensal (`phase_demography`) move
	 * as coortes. Ela viaja no snapshot do CLI; o save de estado guarda só as
	 * coortes resultantes. Aceita só DICTIONARY com {"nations": {tag: {...}}};
	 * nação com faixas que não somam ~1 ([0,98, 1,02]) é ignorada. Sem nação
	 * válida, o mapa atual não muda.
	 */
	void set_age_structure(const Variant& data);

	/**
	 * B7: carrega a taxa de emprego informal por nação (informality.json) e a
	 * cobertura de saúde de referência (health_access.json). DEFINIÇÃO estática
	 * no WorldState (como `age_structures`): a fração inicial é semeada no dia 0
	 * (`SimCore::seed_informality`) e a referência de saúde é lida na semeadura
	 * e na mortalidade mensal. Aceita só DICTIONARY com {"nations": {tag: v}},
	 * onde `v` é um número (fração 0..1) ou {"value": número}. Nação sem entrada
	 * cai na âncora de renda (informalidade) ou em kHealthAccessDefault.
	 */
	void set_informality(const Variant& data);
	void set_health_access(const Variant& data);

	/**
	 * B5 (D-089): NÍVEL INICIAL de infraestrutura do estado (estrada/energia/
	 * água, 0..kInfraMaxLevel) por id de estado de `state_regions.json`. O
	 * GDScript aplica a regra %->nível do dado real
	 * (`core/economy/initial_infra.gd`) e entrega
	 * `{state_id: {road, power, water}}`. Chame DEPOIS de `initialize_world`
	 * (as regiões precisam existir) e ANTES do primeiro dia; o save carrega os
	 * níveis persistidos, então o setter é só do estado inicial.
	 *
	 * Região com mais de um estado usa o MENOR nível de cada tipo entre eles
	 * (pior caso — a mesma lógica rural do dado). Estado sem entrada = 0
	 * (ausência explícita, nunca cobertura zero). Cidade sem bloco `region`
	 * ou sem `states` não é tocada. Arquivo ausente = `{}` = nada muda.
	 */
	void set_region_infra(const Variant& data);

	/**
	 * B6 (D-095) — PATRIMÔNIO DA REGIÃO a partir do dado (`land_property.json`
	 * + a regra de rateio nacional→estado aplicada no GDScript). `data` =
	 * `{city_id: {land_ha, land_gini, gini_observed}}` (ou por estado). Idempotente:
	 * só age quando a região ainda não tem patrimônio (`property_ready` falso) —
	 * um save v23 carrega o estado gravado, e um save v22 recebe o dado agora.
	 * Cidade sem hectares > 0 não é tocada (ausência explícita). Arquivo ausente
	 * = `{}` = nada muda.
	 */
	void set_region_property(const Variant& data);

	/**
	 * B6 — alíquota ANUAL do imposto sobre a propriedade (0,0 por padrão) de
	 * todas as regiões da nação. Campo persistido e publicado como receita
	 * potencial; nada é arrecadado enquanto for 0 (impostos são do macro
	 * político). Fora de [0,1] é limitado.
	 */
	void set_property_tax_rate(const String& nation, double rate);

	/**
	 * §19 (W0): carrega solo urbano, moradia e finanças municipais dos quatro
	 * JSON (`urban_land`, `housing_city`, `housing`, `municipal_finance`, já
	 * parseados pelo GDScript). Chame DEPOIS de criar o mundo: o casamento é
	 * pelo id da cidade. Nenhuma lógica diária roda ainda — é só o estado
	 * inicial (área por zona, densidade, estoque, posse, aluguel/preço e taxas
	 * da prefeitura). Cidade sem entrada fica com defaults neutros.
	 */
	void set_urban(const Dictionary& urban_land,
		const Dictionary& housing_city, const Dictionary& housing,
		const Dictionary& municipal_finance);

	/**
	 * §19 (W0): leitura do estado urbano de uma cidade (teste/UI). Arrays na
	 * ordem dos enums: zonas [res, com, ind]; densidades [low, mid, high];
	 * padrões [popular, medio, alto]; posse [própria, aluguel, informal];
	 * donos [família, empresa, governo]. Vazio se a cidade ou o dado não existe.
	 */
	Dictionary get_urban_city(const String& city_id) const;

	/**
	 * §19 (W1): abre obra §15 de EXPANSÃO de uma zona urbana
	 * (`km2` consome reserve_km2 e amplia a infraestrutura). Paga o orçamento
	 * do governo da cidade; o W2 passa a financiar pela prefeitura. Devolve
	 * {ok, error, project_id, cost}; a área/capacidade só muda na conclusão.
	 */
	Dictionary urban_expand(const String& city_id, int zone, double km2);

	/**
	 * §19 (W1): abre obra §15 de ADENSAMENTO (sobe max_density da zona em um
	 * passo). Exige infraestrutura que atenda a população da cidade. Devolve
	 * {ok, error, project_id, cost}.
	 */
	Dictionary urban_densify(const String& city_id, int zone);

	/**
	 * §19 (integração) — moradia social da prefeitura: abre o projeto de
	 * moradia do W3 (dono GOVERNMENT) pagando do caixa municipal. Devolve
	 * {ok, units, cost} com o que efetivamente foi contratado/pago.
	 */
	Dictionary urban_social_housing(const String& city_id, double units);

	/** F8: catálogo carregado (id, nomes, ícone, educaçao, exportável). */
	Array get_service_sectors() const;

	/**
	 * F8: detalhe de uma cidade para a tela de Serviços — mercados dos 20
	 * setores + negócios (dono, capacidade, emprego, lucro, semente do nome).
	 * Fora do status diário de propósito: a lista completa de negócios não é
	 * republicada a cada tick.
	 */
	Dictionary get_services_detail(const String& city_id) const;

	/**
	 * F8: hook de teste/administração — fixa a capacidade AGREGADA de um setor
	 * numa cidade, redistribuindo entre os negócios existentes (ou criando um
	 * se não houver). Serve para medir o efeito da educação/saúde privadas sem
	 * depender da entrada endógena.
	 */
	void set_service_capacity(const String& city_id, const String& sector,
		double units_per_day);

	/** F8: hook de teste — força lucro/dias de prejuízo de um negócio. */
	void set_service_business_state(int business_id, int loss_days,
		double profit);

	/** F8: número de negócios de serviço ativos (não falidos). */
	int get_service_business_count() const;

	/** F8: falências de negócios de serviço acumuladas. */
	int get_service_bankruptcies() const;

	/**
	 * D-099: composição das falências de CONSTRUÇÃO acumuladas (contadores de
	 * runtime, zerados no load): `by_product` (por índice de bem, na ordem do
	 * catálogo), `planned_primary` (primárias com plano de safra) e `total`.
	 * Diagnóstico do aceite de 365 d; não entra em `get_status()`.
	 */
	Dictionary get_bankruptcy_diag() const;

	/**
	 * Investe capital da empresa na expansão de uma construção existente.
	 * O valor sai do caixa da empresa e é pago ao setor de construção.
	 * @param city_id Cidade da construção.
	 * @param building_id Identificador da construção.
	 * @param amount Capital a investir.
	 * @returns Resumo com "invested", "capacity_added" e "error".
	 */
	Dictionary invest(const String& city_id, const String& building_id, double amount);

	/**
	 * Cria uma construção nova. O custo sai do caixa da empresa (private) ou do
	 * orçamento do governo (state) e é pago ao setor de construção.
	 * `recipe_id` (opcional, v4): receita industrial de recipes.json; quando
	 * presente, input_product/input_per_output são derivados do primeiro
	 * insumo da receita (compatibilidade de exibição) e a fase de produção usa
	 * a receita completa (1..6 insumos + subprodutos).
	 * @returns Resumo com "created", "cost" e "error".
	 */
	Dictionary build(const String& city_id, const String& building_id,
		const String& resource, int input_product, int output_product,
		double capacity, double jobs, double wage, double opex,
		double input_per_output, const String& owner, double cost,
		const String& recipe_id = "");

	/**
	 * Gasta orçamento público em serviços domésticos (ex.: prospecção).
	 * @returns true quando havia orçamento suficiente.
	 */
	bool spend_government(const String& city_id, double amount);

	/**
	 * Expande a capacidade de um porto (obra pública): o custo sai do orçamento
	 * da cidade-sede e é pago ao setor de construção; a capacidade entra em
	 * operação depois de `kPortBuildDays` dias.
	 * @param port_id Identificador do porto (ports.json).
	 * @param t_per_day Capacidade a adicionar, em t/dia.
	 * @returns {expanded, cost, days, capacity, error}.
	 */
	Dictionary expand_port(const String& port_id, double t_per_day);

	/**
	 * Hook de teste/administração: define a capacidade corrente de um porto,
	 * mantendo a base (nível 1). Não cobra nem agenda obra.
	 */
	void set_port_capacity(const String& port_id, double t_per_day);

	/**
	 * Hook de teste: liga/desliga a construção instantânea (§15). Desligado por
	 * padrão, todo `build`/`group_invest`/`invest` vira um
	 * PROJETO físico (materiais + prazo + pagamento incremental). Ligado
	 * restaura o comportamento antigo (prédio pronto na hora, custo integral na
	 * criação) — usado pelos testes que não medem cronograma de obra.
	 */
	void set_instant_construction(bool instant);

	/**
	 * A5: liga/desliga a FASE DA IA C++ no fim do dia (`AiPhase::run`).
	 * DESLIGADO por padrão: os testes e o caminho GDScript existentes não
	 * mudam. Com a IA ligada, `tick_and_publish` roda a fase já com o dia
	 * fechado e ANTES da publicação do status único (D-076): `tick_day()`
	 * devolve o status do dia **com** os efeitos da IA. Só grava a flag: NÃO
	 * reseta o estado da IA (`reset` é do mundo novo —
	 * initialize_scenario/initialize_world/from_snapshot*).
	 */
	void set_ai_enabled(bool enabled);

	/** Serializa todo o estado do cenário (save). */
	Dictionary to_dict() const;

	/** Restaura o estado do cenário a partir de to_dict() (load). */
	void from_dict(const Dictionary& data);

	/**
	 * Superfície de ferramenta/teste (A1a): snapshot ATÔMICO do mundo no formato
	 * que o CLI headless consome — {"definitions": bloco estático das
	 * definições, "save": o mesmo dicionário de to_dict()}. Serve para
	 * exportar o mundo do jogo e rodá-lo na biblioteca sem o Godot.
	 */
	Dictionary to_snapshot() const;

	/**
	 * Superfície de ferramenta/teste (A1a): restaura um snapshot de
	 * to_snapshot(). Aplica `definitions` e depois `save` (o save depende
	 * do bloco estático) e invalida o cache de status. Sem a chave
	 * `definitions` ou `save`, registra erro e não toca no mundo.
	 */
	void from_snapshot(const Dictionary& snapshot);

	/**
	 * Superfície de ferramenta/teste (A1): a MESMA fotografia de
	 * to_snapshot(), gravada em arquivo pelo serializador da BIBLIOTECA
	 * (`ogs::save::dump_json`), sem passar pelo JSON do Godot.
	 *
	 * Motivo: o par `JSON.stringify`/`JSON.parse_string` do Godot NÃO é
	 * round-trip IEEE-exato para |x| < 1 (0.0002886172890222971 sai como
	 * 0.00028861728902229); o nlohmann da biblioteca, por onde já passa o CLI
	 * headless, é exato. Um snapshot que atravessa o JSON do engine perde bits
	 * e deixa de ser comparável campo a campo com o CLI.
	 *
	 * Grava UTF-8 sem BOM em modo binário (`std::ofstream`), com o mesmo
	 * formato de `gdextension/bin/opengs_sim_cli.exe --snapshot=...`.
	 * @returns false (com push_error) quando o arquivo não abre ou a escrita
	 *          falha.
	 */
	bool to_snapshot_file(const String& path) const;

	/**
	 * Superfície de ferramenta/teste (A1): restaura um snapshot gravado por
	 * to_snapshot_file(), lido com `ogs::save::parse_json_file` (exato,
	 * sem o JSON do Godot). Exige as chaves `definitions` e `save`, invalida o
	 * cache de status exatamente como from_snapshot e aplica as definições
	 * ANTES do save.
	 * @returns false (com push_error) em arquivo ilegível, JSON inválido ou
	 *          chave ausente/inválida — nesse caso o mundo NÃO é tocado.
	 */
	bool from_snapshot_file(const String& path);

	/**
	 * Ajusta parâmetros de balanceamento usados pelo laboratório.
	 * Chaves aceitas: natural_unemployment, service_wage_adjust,
	 * service_wage_max, inventory_cover_days, inventory_recovery,
	 * trade_neighbors, trade_external, income_basket_margin,
	 * infra_access_penalty (B5, penalidade máxima de acesso com estrada 0),
	 * harvest_credit_rate (D-105: 1 liga o juro da taxa básica no envelope de
	 * crédito da safra; padrão 0 = desligado até a C4).
	 */
	void set_balance(const Dictionary& values);

	/**
	 * F4: define a frota nacional (hook de teste/administração). Marca a frota
	 * como `derived: false` (valor declarado, não derivado). Libera os veículos
	 * ocupados; o combustível e as peças passam a ser exatamente os informados.
	 */
	void set_fleet(const String& nation, double trucks, double ships,
		double planes, double fuel, double spare_parts);

	/** F4: fotografia da frota de uma nação (contagens, saúde, fila do dia). */
	Dictionary get_fleet(const String& nation) const;

	/**
	 * F4: compra `amount` toneladas do bem no mercado interno da nação e
	 * transfere para a frota (1 t de truck/cargo_ship/cargo_plane = 1 veículo;
	 * fuel/spare_parts vão para o estoque). Paga com o orçamento da cidade-sede
	 * (maior população de simulação), até `budget_share` do caixa dela.
	 * @returns {bought, spent, unfilled, error}.
	 */
	Dictionary fleet_buy(const String& nation, const String& product,
		double amount, double budget_share = 0.1);

	/**
	 * Hook de teste/administração: define o estoque de um produto no mercado de
	 * uma cidade (não é usado pelo jogo; serve para exercitar a compra de frota
	 * em cenários controlados).
	 */
	void set_market_stock(const String& city_id, const String& product,
		double stock);

	/**
	 * Hook de teste/administracao: define o preco (na ANCORA XGS) de um produto
	 * no mercado de uma cidade. A moeda local e so exibicao (`price_local`).
	 */
	void set_market_price(const String& city_id, const String& product,
		double price);

	/**
	 * F5: carrega as moedas (currencies.json). Cada item traz `code`,
	 * `nations[]`, `rate_per_anchor` (valor em XGS de 1 unidade local) e
	 * `source`; `rate_per_anchor` nulo/<=0 e uma moeda `unknown:no_gdp` e usa
	 * o fallback documentado (mediana das taxas conhecidas, source
	 * `fallback:median`). `regime` "peg" congela a taxa; ausente/"float" deixa
	 * o cambio dinamico por balanca comercial. Pode ser chamado antes ou depois
	 * do mundo: os precos ficam na ancora e NAO sao reescalados.
	 */
	void set_currencies(const Variant& currencies);

	/** F5: taxa de cambio da moeda de uma nacao (ancora por unidade local). */
	Dictionary get_currency(const String& nation) const;

	/** F5: todas as moedas carregadas (status/UI e testes). */
	Array get_currencies() const;

	/**
	 * F5: hook de teste/administracao para a taxa de uma moeda (por code).
	 * Tambem fixa a base da banda (`base_rate`; sem ele, a propria taxa).
	 * `floating` controla o drift diario e o fator de competitividade.
	 */
	void set_currency_rate(const String& code, double rate,
		bool floating = true, double base_rate = -1.0);

	/**
	 * F5: tarifa ad valorem (0..1) cobrada do comprador `buyer_nation` sobre o
	 * valor CIF (origem convertida + frete) do bem `product` importado de
	 * `seller_nation`. `seller_nation = "*"` vale para qualquer origem.
	 * `product = "*"` vale para todos os bens. A tarifa e receita do governo
	 * do comprador (dinheiro conservado).
	 */
	void set_tariff(const String& buyer_nation, const String& seller_nation,
		const String& product, double rate);

	/** F5: tarifa efetiva (buyer, seller, product) apos os coringas. */
	double get_tariff(const String& buyer_nation,
		const String& seller_nation, const String& product) const;

	/** F5: tarifas declaradas (status/UI). */
	Array get_tariffs() const;

	// ------------------------------------------------------------------
	// F9 — investimento e propriedade (produção_chains_impl_plan §14).
	//
	// Grupos/empresas com identidade: id determinístico, nome gerado no
	// cliente (core/culture/business_name_generator.gd) e entregue por
	// set_group_names, nação de origem, setor principal, caixa e
	// carteira de ativos em qualquer país. O núcleo guarda o dinheiro e a
	// propriedade; a decisão de investir é de cadência baixa e roda no
	// GDScript (core/economy/ai/group_investor.gd), nunca por tick.
	//
	// Propriedade de uma construção (`Building::owner`):
	//   "state"                 estatal (lucro -> orçamento do anfitrião)
	//   "private"               privado agregado legado (sem grupo)
	//   "national:<grupo>"      grupo privado nacional
	//   "foreign:<grupo>@<nação>" grupo estrangeiro (repatria o lucro)
	// ------------------------------------------------------------------

	/**
	 * Semeia os grupos a partir dos ativos privados existentes, agrupando por
	 * (nação, setor) em lotes de até 12 ativos. Determinístico e idempotente:
	 * com grupos já criados, não faz nada. O caixa nasce em 0 (nenhum dinheiro
	 * é criado); os grupos se capitalizam com o lucro retido.
	 */
	void seed_groups();

	/** Nomes gerados pelo cliente (id -> nome). Devolve quantos aplicou. */
	int set_group_names(const Dictionary& names);

	/** Ranking de grupos (ativos, faturamento, países, caixa, setor). */
	Array get_groups() const;

	/**
	 * Ranking LEVE para a IA: mesma ordem de `get_groups` (faturamento,
	 * caixa, id), mas sem varrer construções nem montar conjuntos por grupo.
	 * Devolve só os campos de decisão (id, origem, setor, caixa, regime,
	 * faturamento, distribuível). Custo O(grupos log grupos), nunca O(ativos).
	 */
	Array get_group_ranking() const;

	/** Fotografia de um grupo (ou {} se não existe). */
	Dictionary get_group(const String& group_id) const;

	/**
	 * Move `amount` do orçamento das cidades da nação de origem do grupo para
	 * o caixa dele (capitalização explícita; dinheiro conservado). Usado por
	 * testes e pela administração. Devolve {funded, moved, error}.
	 */
	Dictionary fund_group(const String& group_id, double amount);

	/**
	 * Investimento de um grupo (FDI ou doméstico), com a política de abertura
	 * do setor aplicada:
	 *  - `closed`: recusa investimento estrangeiro;
	 *  - `joint_venture`: exige `partner_group` nacional com participação
	 *    local >= min_local (o parceiro paga a fatia dele);
	 *  - `restricted`: estrangeiro só mantém posição já existente no setor;
	 *  - `open`: liberado.
	 * O custo sai do caixa do grupo (e do parceiro, no JV) e é pago ao setor
	 * de construção do anfitrião. Devolve {created, cost, error, ...}.
	 */
	Dictionary group_invest(const String& group_id, const String& city_id,
		const String& building_id, const String& resource, int output_product,
		double capacity, const String& recipe_id,
		const String& partner_group = "");

	/**
	 * D-086 — APROFUNDAR/EXPANDIR mina: obra física §15 que abre o estágio
	 * seguinte da mina, repõe a produção (curva de declínio do estágio) e sobe
	 * a capacidade, reivindicando uma fatia da reserva de REFERÊNCIA da jazida
	 * (o recurso nunca acaba — a reserva não é um limite físico). O custo sai
	 * do caixa do DONO (Estado/grupo/firma) e vira demanda de construção.
	 * Devolve {started, cost, capacity_added, stage, duration_days, project_id,
	 * error}. Comando da fila A3 (`deepen_mine`).
	 */
	Dictionary deepen_mine(const String& city_id, const String& building_id);

	/**
	 * B5: obra de infraestrutura da região/estado (estrada/energia/água).
	 * `infra_type`: 0 estrada, 1 energia (luz), 2 água. Sobe UM nível (até
	 * kInfraMaxLevel) via projeto físico §15, pago pelo GOVERNO NACIONAL.
	 * Devolve {started, cost, infra_type, next_level, duration_days, project_id,
	 * error}. Comando da fila A3 (`infra_build`).
	 */
	Dictionary infra_build(const String& city_id, int infra_type);

	/**
	 * F9: abertura ao capital estrangeiro por (nação, setor).
	 * `mode`: `open` (padrão), `restricted`, `joint_venture`, `closed`.
	 * `min_local`: fração mínima do parceiro local em joint venture (0..1).
	 */
	Dictionary set_fdi_policy(const String& nation, const String& sector,
		const String& mode, double min_local = 0.0);

	/** F9: incentivo setorial (isenção de imposto e subsídio por unidade). */
	Dictionary set_sector_incentive(const String& nation,
		const String& sector, double tax_holiday, double subsidy);

	/**
	 * Cria uma empresa estatal (grupo `state_owned`) capitalizada com
	 * `capital` retirado do orçamento das cidades da nação (conserva o
	 * dinheiro). Devolve {created, id, error}.
	 */
	Dictionary create_state_company(const String& nation,
		const String& name, double capital);

	/**
	 * Privatiza um ativo estatal: o comprador paga o valor contábil
	 * (capacidade × capital por capacidade) ao governo do anfitrião.
	 */
	Dictionary privatize(const String& city_id, const String& building_id,
		const String& buyer_group);

	/**
	 * Nacionaliza um ativo privado/estrangeiro: vira estatal, o anfitrião paga
	 * compensação de 50% do valor contábil (se houver orçamento) e a confiança
	 * da nação cai por `kNationalizationDays` dias — o que encarece o IDE.
	 */
	Dictionary nationalize(const String& city_id, const String& building_id);

	/**
	 * B5: concede uma mina/fazenda estatal a um grupo (nacional/estrangeiro)
	 * com um royalty sobre a produção (`building.royalty`). Transferência de
	 * dinheiro conservada (o concessionário paga o valor contábil ao governo).
	 * `royalty_rate` (0..1; <= 0 usa o padrão kConcessionRoyaltyRate). Devolve
	 * {ok, price, royalty, owner, error}. Comando da fila A3 (`concede`).
	 */
	Dictionary concede(const String& city_id, const String& building_id,
		const String& group_id, double royalty_rate);

	/**
	 * Status de investimento (sob demanda, fora do tick): ranking de grupos,
	 * participação estatal/nacional/estrangeira na produção e no emprego de
	 * cada nação, fluxo de lucro repatriado e políticas/incentivos.
	 */
	Dictionary get_investment_status() const;

	/**
	 * Sinais de investimento BARATOS para a IA (sem varredura de ativos nem
	 * ranking): confiança por nação, políticas de IDE e incentivos setoriais.
	 * É o que a decisão de grupo consome — o status completo (participação por
	 * dono) fica para a UI e os relatórios.
	 */
	Dictionary get_investment_signals() const;

	/** F9: confirmação de que os grupos foram semeados neste mundo. */
	bool groups_seeded() const { return ws_.groups_seeded; }

protected:
	static void _bind_methods();

private:
	// ------------------------------------------------------------------
	// A3: aplicação dos comandos. Cada `apply_*` é o corpo antigo do método
	// público, SEM o mutex (quem aplica já é o escritor único) e SEM
	// `status_invalidate()` (a barreira que aplicou invalida uma vez).
	// O método público só monta o Command, enfileira e chama `dispatch_command`.
	// ------------------------------------------------------------------
	Dictionary dispatch_command(const ogs::Command& cmd);
	Dictionary apply_one_locked(const ogs::Command& cmd);
	// Aplica a fila pendente; quando `wanted_seq` != 0, devolve no retorno o
	// resultado daquele comando (é o contrato de retorno dos métodos públicos).
	Dictionary apply_pending_locked(uint64_t wanted_seq);

	Dictionary apply_invest(const String& city_id, const String& building_id,
		double amount);
	// D-086: aprofundar/expandir mina (obra física §15 → estágio seguinte).
	Dictionary apply_deepen_mine(const String& city_id,
		const String& building_id);
	// B5: obra de infraestrutura da região (estrada/energia/água).
	Dictionary apply_infra_build(const String& city_id, int infra_type);
	Dictionary apply_storage_build(const String& city_id, int type, int scope,
		double tons);
	Dictionary apply_storage_trade(const String& city_id, int type, int scope,
		int product, double tons, bool buy, bool relief);
	Dictionary apply_build(const String& city_id, const String& building_id,
		const String& resource, int input_product, int output_product,
		double capacity, double jobs, double wage, double opex,
		double input_per_output, const String& owner, double cost,
		const String& recipe_id);
	bool apply_spend_government(const String& city_id, double amount);
	void apply_price_shock(const String& product, double multiplier);
	void apply_set_suitability(const String& city_id, const String& product,
		double value);
	bool apply_set_deposit(const String& city_id, const String& product,
		double reserve_kt, double quality, double depth_m);
	void apply_set_external_demand(const String& city_id,
		const String& product, double ceiling_t_per_day);
	void apply_set_external_income_index(const String& city_id, double index);
	void apply_set_productivity_index(const String& city_id, double index);
	void apply_set_income_gini(const String& city_id, double gini);
	// B2: quintis de renda observados da nação (5 frações, soma ~1) ou vazio
	// para re-semear com a proxy do Gini declarado.
	void apply_set_city_strata(const String& city_id,
		const PackedFloat64Array& shares);
	// B3: aplica o bloco de região (setup; ver set_city_region).
	void apply_set_city_region(const String& city_id, const Dictionary& data);
	void apply_set_minimum_wage(const String& nation, double value);
	void apply_set_wage_rigidity(double value);
	void apply_set_tariff(const String& buyer_nation, const String& seller_nation,
		const String& product, double rate);
	void apply_set_currency_rate(const String& code, double rate,
		bool floating, double base_rate);
	Dictionary apply_expand_port(const String& port_id, double t_per_day);
	void apply_set_port_capacity(const String& port_id, double t_per_day);
	void apply_set_market_stock(const String& city_id, const String& product,
		double stock);
	void apply_set_market_price(const String& city_id, const String& product,
		double price);
	Dictionary apply_fleet_buy(const String& nation, const String& product,
		double amount, double budget_share);
	Dictionary apply_fund_group(const String& group_id, double amount);
	Dictionary apply_group_invest(const String& group_id, const String& city_id,
		const String& building_id, const String& resource, int output_product,
		double capacity, const String& recipe_id, const String& partner_group);
	Dictionary apply_set_fdi_policy(const String& nation, const String& sector,
		const String& mode, double min_local);
	Dictionary apply_set_sector_incentive(const String& nation,
		const String& sector, double tax_holiday, double subsidy);
	Dictionary apply_create_state_company(const String& nation,
		const String& name, double capital);
	Dictionary apply_privatize(const String& city_id, const String& building_id,
		const String& buyer_group);
	Dictionary apply_nationalize(const String& city_id, const String& building_id);
	// B5: concessão de mina/fazenda com royalty.
	Dictionary apply_concede(const String& city_id, const String& building_id,
		const String& group_id, double royalty_rate);
	Dictionary apply_urban_expand(const String& city_id, int zone, double km2);
	Dictionary apply_urban_densify(const String& city_id, int zone);
	Dictionary apply_urban_social_housing(const String& city_id, double units);
	void apply_set_service_capacity(const String& city_id, const String& sector,
		double units_per_day);
	void apply_set_service_business_state(int business_id, int loss_days,
		double profit);

	// Fila de comandos (A3). `tick_in_progress_` diz se um dia está rodando em
	// outra thread: nesse caso o método público só enfileira. `tick_thread_`
	// (hash da thread do dia) separa "outra thread" de "a própria thread do
	// dia": as mutações que o tick faz por dentro (seed_industry → build)
	// precisam aplicar na hora, como sempre fizeram.
	ogs::CommandQueue commands_;
	std::atomic<bool> tick_in_progress_{false};
	std::atomic<std::size_t> tick_thread_{0};
	std::atomic<int> current_day_{0};

	// ------------------------------------------------------------------
	// Cache do status: os mesmos Dictionary/Array são reescritos a cada dia.
	//
	// Motivo: montar o status do zero custa ~62 ms em 650 cidades (650 × 21
	// dicionários de mercado, 650 POPs, 650 camadas de construção). Reescrever
	// valores em contêineres existentes não aloca e não re-hasheia a estrutura.
	// Campos imutáveis (id, nome, centro, aptidão) só são reescritos quando a
	// origem muda, comparando contra a cópia em std::string (memcmp barato).
	// ------------------------------------------------------------------
	struct StatusCitySlot {
		// Quantidade de escalares publicados em fill_city_status; o
		// espelho abaixo é indexado por ordem de chamada de sync_scalar.
		// F9: eram 31 chamadas com espelho de 30 (escrita fora do limite desde a
		// F7); agora 33 (service_jobs_filled e in_kind_food entraram).
		static constexpr int kStatusScalarCount = 33;
		Dictionary dict;
		// Strings estáveis: evitam realocar String a cada dia.
		std::string id_src, name_src, nation_src, home_src, landmass_src, currency_src;
		String id, name, nation, home_nation, landmass, currency;
		Array center;
		Array suitability;
		Array external_demand;
		Dictionary deposits;
		std::vector<Dictionary> deposit_entry;  // 1 por produto (lazy)
		// Bloco estático de cada jazida (reserva inicial, qualidade,
		// profundidade, eficiência, opex) escrito UMA vez; por dia só mudam
		// reserva restante, extraído e esgotado. Antes o dicionário inteiro era
		// reconstruído a cada dia (String + Dictionary aninhado por cidade).
		std::vector<char> deposit_present;      // 1 por produto
		std::vector<char> deposit_depleted;     // último `depleted` publicado
		std::vector<Deposit> deposit_written;  // último valor publicado
		std::vector<String> deposit_key;        // nome do produto (chave)
		// Lista de produtos com jazida rastreada nesta cidade. Recalculada só
		// quando uma jazida nova é descoberta: o laço diário percorre apenas as
		// jazidas da cidade, não os 113 bens (medido: ~4,5 ms/dia só de varredura).
		std::vector<int> deposit_tracked;
		int deposit_tracked_version = -1;
		bool deposits_published = false;
		Dictionary pop;
		// Espelhos do que já foi escrito em `pop`: só reescreve o que mudou
		// (cada escrita é uma chamada à ponte GDExtension com hash da chave).
		// 26 campos da F3 + 7 da F7 (idade ativa, aposentados, estudantes,
		// reserva recrutável, nível de renda, salário mínimo, rigidez).
		double pop_written[33] = {};
		bool pop_written_filled = false;
		bool pop_published = false;
		// B1: coortes etárias publicadas no bloco `pop` (5 faixas). Mudam uma
		// vez por mês (fase de demografia): o Array só é republicado quando o
		// conteúdo muda, como o grupo "raro".
		std::vector<double> age_bands;
		double age_bands_written[5] = {};
		bool age_bands_filled = false;
		// B2: bloco `pop.income_strata` (5 quintis). Republicado quando o
		// conteúdo muda (fluxos diários; patrimônio no fechamento do mês).
		Dictionary income_strata;
		std::vector<double> income_strata_written;
		bool income_strata_filled = false;
		// F7: vetores de educação/trabalho reutilizados (3 posições cada). São
		// mutados em lugar e compartilhados com o Dictionary publicado; só são
		// reinseridos quando o bloco é criado.
		Array education;
		Array education_share;
		Array labor_force_by_level;
		Array employed_by_level;
		Array unemployed_by_level;
		Array unemployment_by_level;
		Array vacancies_by_level;
		Array wage_by_level;
		Array wage_income_by_level;
		Array studying;
		// F7 calibração: renda por estrato (nível de consumo de cada estrato,
		// fatia da POP por nível e renda disponível do estrato).
		Array income_level_share;
		Array income_level_by_stratum;
		Array stratum_disposable;
		Array stratum_population;
		Array stratum_wage_income;
		// F7 perf: espelhos dos vetores acima. Eles são mutados em lugar (e
		// compartilhados com o `pop` publicado), então cada elemento escrito é
		// uma chamada Variant. Educação/estratos mudam em escala de semanas:
		// só reescreve o grupo quando o conteúdo muda de fato.
		double labor_rare_written[16] = {};   // education, força, estudo, fatias, níveis
		bool labor_rare_filled = false;
		double labor_daily_written[24] = {};  // folha/emprego/vagas por nível
		bool labor_daily_filled = false;
		double stratum_written[12] = {};      // renda/população/folha por estrato
		bool stratum_written_filled = false;
		// Espelho dos 25 escalares publicados por cidade (população, PIB,
		// caixas, indicadores derivados): escrita só quando o double muda.
		double scalar_written[kStatusScalarCount] = {};
		bool scalar_written_filled = false;
		// desejado/consumido por categoria alimentar (2 × kMaxFoodProducts).
		double pop_food_written[2 * kMaxFoodProducts] = {};
		Array desired_by_category;
		Array consumed_by_category;
		Array markets;
		std::vector<Dictionary> market_entry;  // 1 por produto
		// F8: bloco plano de serviços (20 setores × kServiceFields). Array
		// de doubles em vez de 20 dicionários por cidade: a publicação custa uma
		// escrita só e a UI indexa pela ordem de `service_sectors`.
		PackedFloat64Array services;
		PackedFloat64Array services_written;
		bool services_filled = false;
		// F9: emprego setorial (4 posições: agro, indústria, serviços,
		// informal) e a fração do emprego. Espelho de 8 doubles evita
		// reescrever o PackedFloat64Array quando nada mudou.
		PackedFloat64Array sector_workers;
		PackedFloat64Array sector_share;
		double sector_written[8] = {};
		bool sector_written_filled = false;
		bool sector_published = false;
		Array buildings;
		std::vector<Dictionary> building_entry;  // cresce com a cidade
		// `buildings` é Array (referência): o dicionário publicado enxerga os
		// elementos mutados. Só é preciso reinserir cada `entry` quando o
		// conjunto muda de tamanho (ou a identidade do prédio no índice muda).
		bool buildings_bound = false;
		bool buildings_published = false;
		// Bloco ESTÁTICO de cada construção (id, kind, resource, owner, receita,
		// insumos, subprodutos): escrito uma vez e reescrito só quando a
		// construção muda. Antes cada prédio alocava Strings + Arrays todos os
		// dias (medido: ~12 ms/dia com ~4 mil prédios no dia 30).
		struct BuildingCache {
			std::string id_src, resource_src, owner_src;
			String id, resource, owner;
			bool filled = false;
			int recipe = -2;
			// Últimos valores diários publicados (comparação de double evita
			// reescrever o Dictionary para prédios parados).
			double capacity = -1.0, jobs = -1.0, wage = -1.0, opex = -1.0;
			double output_scale = -1.0, site = -1.0, last_utilization = -1.0;
			double workers = -1.0, utilization = -1.0, revenue = -1.0;
			double input_value = -1.0, costs = -1.0, profit = -1.0;
			double loss_days = -1.0;
			// D-086: estágio da mina (int) e produção relativa do estágio.
			int stage = -1;
			double stage_factor = -1.0, stage_reserve_kt = -1.0;
			double stage_mined_kt = -1.0, next_deepen_cost = -1.0;
		};
		std::vector<BuildingCache> building_cache;
		Array imported, exported, import_value, export_value;
		// Cópias dos últimos valores gravados em `markets` e nos quatro fluxos
		// de comércio. A cidade típica movimenta 2–5 dos 21 produtos: escrever
		// os outros 16 custava ~100 mil hashes de String por dia (650 cidades).
		// Comparar double é ~20× mais barato que escrever em Dictionary.
		std::vector<Market> market_written;
		// F5: preço local publicado (`price_anchor ÷ taxa`), por produto. A
		// comparação evita reescrever o Dictionary quando nada mudou; com
		// câmbio float a taxa muda e TODOS os locais são republicados no dia.
		std::vector<double> market_local_written;
		std::vector<char> market_written_filled;  // 1 por produto
		std::vector<double> io_written;           // 4 × kProducts
		std::vector<char> io_filled;              // 4 flags (um por array)
		bool static_filled = false;
		int static_version = -1;
	};

	// Chaves do status pré-construídas. `dict["price"]` converte UTF-8 ->
	// UTF-32 e aloca uma String a cada escrita; com as chaves prontas a escrita
	// fica só com o hash e a atribuição. São `StringName` (hash calculado uma
	// vez, comparação por ponteiro): cada cidade reescreve ~45 chaves por dia e
	// o `String` recalculava o hash de todas elas (medido no cenário real de
	// 858 cidades). O GDScript continua lendo por `String` — o hash é o mesmo e
	// o Dictionary compara `String`/`StringName` pela sequência de caracteres.
	struct StatusKeys {
		StringName age_bands, amount, bankruptcies, buildings, calendar_day, capacity, capital_cost_per_capacity, capital_income, center, cities, city_count, construction_cash, construction_revenue, consumed, consumed_by_category, consumed_today, consumed_total, costs, day, deficit, demand, dependents, depleted, depleted_mines, deposits, depth_m, desired, desired_by_category, disposable, disposable_per_capita, education, education_share, efficiency, employed, employed_by_level, export_value, exported, external_demand, external_income, external_income_index, extracted_kt, firm_cash, food_spend, foreign_cash, freight_cost, freight_revenue, from_city, from_nation, gdp, gdp_per_capita, good, government_budget, home_nation, id, import_value, imported, in_transit, income, income_level, income_level_by_stratum, income_level_share, income_strata, stratum_disposable, stratum_population, stratum_wage_income, industrialization, initial_kt, input_product, input_value, invariant_violations, jobs, kind, labor, labor_force, labor_force_by_level, landmass, legs, living_standard, loss_days, market_totals, markets, migration, minimum_wage, mode, money_total, name, nation, nations, need, opex, opex_factor, opex_revenue, output_product, output_scale, owner, pop, population, ports, price, price_local, produced_today, produced_total, productivity_index, product, production, products, profit, quality, recruit_reserve, remaining_kt, resource, retirees, revenue, satisfaction, savings, scope, service_cash, service_jobs, services, service_sectors, service_businesses, service_bankruptcies, service_exports, service_profit, service_revenue, service_spend, service_wage, service_workers, service_jobs_filled, sector_workers, sector_share, in_kind_food, shortage, simulation_population, site_suitability, sold, stock, stockpile, students, studying, suitability, supply, surplus, tag, taxes, to_city, to_nation, trade_orders, trade_orders_count, transfers, transit_hours, transport_cash, unemployed, unemployed_by_level, unemployment_by_level, utilization, vacancies_by_level, wage, wage_by_level, wage_income, wage_income_by_level, wage_rigidity, workers, working_age, x, y, host_city, capacity_t_per_day, delay_hours, level, queue, throughput, recipe, inputs, byproducts, qty, capital_per_capacity, last_utilization, strategic, yield, from, to, from_px, to_px, distance_km, load_tons, trips, speed_kmh, batch_capacity_tons, dispatched, cost, needs_level, needs_satisfaction, basic_satisfaction, life_satisfaction, luxury_satisfaction, needs_spend, durable_spend, durable_stock, government_goods_spend, trucks, ships, planes, fleet, fuel, spare_parts, health, fuel_used, missed_no_fleet, missed_no_fuel, source, target_trucks, target_ships, target_planes, currency, currencies, code, rate, regime, trade_balance, fallback, tariff, tariffs, seller, buyer, origin_price, origin_price_anchor, delivered_price, fx_rate, seller_currency, buyer_currency, seller_fx_rate, tariff_revenue, owner_kind, owner_group, owner_nation, local_share, groups, stage, stage_factor, stage_reserve_kt, stage_mined_kt, next_deepen_cost;
		StringName light, product_count, m_price, m_stock, m_supply, m_surplus,
			m_sold, m_shortage, m_desired, p_need, p_consumed, p_satisfaction,
			p_labor_force, p_unemployed, i_city, i_resource, i_owner_group,
		b_offset, b_id, b_output_product, b_owner, b_capacity, b_utilization,
		b_costs, b_site_suitability, d_offset, d_resource, d_product,
		d_initial_kt, d_remaining_kt, d_quality, d_depth_m, d_efficiency,
		d_opex_factor, d_extracted_kt, d_depleted,
		c_offset, c_output_product, c_capacity, c_expand,
		// B6: Gini de riqueza (poupança + imóvel + terra + participações) da
		// cidade e da nação.
		wealth_gini;
		StatusKeys();
	};

	// Slot de status por nação: reaproveita o dicionário da nação, os
	// sub-dicionários `fleet`/`labor` e os Arrays de níveis (educação, força de
	// trabalho, salários, fatias de renda). Antes o bloco `nations` alocava um
	// Dictionary + ~10 Arrays novos por nação a cada tick (~2 mil alocações/dia
	// em 179 nações). Os contêineres são reutilizados e os valores reescritos em
	// lugar — byte-idêntico ao código anterior.
	struct StatusNationSlot {
		Dictionary dict;
		Dictionary fleet;
		Dictionary labor;
		Array education;
		Array labor_force_by_level;
		Array employed_by_level;
		Array unemployed_by_level;
		Array vacancies_by_level;
		Array unemployment_by_level;
		Array wage_by_level;
		Array income_share;
		Array fleet_health;
		bool fleet_built = false;
		bool labor_built = false;
	};
	struct StatusCache {
		Dictionary root;
		Array cities;
		Array ports;
		Array orders;
		Array products;
		Array violations;
		Array nations;
		Array currencies;
		Array tariffs;
		Array service_sectors;
		bool service_sectors_filled = false;
		Array tot_produced, tot_consumed, tot_produced_today, tot_consumed_today;
		// Totais do sistema por produto (soma dos mercados das cidades). A IA
		// lia 858 cidades × 21 mercados em GDScript no frame da virada (~13 ms);
		// o núcleo publica aqui a mesma soma, calculada na thread do tick.
		Dictionary market_totals;
		PackedFloat64Array tot_surplus, tot_sold, tot_demand;
		std::vector<StatusCitySlot> city_slots;
		std::vector<Dictionary> port_slots;
		std::vector<Dictionary> order_slots;
		std::vector<Array> order_leg_slots;
		// Dicionários de perna reaproveitados (1 por perna de pedido): antes
		// cada pedido alocava um Dictionary novo por perna a cada dia.
		std::vector<std::vector<Dictionary>> order_leg_entry;
		std::vector<StatusNationSlot> nation_slots;
		// Caminho total em trânsito por cidade (evita varredura O(cidades ×
		// cargas) dentro do laço de cidades).
		std::vector<double> in_transit_by_city;
		bool header_filled = false;
		// S17: cadência do agregado nacional `labor` (educação/força/emprego/
		// fatias de renda). É dado de exibição (telas de política), não decisão
		// da IA — recomputado uma vez por semana (dia % 7 == 0), cacheado nos
		// outros dias. A primeira publicação sempre computa.
		bool labor_cadence_built = false;
		// A4: dia a que se referem os valores de `labor` guardados neste slot
		// (permite semear um slot que ficou parado várias semanas).
		int labor_values_day = -1;
		// S7-c: sinais leves por cidade (os campos que a IA varre em TODAS as
		// cidades todo dia), em arrays planos indexados por cidade — um
		// PackedFloat64Array por campo em vez de ~113 dicionários de mercado +
		// POP por cidade (o custo de ponte que dominava status_cities). O bloco
		// pesado (markets/pop/buildings/deposits) sai do status diário na
		// próxima fatia e fica sob demanda em get_city_status(id).
		Dictionary light;
		bool light_filled = false;
		PackedFloat64Array light_m_price, light_m_stock, light_m_supply,
			light_m_surplus, light_m_sold, light_m_shortage, light_m_desired;
		PackedFloat64Array light_p_need, light_p_consumed, light_p_satisfaction,
			light_p_labor_force, light_p_unemployed;
		PackedInt32Array light_i_city;
		PackedStringArray light_i_resource, light_i_owner_group;
		// S7-c fatia 3: tabelas planas CSR de construções e jazidas — o que a
		// IA por cidade lê (produto de saída, dono, capacidade, utilização,
		// custos, aptidão do sítio; e recurso/reserva/fator de profundidade da
		// jazida). Mesmos valores do bloco pesado, na mesma ordem
		// (cidade → construção; cidade → produto rastreado). `b_offset`/`d_offset`
		// são os offsets por cidade (tamanho cidades+1, padrão CSR).
		PackedInt32Array light_b_offset, light_d_offset;
		PackedInt32Array light_b_output_product;
		PackedStringArray light_b_id, light_b_owner;
		PackedFloat64Array light_b_capacity, light_b_utilization, light_b_costs,
			light_b_site_suitability;
		PackedInt32Array light_d_product, light_d_depleted;
		PackedStringArray light_d_resource;
		PackedFloat64Array light_d_initial_kt, light_d_remaining_kt,
			light_d_quality, light_d_depth_m, light_d_efficiency,
			light_d_opex_factor, light_d_extracted_kt;
		// §15: tabela plana CSR de OBRAS em andamento — o que a IA lê para não
		// reconstruir a mesma lacuna enquanto uma obra está em voo (direção de
		// calibração do D-060). `c_offset` por cidade (cidades+1, padrão CSR);
		// `c_capacity` é a capacidade futura (obra nova) ou adicionada
		// (expansão); `c_expand` = 1 quando é expansão de prédio existente, 0
		// quando é obra nova (o produto de saída ainda não existe na cidade).
		PackedInt32Array light_c_offset;
		PackedInt32Array light_c_output_product, light_c_expand;
		PackedFloat64Array light_c_capacity;
	};

	static const ModeSpec& spec(TransportMode mode) { return ogs::SimCore::spec(mode); }
	static const char* scope_name(MarketScope scope) {
		return ogs::SimCore::scope_name(scope);
	}

	/** Preenche um Dictionary de perna para o status, reutilizando as chaves. */
	void fill_leg_status(Dictionary& dict, const Leg& leg) const;

	// S5b: os helpers do núcleo migraram para ogs::SimCore; estes wrappers
	// preservam as chamadas da fachada sem renomear nenhuma referência.
	City* find_city(const std::string& id) { return sim_core_.find_city(id); }
	const City* find_city(const std::string& id) const { return sim_core_.find_city(id); }
	/**
	 * Refaz os índices derivados do mundo (id da cidade -> posição e
	 * construções por cidade). Chamado ao inicializar, ao carregar um save e no
	 * início de cada tick: sem eles, as fases varrem TODAS as construções para
	 * cada cidade (O(cidades × construções), que não escala para 650 cidades).
	 */
	void rebuild_indexes() { sim_core_.rebuild_indexes(); }
	/**
	 * Preenche o slot de status de uma cidade (reaproveitando dicionários).
	 * @param city_index Posição em ws_.cities.
	 * @param slot Cache da cidade (mesma posição em status_cache_).
	 * @param in_transit Carga em trânsito até a cidade (já somada em passada única).
	 * @param heavy Publica o detalhe pesado (markets/pop/services/buildings/
	 *   deposits). O status diário passa `false` (S7-c): esses blocos ficam sob
	 *   demanda em `get_city_status(id)`, que passa `true`. Os escalares leves
	 *   (população, PIB, indicadores, emprego setorial, fluxos de comércio) e o
	 *   bloco `light` são publicados sempre.
	 */
	void fill_city_status(std::size_t city_index, StatusCitySlot& slot,
		double in_transit, bool heavy) const;
	/**
	 * Preenche os sinais leves por cidade (S7-c): mercados (7 campos), POP
	 * (5 escalares) e incumbentes por grupo, em arrays planos indexados por
	 * cidade. São os MESMOS doubles do bloco pesado por cidade (bit a bit),
	 * para a varredura diária da IA não precisar de `markets`/`pop`/`buildings`
	 * completos.
	 */
	void fill_light_signals(StatusCache& cache) const;

	// A4: escalares por cidade (os MESMOS do bloco leve do status), num POD
	// para a ficha/colunas não duplicarem a conta. A ordem dos campos é a
	// ordem de `sync_scalar` em `fill_city_status` — o status continua
	// byte-idêntico.
	struct CityScalars {
		double population = 0.0, simulation_population = 0.0, gdp = 0.0,
			gdp_per_capita = 0.0, external_income_index = 0.0, firm_cash = 0.0,
			service_cash = 0.0, government_budget = 0.0,
			government_goods_spend = 0.0, service_revenue = 0.0,
			service_jobs = 0.0, service_workers = 0.0,
			service_jobs_filled = 0.0, in_kind_food = 0.0,
			service_profit = 0.0, opex_revenue = 0.0, freight_revenue = 0.0,
			construction_revenue = 0.0, service_wage = 0.0,
			disposable_per_capita = 0.0, living_standard = 0.0,
			production = 0.0, jobs = 0.0, income = 0.0, industrialization = 0.0,
			stockpile = 0.0, need = 0.0, deficit = 0.0, consumed = 0.0,
			in_transit = 0.0, fx_rate = 0.0, tariff_revenue = 0.0,
			productivity_index = 0.0;
	};
	CityScalars city_scalars(std::size_t city_index, double in_transit) const;

	// A4: soma das cargas em trânsito por cidade (uma passada só).
	void compute_in_transit(std::vector<double>& out) const;

	// A4: pedaços do `get_status()` extraídos para as consultas reusarem o
	// MESMO código (mesmos valores e mesma ordem de chaves).
	void build_ports(StatusCache& cache);
	void build_orders(StatusCache& cache);
	void build_nations(StatusCache& cache,
		const std::vector<double>& in_transit_by_city);
	void build_currencies(StatusCache& cache);
	void build_tariffs(StatusCache& cache);

	/**
	 * A4: retrato SEMANAL do agregado `nations[].labor` (S17), capturado no
	 * fecho do dia `day % 7 == 0` como doubles puros. O status sob demanda lê
	 * o retrato mais recente, então um `get_status()` no dia 60 publica os
	 * mesmos valores que a publicação diária antiga (dia 56) — o golden não
	 * muda. Antes do 1º tick o retrato nasce do estado corrente (como o
	 * `warm_status()` fazia no dia 0).
	 */
	struct NationLaborSnapshot {
		std::string tag;
		double working_age = 0.0, labor_force = 0.0, employed = 0.0,
			unemployed = 0.0, students = 0.0, recruit_reserve = 0.0,
			income_population = 0.0, productivity_weighted = 0.0,
			minimum_wage = 0.0, wage_rigidity = 0.0;
		double education[kEducationLevels] = {};
		double labor_force_by_level[kEducationLevels] = {};
		double employed_by_level[kEducationLevels] = {};
		double unemployed_by_level[kEducationLevels] = {};
		double vacancies_by_level[kEducationLevels] = {};
		double wage_weighted[kEducationLevels] = {};
		double wage_weight[kEducationLevels] = {};
		double income_share[kIncomeLevels] = {};
	};
	void capture_nation_labor();
	const NationLaborSnapshot* find_labor_snapshot(const std::string& tag) const;
	int labor_snapshot_day_ = -1;
	std::vector<NationLaborSnapshot> labor_snapshot_;
	// A4: no caminho `tick_day()` (tools/testes/goldens) o primeiro build de um
	// slot calcula o trabalho do dia corrente, como sempre; no caminho do jogo
	// (sem status diário) ele é semeado pelo retrato semanal.
	bool status_seed_labor_from_snapshot_ = true;

	// A4: cache das linhas/colunas sob demanda. Não participa do caminho
	// diário; o resumo do dia é sempre construído do zero (imutável).
	Dictionary columns_cache_;
	int columns_day_ = -1;
	int columns_static_version_ = -1;
	std::vector<StatusCitySlot> query_city_slots_;
	Array query_city_rows_;
	int query_rows_day_ = -1;
	int query_rows_static_version_ = -1;
	Dictionary query_light_cache_;
	int query_light_day_ = -1;
	Array query_orders_cache_;
	int query_orders_day_ = -1;
	double money_total() const { return sim_core_.money_total(); }
	// Deriva a frota inicial de população/PIB (source: derived): veículos,
	// estoque de combustível e de peças. Chamado ao inicializar e ao carregar
	// um save sem o bloco `fleets`.
	void init_fleets() { sim_core_.init_fleets(); }
	Port* find_port(const std::string& id) {
		return sim_core_.find_port(id);
	}
	/**
	 * Índice da cidade da nação/massa mais próxima do ponto (-1 se nenhuma).
	 * Usado como cidade-sede/cobradora quando `port_of` não aponta para uma
	 * cidade existente (cenário sintético, save antigo).
	 */
	int city_index_near(const std::string& nation, const std::string& landmass,
	                         const MapPoint& from) const {
		return sim_core_.city_index_near(nation, landmass, from);
	}
	/**
	 * Capacidade inicial de cada porto (nível 1) pela população de simulação da
	 * cidade-sede (`port_of`); sem sede conhecida, usa a cidade mais próxima da
	 * mesma nação/massa e, por fim, o piso de referência. Chamado no fim de
	 * initialize_world e de from_dict (save sem portos).
	 */
	void init_port_capacities() { sim_core_.init_port_capacities(); }
	int product_index(const String& product) const;
	// Slot da cesta alimentar (0..ws_.food_products.size()-1) -> id de produto.
	int food_product(int slot) const { return sim_core_.food_product(slot); }
	// Id de produto -> fator calórico (0.0 se não for alimento da cesta).
	double food_calories(int product) const { return sim_core_.food_calories(product); }
	const char* product_name(int product) const;
	int recipe_index(const String& recipe_id) const;
	static double extraction_efficiency(const Deposit& deposit) {
		return ogs::SimCore::extraction_efficiency(deposit);
	}
	static double depth_opex_factor(const Deposit& deposit) {
		return ogs::SimCore::depth_opex_factor(deposit);
	}
	double base_price(int product) const { return sim_core_.base_price(product); }
	// F5: câmbio, preço entregue e tarifa.
	const Currency* currency_for(const std::string& nation) const;
	Currency* currency_for(const std::string& nation);
	double fx_rate(const std::string& nation) const {
		return sim_core_.fx_rate(nation);
	}
	/**
	 * F5: fator de competitividade da âncora (`1` com câmbio fixo; `taxa ÷
	 * taxa-base` no regime float, dentro da banda ±10%). Aplicado ao valor de
	 * oferta/importação de uma nação float — é o efeito de preço rígido na
	 * moeda local, sem alterar o preço interno (âncora).
	 */
	double competitiveness(const std::string& nation) const;
	const char* currency_code(const std::string& nation) const {
		return sim_core_.currency_code(nation);
	}
	// Tarifa ad valorem efetiva (coringas "*").
	double tariff_rate(const std::string& buyer_nation,
		const std::string& seller_nation, int product) const;
	// Refaz os índices code->moeda e nação->moeda depois de carregar a lista
	// (set_currencies, from_dict).
	void rebuild_currency_indexes() {
		sim_core_.rebuild_currency_indexes();
	}
	// Refaz a tabela `buyer|seller|product` -> taxa (set_tariff,
	// from_dict).
	void rebuild_tariff_lookup();
	/**
	 * Define os bens e preços-base a partir de dados externos (goods.json).
	 * Cada item deve ter "id" (wheat/iron/...), "base_price" e, opcionalmente,
	 * "order" (0..kMaxProducts-1). A ordem 0..20 dos primários é preservada
	 * e os bens novos entram nas posições seguintes. Pode ser chamado antes ou
	 * depois da inicialização: o estado existente é preservado por índice.
	 */
	void set_goods(const Array& goods);
	/**
	 * Define as receitas industriais (recipes.json): produto, yield, insumos com
	 * quantidade, subprodutos e engenharia (jobs/wage/opex/capital por t/dia).
	 */
	void set_recipes(const Array& recipes);
	/**
	 * Define a cesta alimentar a partir de needs.json: usa
	 * `levels.basic.categories.food.substitutes` e `equivalence.calories`.
	 * Mantém a cesta de 5 culturas quando o dicionário não traz o dado.
	 */
	void set_needs(const Dictionary& needs);
	// F3: zera/redimensiona o estado de necessidades de uma POP (duráveis por
	// produto, vetores dos 3 níveis) sem apagar estoque já acumulado.
	void init_pop_needs(Pop& pop) const { sim_core_.init_pop_needs(pop); }
	/**
	 * F7: deriva e (re)inicializa o perfil de trabalho/educação de uma cidade a
	 * partir do índice de renda externa (PIB PPP per capita já normalizado).
	 * É a única fonte de dado por país disponível no cenário; a tabela de
	 * faixas é uma ESTIMATIVA declarada (ordem de grandeza ILO/World Bank,
	 * Barro-Lee/UNDP) e está documentada em economy_sim.cpp. Não mexe em
	 * população nem em caixa.
	 */
	void apply_labor_profile(City& city);
	/**
	 * F7 calibração: recalcula a população de cada estrato de renda
	 * (escolaridade/ocupação) a partir da distribuição de educação. Inclui os
	 * dependentes: a escolaridade é medida na idade ativa, mas a renda do
	 * estrato sustenta toda a sua população.
	 */
	void refresh_strata(Pop& pop) const { sim_core_.refresh_strata(pop); }
	/**
	 * F7: escolaridade das vagas por setor industrial (fallback quando a receita
	 * não declara `education_mix`). Tabela documentada; escreve 3 frações.
	 */
	static void education_mix_for_sector(const std::string& sector,
		double* mix) {
		ogs::SimCore::education_mix_for_sector(sector, mix);
	}

	// F8: índices cidade→negócios e setor→negócios (como os de construção).
	void rebuild_service_indexes() { sim_core_.rebuild_service_indexes(); }
	// F8: semeia os negócios iniciais de cada cidade (capital do caixa do
	// próprio setor + poupança das famílias acima da reserva de comida).
	// Roda no boot do mundo e ao carregar save anterior ao v10.
	void seed_services() { sim_core_.seed_services(); }
	// F8: abre um negócio do setor com a capacidade pedida. O capital vem do
	// caixa do próprio setor (service_cash) e, se faltar, da poupança das
	// famílias ACIMA da reserva de comida (a parcela de poupança move
	// savings→service_cash e é devolvida no fechamento — conservação).
	bool service_open(City& city, int sector, double capacity,
		double capital) {
		return sim_core_.service_open(city, sector, capacity, capital);
	}
	// F8: fecha um negócio insolvente, devolvendo à POP só o capital que ela
	// aportou (a parcela financiada pelo setor fica no setor).
	void service_close(ServiceBusiness& business) {
		sim_core_.service_close(business);
	}
	int service_sector_index(const std::string& sector) const {
		return sim_core_.service_sector_index(sector);
	}

	/**
	 * F7 comida — custo EFETIVO da necessidade alimentar do dia: os preços dos
	 * alimentos que a cidade REALMENTE tem em estoque (na mesma proporção de
	 * disponibilidade usada na compra) mais o resíduo importado ao CIF
	 * conhecido (`import_price`) ou, sem ele, ao preço local. O preço do
	 * alimento mais barato do mundo (pão processado sem estoque local)
	 * subestimava o subsídio de subsistência das POPs.
	 *
	 * `local_cost` = custo da parcela coberta pelo estoque local;
	 * `import_cost` = custo do que falta e terá de ser importado;
	 * `per_ge` = (local_cost + import_cost) / need, ou o preço mais barato por
	 * caloria quando não há necessidade. Sem alocação: uma passada por produto.
	 */
	void food_basket_cost(const City& city, double need,
		double& local_cost, double& import_cost, double& per_ge) const {
		sim_core_.food_basket_cost(city, need, local_cost, import_cost,
			per_ge);
	}
	/**
	 * F7: atualiza os salários por nível depois do lucro do dia (mercado
	 * apertado/folgado, margem das firmas, âncora de renda do país, piso de
	 * salário mínimo e rigidez). Chamado na fase de contabilidade.
	 */
	void update_wages(City& city, double building_margin) {
		sim_core_.update_wages(city, building_margin);
	}
	/**
	 * F7 calibração: TFP efetiva da cidade — o fator explícito do laboratório
	 * quando declarado (> 0) ou o FATOR COMPLETO derivado do PIB PPP per capita
	 * real (`clamp(external_income_index, 0,25, 4,0)`, `derived:gdp_ppp`,
	 * normalizado para a média mundial = 1,0). É o nível de produtividade do
	 * país, não um prêmio: países abaixo da média recebem fator < 1. O piso de
	 * subsistência da âncora salarial (update_wages) impede que o fator
	 * < 1 derrube o trabalhador abaixo da cesta alimentar.
	 * EUA ~3,5, DEU ~2,9, BRA ~0,9, IND ~0,46, NGA ~0,37.
	 */
	double effective_productivity(const City& city) const {
		return sim_core_.effective_productivity(city);
	}
	/**
	 * F7 calibração — declara o Gini de renda da nação (0..1). 0 desliga a
	 * divisão por classes e mantém os estratos de escolaridade.
	 */
	void set_income_gini(const String& city_id, double gini);
	/**
	 * B2 — declara os cinco quintis de RENDA observados da nação (frações da
	 * renda total, soma ~1) e semeia os estratos da cidade. Vazio = re-semeia
	 * com a proxy lognormal do Gini declarado. API de inicialização (antes do
	 * dia 1), como set_urban.
	 */
	void set_city_strata(const String& city_id,
		const PackedFloat64Array& shares);
	/**
	 * B3 — declara o bloco de REGIÃO de uma cidade (estados inteiros), já
	 * resolvido pelo GDScript a partir de state_regions.json: `population_region`,
	 * `urban_population`, `region_population`, `urban_share` (0..1 ou -1), `name`
	 * e `states` ([{id, name, population, area_km2}]). Semeia demografia/quintis
	 * do interior e reconcilia a população exibida. API de inicialização (antes
	 * do dia 1), como set_city_strata.
	 */
	void set_city_region(const String& city_id, const Dictionary& data);
	/**
	 * B3 — ficha da região de uma cidade (nome, estados, população, urbanização,
	 * emprego, renda, comida, satisfação). `{}` quando a cidade não existe ou não
	 * tem dado de região.
	 */
	Dictionary get_region_status(const String& city_id) const;
	/**
	 * F7 calibração — recalcula as classes de consumo (população e teto) a
	 * partir do Gini declarado e do custo da cesta básica do dia. Sem Gini,
	 * espelha os estratos de escolaridade (`stratum_*`).
	 */
	void refresh_income_classes(City& city) const {
		sim_core_.refresh_income_classes(city);
	}
	/**
	 * Bootstrap industrial (§12.3): no cenário real, semeia uma planta
	 * estratégica de referência (moinho + padaria) em poucas capitais quando o
	 * mundo ainda não tem nenhuma construção secundária. Nota: é uma
	 * ESTIMATIVA de transição, não dado observado; run once, marcada no status.
	 */
	void seed_industry();
	Building* find_building(const std::string& city_id, const std::string& id);

	// F9: propriedade e grupos.
	int group_index(const std::string& id) const {
		return sim_core_.group_index(id);
	}
	Group* find_group(const std::string& id) {
		return sim_core_.find_group(id);
	}
	const Group* find_group(const std::string& id) const {
		return sim_core_.find_group(id);
	}
	// Decompõe o dono de uma construção: kind 0 state, 1 national, 2 foreign,
	// 3 private (legado). Preenche grupo e nação de origem.
	int owner_kind(const std::string& owner, std::string* group_id,
		std::string* origin_nation) const {
		return sim_core_.owner_kind(owner, group_id, origin_nation);
	}
	int fdi_mode(const std::string& nation, const std::string& sector) const {
		return sim_core_.fdi_mode(nation, sector);
	}
	double sector_tax_holiday(const std::string& nation,
		const std::string& sector) const {
		return sim_core_.sector_tax_holiday(nation, sector);
	}
	double sector_subsidy(const std::string& nation,
		const std::string& sector) const {
		return sim_core_.sector_subsidy(nation, sector);
	}
	double nation_confidence(const std::string& nation) const {
		return sim_core_.nation_confidence(nation);
	}
	// Credita `amount` no capital das famílias da nação, ponderado pelo estrato
	// dono (topo), e devolve o total efetivamente creditado.
	double credit_nation_households(const std::string& nation,
		double amount) {
		return sim_core_.credit_nation_households(nation, amount);
	}
	// Imposto corporativo + retenção: move o lucro do grupo da cidade para o
	// caixa dele (ou para o orçamento, se estatal). Chamado na contabilidade.
	void route_group_profit(int group_index, const std::string& host_nation,
		double profit, double revenue, City& city, double& firm_surplus) {
		sim_core_.route_group_profit(group_index, host_nation, profit,
			revenue, city, firm_surplus);
	}

	// Medição por fase do tick (bench canônico; desligada no jogo). A
	// infraestrutura vive em sim/profiling.h (OGS_PROFILING, só no build debug).
	bool profile_ = false;
	double profile_us_[kPhaseCount] = {};
	// Subdivisão da fase `status` (medida por fase): cidades, pedidos, nações, resto.
	double status_sub_us_[4] = {};

	// Estado da simulação (cidades, construções, mercados, grupos, frota,
	// moedas, ...). S5a-2: movido para ogs::WorldState (sim/world_state.h);
	// a fachada guarda um único `ws_` e converte Godot <-> sim.
	ogs::WorldState ws_;

	// Núcleo da simulação (S5b): executa a ordem do dia sobre `ws_`. A fachada
	// converte Godot <-> sim e loga os invariantes; o núcleo não emite erro Godot.
	ogs::SimCore sim_core_{ws_};

	// §15: modo de construção instantânea (hook de teste). false = obra vira
	// projeto físico (padrão); true = comportamento antigo (prédio na hora).
	bool instant_construction_ = false;

	// R1/Q-014: as plantas de REFERÊNCIA do bootstrap (seed_industry) são
	// estoque industrial pré-existente do cenário, não obra nova — nascem
	// concluídas (custo debitado na criação, sem ConstructionProject). A
	// obra física continua valendo para investimento/expansão da IA e do jogo.
	// Ligado só dentro de seed_industry.
	bool bootstrap_construction_ = false;

	// A5: a fase da IA C++ (estado entre dias NÃO serializado) e o interruptor.
	// A4: `tick_day_status_` saiu — o tick só publica o resumo leve e o status
	// completo é montado sob demanda (não há status do dia guardado aqui).
	ogs::AiPhase ai_phase_;
	bool ai_enabled_ = false;
	// C4 (D-110): definições de `banks.json` (nomes, estrangeiro e origem).
	// Guardadas para reaplicar DEPOIS de um load (o save v31 migrado não tem
	// nomes e o v32 pode vir de um mundo com outro dado).
	std::vector<ogs::bank::NameData> bank_defs_;

	// Status reaproveitado entre dias (ver StatusCache). Dois slots
	// alternados: o worker do tick escreve no slot inativo enquanto a UI/IA
	// continuam lendo o último status COMPLETO (double buffer).
	StatusCache status_slots_[2];
	int status_live_ = -1;
	int status_built_day_ = -1;
	// Chaves prontas do status (evitam converter const char* -> String a cada
	// escrita; ver StatusKeys).
	StatusKeys status_keys_;
	// Sobe quando um campo estático das cidades muda (aptidão, demanda externa,
	// depósitos revelados): força a reescrita desses contêineres no status.
	int status_static_version_ = 0;
	// Sobe quando uma jazida nova é revelada (set_deposit/save). Separado
	// da versão estática para uma descoberta não forçar a reescrita de
	// aptidão/demanda externa de todas as cidades (~190 mil escritas/dia).
	int status_deposit_version_ = 0;
	// A4: contador monotônico de dados publicados (dia novo ou mutação que
	// invalidou o status). A UI compara para saber se o retrato em cache velho.
	int data_version_ = 0;
	// Serializa o acesso ao estado demo entre a thread do tick e a main thread
	// (UI, IA, ferramentas). O `sim` tem UM escritor por convenção
	// (tick_scheduler); o mutex é a rede de segurança para quem chamar
	// tick_day/get_status/investir enquanto um dia roda na thread — sem
	// ele a disputa vira corrupção de estado e crash (signal 11).
	// Recursivo: tick_day() chama tick_core() e get_status().
	mutable std::recursive_mutex mutex_;
};

} // namespace godot
