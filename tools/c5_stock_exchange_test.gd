extends SceneTree

## C5 (D-112) — BOLSA DE VALORES POR PAÍS, CONFIANÇA E EXPECTATIVAS.
##
## Cenário: o fixture AGRIA/PORTUS (nação DEMO, moeda DEM) para a mecânica, com
## o banco estrangeiro sintético para a carteira do exterior. Nada aqui inventa
## número: preço = patrimônio/lucro MEDIDO do grupo e sinais MEDIDOS da nação.
##
##   C1  listagem pelo critério declarado (não estatal, com receita medida),
##       nome fictício (`<ISO3> Exchange`) e identidade carteiras = emissores.
##   C2  preço converge ao fundamental × confiança (passo semanal; com passo 1
##       o fechamento é exatamente o alvo) e não salta no passo default.
##   C3  juro alto derruba preço e confiança; a expectativa é ADAPTATIVA (não
##       pula no primeiro dia do choque).
##   C4  dividendo mensal fecha no balanço (dinheiro conservado na identidade).
##   C5  valorização NÃO cria dinheiro (`money_total` idêntico com o crédito
##       desligado; o market cap muda).
##   C6  migração v32 -> v33: a bolsa nasce com preço = fundamental medido e a
##       carteira do default declarado.
##   C7  save v33: round-trip bit-exato e continuação de 5 dias idêntica.
##   C8  aquecimento (D-112 fix): índice não publicado e preço sem reação antes
##       de kIndexWarmupDays; formação no fundamental medido; papel de lucro
##       estável não oscila > 20 % fora do aquecimento.
##   C9  banda de P/L declarada (3..30) não trava a valoração no juro extremo.
##
## Roda pelo runner: powershell -File tools/run_tests.ps1 -List c5_stock_exchange_test

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"

const EPS := 1e-6
# Marcas reais que NÃO podem aparecer: o nome é fictício/genérico por política.
const REAL_BRANDS: Array[String] = ["NYSE", "NASDAQ", "LSE", "B3", "BM&F",
	"Euronext", "Deutsche Boerse", "JPX", "SSE", "HKEX", "Bovespa", "S&P"]

var _failures: Array[String] = []


func _init() -> void:
	_test_listing_and_identity()
	_test_price_converges()
	_test_rate_shock()
	_test_dividends()
	_test_valuation_no_money()
	_test_migration_v32()
	_test_save_v33()
	_test_warmup()
	_test_pe_band()
	if _failures.is_empty():
		print("[c5-stock] OK: C1-C9 — listagem por critério medido, identidade "
			+ "carteiras = emissores, preço convergente ao fundamental, juro "
			+ "alto derrubando preço/confiança (expectativa adaptativa), "
			+ "dividendo conservado, valorização sem dinheiro novo, migração "
			+ "v32->v33, save v33 bit-exato, aquecimento com lucro de "
			+ "referência medido e banda de P/L declarada.")
		quit(0)
	else:
		print("[c5-stock] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


# --- C1 -------------------------------------------------------------------
func _test_listing_and_identity() -> void:
	var sim = _world(8)
	var status: Dictionary = sim.get_stock_status()
	var totals: Dictionary = status.get("totals", {})
	_check("C1 status publica papéis", int(totals.get("listed", 0)) > 0,
		"listed=%s" % totals.get("listed"))
	_check("C1 identidade carteiras = market cap",
		absf(float(totals.get("equity_identity", 1.0))) <= 1e-6,
		"gap=%s" % totals.get("equity_identity"))
	var groups_by_id: Dictionary = {}
	for group: Dictionary in sim.get_groups():
		groups_by_id[String(group.get("id", ""))] = group
	var bad_listing := 0
	var bad_holdings := 0
	var bad_names := 0
	var tickers: Array = status.get("tickers", [])
	for row: Dictionary in tickers:
		var group_id := String(row.get("group_id", ""))
		var group: Dictionary = groups_by_id.get(group_id, {})
		if bool(group.get("state_owned", false)) \
				or String(group.get("origin_nation", "")).is_empty() \
				or float(group.get("revenue_total", 0.0)) <= 0.0:
			if bool(row.get("listed", false)):
				bad_listing += 1
		var holdings: Dictionary = row.get("holdings", {})
		var held := float(holdings.get("households_high", 0.0)) \
			+ float(holdings.get("foreign", 0.0))
		for value: Variant in (holdings.get("banks", {}) as Dictionary).values():
			held += float(value)
		for value: Variant in (holdings.get("groups", {}) as Dictionary).values():
			held += float(value)
		if absf(held - float(row.get("shares", 0.0))) > 1e-6 \
				or float(row.get("price", 0.0)) < 0.0:
			bad_holdings += 1
	var exchanges: Array = status.get("exchanges", [])
	for exchange: Dictionary in exchanges:
		var name := String(exchange.get("name", ""))
		if not name.ends_with(" Exchange") \
				or String(exchange.get("name_status", "")) != "game_default":
			bad_names += 1
		for brand: String in REAL_BRANDS:
			if name.contains(brand):
				bad_names += 1
	_check("C1 só lista grupo não estatal com receita medida", bad_listing == 0,
		"fora_do_criterio=%d" % bad_listing)
	# D-113: porte mínimo — o papel listado abre no fundamental medido e nenhum
	# preço de formação fica abaixo do piso declarado (`min_listed_price`).
	var min_price := float((status.get("policy", {}) as Dictionary).get(
		"min_listed_price", -1.0))
	_check("C1 min_listed_price publicado no status", min_price > 0.0,
		"min_price=%s" % min_price)
	var below_min := 0
	for row: Dictionary in tickers:
		if not bool(row.get("listed", false)):
			continue
		if float(row.get("fundamental", 0.0)) < min_price - 1e-9:
			below_min += 1
			continue
		if String(row.get("reason", "")) != "listed":
			below_min += 1
	_check("C1 papel listado tem porte mínimo (preço de formação >= piso)",
		below_min == 0, "abaixo_do_piso=%d" % below_min)
	_check("C1 carteira de cada papel fecha com as ações emitidas",
		bad_holdings == 0, "papeis_quebrados=%d" % bad_holdings)
	_check("C1 nome fictício/genérico, sem marca real", bad_names == 0,
		"nomes_fora=%d" % bad_names)


# --- C2 -------------------------------------------------------------------
func _test_price_converges() -> void:
	# Passo 1: o fechamento semanal É o alvo (fundamental × confiança). A
	# convergência é medida DEPOIS do aquecimento (D-112 fix), quando o preço
	# de referência já é a média medida do período de formação.
	var sim = _world(63)
	sim.set_stock_exchange({"step": 1.0})
	for day in 7:
		sim.tick_day()
	var status: Dictionary = sim.get_stock_status()
	var compared := 0
	var worst := 0.0
	for row: Dictionary in status.get("tickers", []):
		if not bool(row.get("listed", false)):
			continue
		var target := float(row.get("fundamental", 0.0)) \
			* _confidence_of(status, String(row.get("exchange", "")))
		var price := float(row.get("price", 0.0))
		if target <= 1e-12:
			continue
		compared += 1
		worst = maxf(worst, absf(price - target) / target)
	_check("C2 preço converge ao fundamental × confiança (passo 1)",
		compared > 0 and worst <= 1e-6,
		"papeis=%d pior_erro=%s" % [compared, worst])
	# Passo default: no fechamento o movimento é no máximo 35 % da distância.
	var sim2 = _world(63)
	var before: Dictionary = _ticker_map(sim2)
	for day in 7:
		sim2.tick_day()
	var after_status: Dictionary = sim2.get_stock_status()
	var after: Dictionary = _ticker_map(sim2)
	var jumped := 0
	var checked := 0
	for group_id: Variant in after:
		var row: Dictionary = after[group_id]
		if not bool(row.get("listed", false)):
			continue
		var old_price := float((before.get(group_id, {}) as Dictionary) \
			.get("price", 0.0))
		var new_price := float(row.get("price", 0.0))
		var target := float(row.get("fundamental", 0.0)) \
			* _confidence_of(after_status, String(row.get("exchange", "")))
		checked += 1
		var bound := 0.35 * absf(target - old_price) + 1e-6
		if absf(new_price - old_price) > bound:
			jumped += 1
	_check("C2 passo default não salta (≤ 35 % da distância por semana)",
		checked > 0 and jumped == 0,
		"papeis=%d saltos=%d" % [checked, jumped])


# --- C3 -------------------------------------------------------------------
func _test_rate_shock() -> void:
	var base = _world(63)
	# Juro baixo (controlado: neutro − 2 p.p.) × juro alto (choque de 20 % a.a.).
	var low = _load_with_rate(base, 0.02)
	var high = _load_with_rate(base, 0.20)
	low.set_stock_exchange({"step": 1.0})
	high.set_stock_exchange({"step": 1.0})
	var conf_base := _confidence_of(base.get_stock_status(), "DEMO")
	# Expectativa adaptativa: um dia de choque não move a confiança ao alvo.
	low.tick_day()
	high.tick_day()
	var day1_low: Dictionary = low.get_stock_status()
	var day1_high: Dictionary = high.get_stock_status()
	var conf_low1 := _confidence_of(day1_low, "DEMO")
	var conf_high1 := _confidence_of(day1_high, "DEMO")
	var raw_low1 := float(_signals_of(day1_low, "DEMO").get("raw", 1.0))
	var raw_high1 := float(_signals_of(day1_high, "DEMO").get("raw", 1.0))
	_check("C3 confiança cai com o choque, mas devagar (expectativa)",
		raw_high1 < raw_low1 and conf_high1 < conf_low1
			and conf_base - conf_high1 < 0.05
			and absf(conf_high1 - conf_base) < absf(raw_high1 - conf_base),
		"base=%s baixo=%s alto=%s raw_baixo=%s raw_alto=%s"
			% [conf_base, conf_low1, conf_high1, raw_low1, raw_high1])
	for day in 6:
		low.tick_day()
		high.tick_day()
	var low_status: Dictionary = low.get_stock_status()
	var high_status: Dictionary = high.get_stock_status()
	var best: Dictionary = _best_paper(low_status)
	var group_id := String(best.get("group_id", ""))
	var low_price := float(best.get("price", 0.0))
	var high_price := float(_paper(high_status, group_id).get("price", 0.0))
	var low_conf := _confidence_of(low_status, "DEMO")
	var high_conf := _confidence_of(high_status, "DEMO")
	_check("C3 juro alto derruba o preço", not best.is_empty() \
		and high_price > 0.0 and high_price < low_price * 0.999,
		"papel=%s preco baixo=%s alto=%s" % [group_id, low_price, high_price])
	_check("C3 juro alto derruba a confiança", high_conf < low_conf,
		"conf baixa=%s alta=%s" % [low_conf, high_conf])


# --- C4 -------------------------------------------------------------------
func _test_dividends() -> void:
	var sim = _world(0)
	# O fechamento mensal é o dia 30: roda até lá e mede a identidade do dia.
	for day in 30:
		sim.tick_day()
	var status: Dictionary = sim.get_stock_status()
	var totals: Dictionary = status.get("totals", {})
	if float(totals.get("dividends_paid", 0.0)) <= 0.0:
		# Sem lucro medido no fixture o dividendo é 0 (ausência declarada): o
		# teste exige apenas que a identidade feche nesse caso.
		print("[c5-stock] nota: fixture sem lucro no mês, dividendo 0 (declarado)")
	var paid := float(totals.get("dividends_paid", 0.0))
	var paid_status := 0.0
	for exchange: Dictionary in status.get("exchanges", []):
		paid_status += float(exchange.get("dividends_paid", 0.0))
	_check("C4 total de dividendos = soma das praças", absf(paid - paid_status) <= EPS,
		"total=%s pracas=%s" % [paid, paid_status])
	var day_status: Dictionary = sim.get_status()
	var balance: Dictionary = day_status.get("balance", {})
	_check("C4 identidade financeira fecha no dia do dividendo",
		bool(balance.get("ok", false)),
		"financial_sum=%s" % balance.get("financial_sum"))
	_check("C4 nenhuma violação de invariante no mês",
		(day_status.get("invariant_violations", []) as Array).is_empty(),
		"violacoes=%s" % str(day_status.get("invariant_violations", [])))
	# O dividendo NÃO cria dinheiro: cada pagamento é limitado ao lucro do
	# período e ao caixa do emissor, e a soma dos papéis é a do emissor.
	for row: Dictionary in status.get("tickers", []):
		if float(row.get("dividend_last", 0.0)) < -1e-9 \
				or float(row.get("dividend_limited", 0.0)) < -1e-9:
			_check("C4 dividendo não negativo", false,
				"papel=%s" % row.get("group_id"))
			break


# --- C5 -------------------------------------------------------------------
func _test_valuation_no_money() -> void:
	var sim = _world(63)
	sim.set_bank_credit(false)
	sim.set_stock_exchange({"step": 1.0})
	sim.tick_day()
	var before: Dictionary = sim.get_status()
	var before_money := float(before.get("money_total", 0.0))
	var before_cap := float((before.get("stock", {}) as Dictionary) \
		.get("market_cap", 0.0))
	for day in 7:
		sim.tick_day()
	var after: Dictionary = sim.get_status()
	var after_money := float(after.get("money_total", 0.0))
	var after_cap := float((after.get("stock", {}) as Dictionary) \
		.get("market_cap", 0.0))
	_check("C5 valorização não cria dinheiro (money_total idêntico)",
		absf(after_money - before_money) <= EPS,
		"antes=%s depois=%s" % [before_money, after_money])
	_check("C5 a bolsa mudou de valor no período (o preço moveu)",
		absf(after_cap - before_cap) > 1e-6,
		"cap antes=%s depois=%s" % [before_cap, after_cap])
	var balance: Dictionary = after.get("balance", {})
	_check("C5 identidade da C1 continua fechando", bool(balance.get("ok", false)),
		"financial_sum=%s" % balance.get("financial_sum"))


# --- C6 -------------------------------------------------------------------
func _test_migration_v32() -> void:
	var sim = _world(30)
	var saved: Dictionary = sim.to_dict()
	var legacy: Dictionary = saved.duplicate(true)
	legacy["version"] = 32
	(legacy as Dictionary).erase("stock")
	var loaded = _load(legacy)
	var status: Dictionary = loaded.get_stock_status()
	var totals: Dictionary = status.get("totals", {})
	_check("C6 v32 carrega com a bolsa criada", int(totals.get("listed", 0)) > 0,
		"listed=%s" % totals.get("listed"))
	var bad_price := 0
	var bad_holdings := 0
	for row: Dictionary in status.get("tickers", []):
		var shares := float(row.get("shares", 0.0))
		var expected := (float(row.get("fundamental", 0.0))
			* _confidence_of(status, String(row.get("exchange", "")))) \
			if shares > 0.0 else 0.0
		if absf(float(row.get("price", 0.0)) - expected) > 1e-9:
			bad_price += 1
		var holdings: Dictionary = row.get("holdings", {})
		var held := float(holdings.get("households_high", 0.0)) \
			+ float(holdings.get("foreign", 0.0))
		for value: Variant in (holdings.get("banks", {}) as Dictionary).values():
			held += float(value)
		for value: Variant in (holdings.get("groups", {}) as Dictionary).values():
			held += float(value)
		if absf(held - shares) > 1e-6:
			bad_holdings += 1
	_check("C6 preço inicial = patrimônio medido por ação", bad_price == 0,
		"papeis_fora=%d" % bad_price)
	_check("C6 carteira inicial soma 100 % das ações", bad_holdings == 0,
		"papeis_fora=%d" % bad_holdings)
	var resaved: Dictionary = loaded.to_dict()
	_check("C6 re-save é v33 e tem o bloco da bolsa",
		int(resaved.get("version", 0)) == 33 and resaved.has("stock"),
		"version=%s" % resaved.get("version"))


# --- C7 -------------------------------------------------------------------
func _test_save_v33() -> void:
	var sim = _world(12)
	var saved: Dictionary = sim.to_dict()
	var loaded = _load(saved)
	_check("C7 round-trip v33 bit-exato",
		JSON.stringify(loaded.to_dict()) == JSON.stringify(saved), "difere")
	for day in 5:
		sim.tick_day()
		loaded.tick_day()
	_check("C7 continuação 5 d idêntica",
		JSON.stringify(sim.get_stock_status())
			== JSON.stringify(loaded.get_stock_status())
			and absf(float(sim.get_status().get("money_total", 0.0))
				- float(loaded.get_status().get("money_total", 0.0))) <= EPS,
		"money sim=%s loaded=%s" % [sim.get_status().get("money_total"),
			loaded.get_status().get("money_total")])


# --- C8 -------------------------------------------------------------------
## D-112 fix: o lucro de referência não nasce de um dia. Durante o aquecimento
## a referência é a MÉDIA MEDIDA (peso 1/n), o preço não reage e o índice não é
## publicado; a praça abre depois dele, no fundamental medido.
func _test_warmup() -> void:
	var sim = _world(30)
	var status: Dictionary = sim.get_stock_status()
	var exchanges: Array = status.get("exchanges", [])
	var forming := 0
	for row: Dictionary in exchanges:
		if bool(row.get("index_forming", false)) \
				and absf(float(row.get("index", 0.0)) - 100.0) <= EPS:
			forming += 1
	_check("C8 índice não é publicado durante o aquecimento (base 100)",
		not exchanges.is_empty() and forming == exchanges.size(),
		"formando=%d praças=%d" % [forming, exchanges.size()])
	var before: Dictionary = _ticker_map(sim)
	for day in 14:
		sim.tick_day()
	var after: Dictionary = _ticker_map(sim)
	var moved := 0
	var frozen := 0
	for group_id: Variant in after:
		var old_row: Dictionary = before.get(group_id, {})
		var new_row: Dictionary = after[group_id]
		var old_price := float(old_row.get("price", 0.0))
		var new_price := float(new_row.get("price", 0.0))
		if not bool(old_row.get("listed", false)) \
				or not bool(new_row.get("listed", false)):
			continue
		if old_price <= 1e-12 or new_price <= 1e-12:
			continue
		frozen += 1
		if absf(new_price - old_price) > EPS:
			moved += 1
	_check("C8 preço de referência não reage durante o aquecimento",
		frozen > 0 and moved == 0, "papeis=%d moveram=%d" % [frozen, moved])
	for day in 19:
		sim.tick_day()
	var formed: Dictionary = sim.get_stock_status()
	var still_forming := 0
	for row: Dictionary in formed.get("exchanges", []):
		if bool(row.get("index_forming", false)):
			still_forming += 1
	_check("C8 índice formado no fechamento depois do aquecimento",
		still_forming == 0, "praças_formando=%d" % still_forming)
	var compared := 0
	var worst := 0.0
	for row: Dictionary in formed.get("tickers", []):
		if not bool(row.get("listed", false)):
			continue
		var target := float(row.get("fundamental", 0.0)) \
			* _confidence_of(formed, String(row.get("exchange", "")))
		if target <= 1e-12:
			continue
		compared += 1
		worst = maxf(worst, absf(float(row.get("price", 0.0)) - target) / target)
	_check("C8 abertura da praça = fundamental MEDIDO × confiança",
		compared > 0 and worst <= 1e-6,
		"papeis=%d pior_erro=%s" % [compared, worst])
	# Fora do aquecimento o preço SEGUE o alvo (fundamental medido × confiança):
	# em 63 dias (9 fechamentos semanais) ele não vagueia por conta da semente.
	var snap: Dictionary = _ticker_map(sim)
	for day in 63:
		sim.tick_day()
	var later_status: Dictionary = sim.get_stock_status()
	var later: Dictionary = _ticker_map(sim)
	var tracked := 0
	var off_target := 0
	var worst_error := 0.0
	var detail := ""
	for group_id: Variant in later:
		var old_row: Dictionary = snap.get(group_id, {})
		var new_row: Dictionary = later[group_id]
		if not bool(new_row.get("listed", false)):
			continue
		var new_price := float(new_row.get("price", 0.0))
		var target := float(new_row.get("fundamental", 0.0)) \
			* _confidence_of(later_status, String(new_row.get("exchange", "")))
		if new_price <= 1e-12 or target <= 1e-12:
			continue
		tracked += 1
		var error := absf(new_price / target - 1.0)
		if error > worst_error:
			worst_error = error
			detail = "%s preço %.6f alvo %.6f" % [group_id, new_price, target]
		if error > 0.15:
			off_target += 1
		# Papel sem mudança de lucro nem de confiança não pode ter oscilado.
		var old_price := float(old_row.get("price", 0.0))
		var old_fund := float(old_row.get("fundamental", 0.0))
		var old_conf := _confidence_of(formed, String(old_row.get("exchange", "")))
		var new_conf := _confidence_of(later_status,
			String(new_row.get("exchange", "")))
		var new_fund := float(new_row.get("fundamental", 0.0))
		var fund_stable := old_fund <= 1e-12 \
			or absf(new_fund / old_fund - 1.0) <= 0.02
		var conf_stable := absf(new_conf / old_conf - 1.0) <= 0.02
		var price_moved := old_price > 1e-12 \
			and absf(new_price / old_price - 1.0) > 0.20
		if fund_stable and conf_stable and old_fund > 1e-12 and price_moved:
			off_target += 1
	_check("C8 preço acompanha o alvo medido e não vagueia (63 d)", tracked > 0
		and off_target == 0, "papeis=%d fora=%d pior=%s %s"
			% [tracked, off_target, worst_error, detail])


# --- C9 -------------------------------------------------------------------
## Banda de P/L declarada (3..30): o múltiplo implícito `payout/(taxa+prêmio)`
## não trava a valoração nos extremos de juro (antes: 1,5..12,5 por aritmética).
func _test_pe_band() -> void:
	var base = _world(63)
	var high = _load_with_rate(base, 2.0)
	for day in 7:
		high.tick_day()
	var status: Dictionary = high.get_stock_status()
	var low_pe := INF
	var high_pe := 0.0
	var rows := 0
	for row: Dictionary in status.get("tickers", []):
		if not bool(row.get("listed", false)):
			continue
		var shares := float(row.get("shares", 0.0))
		var ema := float(row.get("profit_ema", 0.0))
		if shares <= 0.0 or ema <= 1e-9:
			continue
		var pe := float(row.get("fundamental", 0.0)) * shares / (ema * 365.0)
		rows += 1
		low_pe = minf(low_pe, pe)
		high_pe = maxf(high_pe, pe)
	_check("C9 banda de P/L (3..30) vale nos extremos de juro",
		rows > 0 and low_pe >= 2.999 and high_pe <= 30.001,
		"papeis=%d p/l_min=%s p/l_max=%s" % [rows, low_pe, high_pe])
	var policy: Dictionary = status.get("policy", {})
	_check("C9 banda e aquecimento declarados na política",
		absf(float(policy.get("pe_floor", 0.0)) - 3.0) <= EPS
			and absf(float(policy.get("pe_ceiling", 0.0)) - 30.0) <= EPS
			and int(policy.get("index_warmup_days", 0)) > 0,
		"pe_floor=%s pe_ceiling=%s aquecimento=%s"
			% [policy.get("pe_floor"), policy.get("pe_ceiling"),
				policy.get("index_warmup_days")])


# --- helpers --------------------------------------------------------------
func _world(days: int):
	var sim = EconomySim.new()
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.set_services(_json(SERVICES_PATH))
	sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_json(AGE_STRUCTURE_PATH))
	sim.set_currencies({
		"regime": "peg",
		"currencies": [
			{
				"code": "DEM", "name": "Demo", "symbol": "D",
				"nations": ["DEMO"], "rate_per_anchor": 1.0, "rate": 1.0,
				"base_rate": 1.0,
			},
			{
				"code": "USD", "name": "Dollar", "symbol": "$",
				"nations": ["USA"], "rate_per_anchor": 2.0, "rate": 2.0,
				"base_rate": 2.0,
			},
		],
	})
	sim.initialize_scenario()
	sim.set_bank_definitions(_definition_foreign())
	for day in days:
		sim.tick_day()
	return sim


func _load(data: Dictionary):
	var sim = EconomySim.new()
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.set_services(_json(SERVICES_PATH))
	sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_json(AGE_STRUCTURE_PATH))
	sim.set_bank_definitions(_definition_foreign())
	sim.from_dict(data)
	return sim


## Mesmo mundo com a taxa básica da DEM forçada no save (choque de juro medido
## pela política: `monetary.policies[code].rate`, o estado que a C2b1 lê).
func _load_with_rate(base, rate: float):
	var data: Dictionary = base.to_dict()
	var block: Dictionary = data.get("monetary", {})
	var policies: Dictionary = block.get("policies", {})
	var dem: Dictionary = policies.get("DEM", {})
	dem["rate"] = rate
	policies["DEM"] = dem
	block["policies"] = policies
	data["monetary"] = block
	return _load(data)


func _definition_foreign() -> Dictionary:
	return {
		"nations": {
			"DEMO": {
				"public": {"status": "present", "name": "Banco Publico de Teste"},
				"private": {"status": "present", "name": "Banco Privado de Teste"},
				"foreign": {"status": "present",
					"branch_candidate": "Banco Estrangeiro de Teste",
					"origin_iso3": "USA"},
			},
		},
	}


func _confidence_of(status: Dictionary, nation: String) -> float:
	for exchange: Dictionary in status.get("exchanges", []):
		if String(exchange.get("nation", "")) == nation:
			return float(exchange.get("confidence", 1.0))
	return 1.0


func _ticker_map(sim) -> Dictionary:
	var out: Dictionary = {}
	for row: Dictionary in sim.get_stock_status().get("tickers", []):
		out[String(row.get("group_id", ""))] = row
	return out


func _first_listed_price(status: Dictionary) -> float:
	for row: Dictionary in status.get("tickers", []):
		if bool(row.get("listed", false)):
			return float(row.get("price", 0.0))
	return 0.0


func _best_paper(status: Dictionary) -> Dictionary:
	var best: Dictionary = {}
	for row: Dictionary in status.get("tickers", []):
		if not bool(row.get("listed", false)):
			continue
		if best.is_empty() or float(row.get("market_cap", 0.0)) \
				> float(best.get("market_cap", 0.0)):
			best = row
	return best


func _paper(status: Dictionary, group_id: String) -> Dictionary:
	for row: Dictionary in status.get("tickers", []):
		if String(row.get("group_id", "")) == group_id:
			return row
	return {}


func _signals_of(status: Dictionary, nation: String) -> Dictionary:
	for exchange: Dictionary in status.get("exchanges", []):
		if String(exchange.get("nation", "")) == nation:
			return exchange.get("signals", {})
	return {}


func _check(name: String, ok: bool, detail: String) -> void:
	if not ok:
		_failures.append("%s (%s)" % [name, detail])


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {}
	return JSON.parse_string(file.get_as_text())
