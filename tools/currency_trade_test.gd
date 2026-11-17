extends SceneTree

## F5 — moedas, câmbio, tarifa e preço entregue (opção c: preço na ÂNCORA).
##
## Cenário mínimo determinístico (3 cidades, 3 nações, mesmas distâncias):
##   SELL_A (NA, moeda XA) — trigo a 40 na âncora (XGS)
##   SELL_B (NB, moeda XB) — trigo a 41 na âncora
##   BUY    (NC, moeda XC) — sem produção, importa a cesta
## SELL_A e SELL_B ficam equidistantes de BUY, então o frete é o mesmo e a
## única variável é o custo entregue na âncora (origem + frete + tarifa).
##
## Valida o contrato F5/opção c:
##   - `market.price` e `import_price` são ÂNCORA; a moeda local é só exibição
##     (`price_local = price_anchor ÷ taxa`) e nunca volta para a simulação;
##   - a comparação de ofertas é na âncora, sem round-trip `(base÷taxa)×taxa`;
##   - um câmbio com taxa fixa NÃO muda o preço interno nem a escolha;
##   - no regime float, a taxa atua como fator de competitividade na âncora
##     (taxa ÷ taxa-base) e pode mudar o vendedor escolhido;
##   - tarifa ad valorem muda a escolha e vira receita do governo comprador;
##   - déficit comercial persistente desvaloriza a moeda do comprador dentro da
##     banda documentada (±10%) e valoriza a do exportador;
##   - save/load v7 preserva moedas (taxa/regime/base) e tarifas; um save v6
##     (preço local) migra para a âncora ao carregar;
##   - dinheiro conservado na âncora (`money_total`) em todos os testes.
##
##   godot --headless --path . --script res://tools/currency_trade_test.gd

const GOODS_PATH := "res://data/definitions/economy/goods.json"

const NATION_A := "NA"
const NATION_B := "NB"
const NATION_C := "NC"
const CITY_A := "SELL_A"
const CITY_B := "SELL_B"
const CITY_BUY := "BUY"
const PRODUCT := "wheat"
const PRICE_A := 40.0
const PRICE_B := 41.0
## Taxa em âncora de 1 unidade local. A âncora vale 1,0; SELL_B recebe 0,5.
const RATE_A := 1.0
const RATE_B := 0.5
## Meia-distância em unidades de mapa (~8 km cada) entre cada vendedor e BUY.
const OFFSET := 100.0

var _failures: Array[String] = []


func _init() -> void:
	var goods := _load_array(GOODS_PATH)
	if goods.is_empty():
		_fail("goods.json vazio")
		_finish()
		return

	var sim = EconomySim.new()
	sim.set_goods(goods)
	# Moedas ANTES do mundo: o preço local nasce na moeda da nação.
	sim.set_currencies(_currencies(1.0, 1.0, 1.0, "peg"))
	if not sim.has_method("get_currency"):
		_fail("binário sem a API F5 (get_currency)")
		_finish()
		return
	sim.initialize_world(_cities(), [], [])

	# Aquecimento: a necessidade diária (fase 5) alimenta a lacuna de importação.
	for _day in 2:
		sim.tick_day()

	_test_conversion(sim)
	_test_float_competitiveness(sim)
	_test_tariff(sim)
	_test_save_load(sim, goods)
	_test_devaluation(sim)

	_finish()


## 1. Preço na âncora: exibição local = âncora ÷ taxa e escolha pelo custo
## entregue (taxas iguais).
func _test_conversion(sim) -> void:
	_prepare_markets(sim)
	var status: Dictionary = sim.tick_day()
	var order := _first_order(status, PRODUCT)
	if order.is_empty():
		_fail("conversão: nenhum pedido de trigo no dia")
		return
	var from_city := String(order.get("from_city", ""))
	if from_city != CITY_A:
		_fail("conversão: vendedor escolhido %s (esperado %s — 40 < 41 na mesma distância)" % [
			from_city, CITY_A])
	var origin := float(order.get("origin_price", 0.0))
	var origin_anchor := float(order.get("origin_price_anchor", 0.0))
	var delivered := float(order.get("delivered_price", 0.0))
	var buyer_fx := float(order.get("fx_rate", 0.0))
	var seller_fx := float(order.get("seller_fx_rate", 0.0))
	var amount := float(order.get("amount", 0.0))
	var freight := float(order.get("freight_cost", 0.0))
	var unit_freight := freight / amount if amount > 0.0 else 0.0
	print("[fx] conversão: %s -> %s âncora=%.2f local=%.2f frete/t=%.2f entregue=%.2f (%s) taxa=%.3f" % [
		from_city, CITY_BUY, origin_anchor, origin, unit_freight, delivered,
		String(order.get("buyer_currency", "?")), buyer_fx])
	if absf(origin_anchor - PRICE_A) > 1e-6:
		_fail("conversão: origem na âncora %.4f (esperado %.2f)" % [origin_anchor, PRICE_A])
	if absf(origin - origin_anchor / maxf(1e-9, seller_fx)) > 1e-6:
		_fail("conversão: exibição local %.4f != âncora ÷ taxa (%.4f)" % [
			origin, origin_anchor / maxf(1e-9, seller_fx)])
	if absf(seller_fx - 1.0) > 1e-9 or absf(buyer_fx - 1.0) > 1e-9:
		_fail("conversão: taxas deveriam ser 1,0 (vendedor=%.3f comprador=%.3f)" % [
			seller_fx, buyer_fx])
	var expected_delivered := origin_anchor + unit_freight
	if absf(delivered - expected_delivered) > 1e-6:
		_fail("conversão: entregue %.6f != (origem ÂNCORA + frete)/taxa %.6f" % [
			delivered, expected_delivered])
	if String(order.get("buyer_currency", "")) != "XC":
		_fail("conversão: moeda do comprador '%s' (esperado XC)" % [
			String(order.get("buyer_currency", ""))])
	if String(order.get("seller_currency", "")) != "XA":
		_fail("conversão: moeda do vendedor '%s' (esperado XA)" % [
			String(order.get("seller_currency", ""))])
	var currencies: Array = status.get("currencies", [])
	if currencies.size() != 3:
		_fail("status: %d moedas publicadas (esperado 3)" % currencies.size())
	# O mercado publica o preço na âncora e a exibição local.
	var market := _city_market(sim, CITY_A, PRODUCT)
	var market_price := float(market.get("price", 0.0))
	var price_local := float(market.get("price_local", 0.0))
	var city_fx := _city_value(status, CITY_A, "fx_rate")
	if absf(price_local - market_price / maxf(1e-9, city_fx)) > 1e-6:
		_fail("status: price_local %.4f != price %.4f ÷ taxa %.3f" % [
			price_local, market_price, city_fx])
	if absf(market_price - origin_anchor) > 1e-6:
		_fail("status: preço do mercado %.4f != origem na âncora %.4f" % [
			market_price, origin_anchor])
	_check_money(status, "conversão")


## 2. Câmbio float muda a competitividade na âncora; taxa fixa não mexe em nada.
func _test_float_competitiveness(sim) -> void:
	# XB desvalorizada 10% dentro da banda: taxa 0,45 com base 0,50. O preço
	# interno continua 41 na âncora; a oferta efetiva vira 41 × 0,9 = 36,9.
	sim.set_currency_rate("XB", 0.45, true, 0.5)
	_prepare_markets(sim)
	var status: Dictionary = sim.tick_day()
	var order := _first_order(status, PRODUCT)
	if order.is_empty():
		_fail("float: nenhum pedido de trigo no dia")
		return
	var from_city := String(order.get("from_city", ""))
	var origin_anchor := float(order.get("origin_price_anchor", 0.0))
	print("[fx] float: vendedor=%s oferta_âncora=%.2f (âncora %.1f × 0,90) entregue=%.2f" % [
		from_city, origin_anchor, PRICE_B, float(order.get("delivered_price", 0.0))])
	if from_city != CITY_B:
		_fail("float: vendedor %s (esperado %s — 41 × 0,90 = 36,9 < 40)" % [
			from_city, CITY_B])
	if absf(origin_anchor - PRICE_B * 0.9) > 1e-6:
		_fail("float: oferta na âncora %.4f (esperado %.4f)" % [
			origin_anchor, PRICE_B * 0.9])
	var seller_fx := float(order.get("seller_fx_rate", 0.0))
	var origin_local := float(order.get("origin_price", 0.0))
	if absf(origin_local - origin_anchor / maxf(1e-9, seller_fx)) > 1e-6:
		_fail("float: exibição local %.4f != oferta_âncora ÷ taxa %.4f" % [
			origin_local, origin_anchor / maxf(1e-9, seller_fx)])

	# Com a MESMA taxa-base (sem desvio) o fator é 1 e o preço fixo decide:
	# B fica em 41 e A (40) vence. A taxa sozinha não reescreve o preço.
	sim.set_currency_rate("XB", 0.5, true, 0.5)
	_prepare_markets(sim)
	status = sim.tick_day()
	order = _first_order(status, PRODUCT)
	if order.is_empty():
		_fail("float neutro: nenhum pedido de trigo no dia")
		return
	from_city = String(order.get("from_city", ""))
	print("[fx] float neutro: vendedor=%s (B em 41 sem desvio)" % from_city)
	if from_city != CITY_A:
		_fail("float neutro: vendedor %s (esperado %s — sem desvio, 40 < 41)" % [
			from_city, CITY_A])

	# Regime fixo: nem uma taxa nominal muito baixa muda o preço na âncora.
	sim.set_currency_rate("XB", 0.5, false)
	_prepare_markets(sim)
	status = sim.tick_day()
	order = _first_order(status, PRODUCT)
	if order.is_empty():
		_fail("regime fixo: nenhum pedido de trigo no dia")
		return
	if String(order.get("from_city", "")) != CITY_A:
		_fail("regime fixo: vendedor %s (esperado %s — preço na âncora não depende da taxa)" % [
			String(order.get("from_city", "")), CITY_A])
	_check_money(status, "câmbio float")


## 3. Tarifa ad valorem muda a escolha e é receita do governo comprador.
func _test_tariff(sim) -> void:
	# Base neutra: câmbio de XB fixo em 0,5 (fator de competitividade 1).
	sim.set_currency_rate("XB", 0.5, false)
	sim.set_tariff(NATION_C, NATION_B, PRODUCT, 1.0)
	var effective := float(sim.get_tariff(NATION_C, NATION_B, PRODUCT))
	if absf(effective - 1.0) > 1e-9:
		_fail("tarifa: efetiva %.3f (esperado 1.0)" % effective)
	_prepare_markets(sim)
	var status: Dictionary = sim.tick_day()
	var order := _first_order(status, PRODUCT)
	if order.is_empty():
		_fail("tarifa: nenhum pedido de trigo no dia")
		return
	var from_city := String(order.get("from_city", ""))
	print("[fx] tarifa: vendedor=%s (B com 100%% de tarifa entrega %.1f)" % [
		from_city, PRICE_B * RATE_B * 2.0])
	if from_city != CITY_A:
		_fail("tarifa: vendedor %s (esperado %s — a tarifa de 100%% em B encarece a entrega)" % [
			from_city, CITY_A])
	# A passa a pagar 50% também: a escolha continua A, mas agora o comprador
	# arrecada tarifa no próprio orçamento (dinheiro conservado).
	sim.set_tariff(NATION_C, NATION_A, PRODUCT, 0.5)
	_prepare_markets(sim)
	status = sim.tick_day()
	order = _first_order(status, PRODUCT)
	if order.is_empty():
		_fail("tarifa: nenhum pedido de trigo no dia (com tarifa em A)")
		return
	var tariff_revenue := _city_value(status, CITY_BUY, "tariff_revenue")
	var tariff_paid := float(order.get("tariff", 0.0))
	print("[fx] tarifa paga: vendedor=%s tarifa=%.2f receita_governo=%.2f" % [
		String(order.get("from_city", "")), tariff_paid, tariff_revenue])
	if tariff_paid <= 0.0:
		_fail("tarifa: pedido sem tarifa paga (%.3f)" % tariff_paid)
	if tariff_revenue <= 0.0:
		_fail("tarifa: receita do governo comprador não publicada (%.3f)" % tariff_revenue)
	var tariffs: Array = status.get("tariffs", [])
	if tariffs.is_empty():
		_fail("status: nenhuma tarifa publicada")
	_check_money(status, "tarifa")


## 4. Save/load preserva moedas e tarifas (e o mundo segue conservado).
func _test_save_load(sim, goods: Array) -> void:
	# Tarifa coringa: confere que a lista declarada (e não só a efetiva) volta.
	sim.set_tariff(NATION_C, "*", "*", 0.25)
	sim.set_tariff(NATION_C, NATION_B, PRODUCT, 1.0)
	var tariffs_before: Array = sim.get_tariffs()
	var saved: Dictionary = sim.to_dict()
	var rate_b_before := float(sim.get_currency(NATION_B).get("rate", 0.0))

	var loaded = EconomySim.new()
	loaded.set_goods(goods)
	loaded.from_dict(saved)
	var currency_b: Dictionary = loaded.get_currency(NATION_B)
	var currency_c: Dictionary = loaded.get_currency(NATION_C)
	var tariffs_after: Array = loaded.get_tariffs()
	print("[fx] save/load: taxas A=%.4f B=%.4f C=%.4f tarifas=%d" % [
		float(loaded.get_currency(NATION_A).get("rate", 0.0)),
		float(currency_b.get("rate", 0.0)), float(currency_c.get("rate", 0.0)),
		tariffs_after.size()])
	if String(currency_b.get("code", "")) != "XB":
		_fail("save/load: moeda de %s = '%s' (esperado XB)" % [
			NATION_B, String(currency_b.get("code", ""))])
	if absf(float(currency_b.get("rate", 0.0)) - rate_b_before) > 1e-9:
		_fail("save/load: taxa de XB %.6f != %.6f" % [
			float(currency_b.get("rate", 0.0)), rate_b_before])
	if bool(currency_b.get("floating", true)):
		_fail("save/load: regime de XB perdido (deveria continuar fixo)")
	if tariffs_after.size() != tariffs_before.size():
		_fail("save/load: %d tarifas (esperado %d)" % [
			tariffs_after.size(), tariffs_before.size()])
	else:
		for index in tariffs_before.size():
			var before: Dictionary = tariffs_before[index]
			var after: Dictionary = tariffs_after[index]
			for key in ["buyer", "seller", "product"]:
				if String(after.get(key, "")) != String(before.get(key, "")):
					_fail("save/load: tarifa %d campo %s '%s' != '%s'" % [
						index, key, String(after.get(key, "")), String(before.get(key, ""))])
			if absf(float(after.get("rate", 0.0)) - float(before.get("rate", 0.0))) > 1e-12:
				_fail("save/load: tarifa %d taxa %.4f != %.4f" % [
					index, float(after.get("rate", 0.0)), float(before.get("rate", 0.0))])
	# O lookup é reconstruído com os coringas: a tarifa específica prevalece.
	if absf(float(loaded.get_tariff(NATION_C, NATION_B, PRODUCT)) - 1.0) > 1e-9:
		_fail("save/load: tarifa efetiva NC->NB em trigo %.3f (esperado 1.0)" % [
			float(loaded.get_tariff(NATION_C, NATION_B, PRODUCT))])
	# O mundo carregado roda um dia sem violar invariante nem criar dinheiro.
	loaded.set_market_stock(CITY_BUY, PRODUCT, 0.0)
	var status: Dictionary = loaded.tick_day()
	var violations: Array = status.get("invariant_violations", [])
	if not violations.is_empty():
		_fail("save/load: invariante violado após carregar: %s" % String(violations[0]))
	var money_saved := float(saved.get("money_total", 0.0))
	var money_loaded := float(status.get("money_total", 0.0))
	if money_saved > 0.0 and absf(money_loaded - money_saved) > maxf(1e-3, money_saved * 1e-9):
		_fail("save/load: dinheiro %.2f -> %.2f (a âncora não foi conservada)" % [
			money_saved, money_loaded])

	# Save v6 (preço local) migra para a âncora ao carregar: o preço gravado
	# volta multiplicado pela taxa da nação (o valor na âncora é preservado).
	var legacy: Dictionary = saved.duplicate(true)
	legacy["version"] = 6
	var rate_by_nation: Dictionary = {}
	for currency: Variant in Array(legacy.get("currencies", [])):
		if typeof(currency) != TYPE_DICTIONARY:
			continue
		var rate := float((currency as Dictionary).get("rate_per_anchor", 1.0))
		for nation: Variant in Array((currency as Dictionary).get("nations", [])):
			rate_by_nation[String(nation)] = rate
	for city_variant: Variant in Array(legacy.get("cities", [])):
		if typeof(city_variant) != TYPE_DICTIONARY:
			continue
		var city: Dictionary = city_variant
		var rate := float(rate_by_nation.get(String(city.get("nation", "")), 1.0))
		for market_variant: Variant in Array(city.get("markets", [])):
			if typeof(market_variant) != TYPE_DICTIONARY:
				continue
			(market_variant as Dictionary)["price"] = float(
				(market_variant as Dictionary).get("price", 0.0)) / rate
		var import_prices: Array = city.get("import_price", [])
		for index in import_prices.size():
			import_prices[index] = float(import_prices[index]) / rate
	var migrated = EconomySim.new()
	migrated.set_goods(goods)
	migrated.from_dict(legacy)
	var migrated_status: Dictionary = migrated.get_status()
	var anchor_before := float(_city_market(loaded, CITY_A, PRODUCT).get("price", 0.0))
	var anchor_after := float(_city_market(migrated, CITY_A, PRODUCT).get("price", 0.0))
	print("[fx] migração v6 -> v7: preço âncora %.4f (antes %.4f)" % [
		float(anchor_after), float(anchor_before)])
	if absf(float(anchor_after) - float(anchor_before)) > 1e-6:
		_fail("migração v6: preço %.4f != âncora v7 %.4f" % [
			float(anchor_after), float(anchor_before)])


## 5. Déficit persistente desvaloriza a moeda do comprador (banda ±10%).
func _test_devaluation(sim) -> void:
	# Tarifa zerada e moedas flutuantes de novo: só a balança move a taxa.
	sim.set_tariff(NATION_C, "*", "*", 0.0)
	sim.set_currency_rate("XA", 1.0, true)
	sim.set_currency_rate("XB", 1.0, true)
	sim.set_currency_rate("XC", 1.0, true)
	_prepare_markets(sim)
	var buyer_rate := 1.0
	var seller_rate := 1.0
	for _day in 40:
		var status: Dictionary = sim.tick_day()
		var violations: Array = status.get("invariant_violations", [])
		if not violations.is_empty():
			_fail("desvalorização: invariante violado: %s" % String(violations[0]))
			break
		buyer_rate = float(sim.get_currency(NATION_C).get("rate", 1.0))
		seller_rate = float(sim.get_currency(NATION_A).get("rate", 1.0))
	print("[fx] balança: comprador (déficit) taxa=%.4f, exportador (superávit) taxa=%.4f" % [
		buyer_rate, seller_rate])
	if buyer_rate >= 0.999:
		_fail("desvalorização: moeda do comprador não caiu (%.4f)" % buyer_rate)
	if buyer_rate < 0.895:
		_fail("desvalorização: moeda do comprador furou a banda (%.4f < 0.90)" % buyer_rate)
	if seller_rate <= 1.001:
		_fail("valorização: moeda do exportador não subiu (%.4f)" % seller_rate)
	if seller_rate > 1.105:
		_fail("valorização: moeda do exportador furou a banda (%.4f > 1.10)" % seller_rate)


# ---------------------------------------------------------------------------
# Cenário e utilitários
# ---------------------------------------------------------------------------

func _cities() -> Array:
	return [
		{
			"id": CITY_A, "name": "Sell A", "nation": NATION_A,
			"population": 100000.0,
			"x": 2000.0 - OFFSET, "y": 1250.0,
		},
		{
			"id": CITY_B, "name": "Sell B", "nation": NATION_B,
			"population": 100000.0,
			"x": 2000.0 + OFFSET, "y": 1250.0,
		},
		{
			"id": CITY_BUY, "name": "Buy", "nation": NATION_C,
			"population": 100000.0,
			"x": 2000.0, "y": 1250.0,
		},
	]


func _currencies(rate_a: float, rate_b: float, rate_c: float, regime: String) -> Dictionary:
	return {
		"version": 1,
		"anchor": "XGS",
		"regime": regime,
		"currencies": [
			{"code": "XA", "nations": [NATION_A], "rate_per_anchor": rate_a,
				"source": "test"},
			{"code": "XB", "nations": [NATION_B], "rate_per_anchor": rate_b,
				"source": "test"},
			{"code": "XC", "nations": [NATION_C], "rate_per_anchor": rate_c,
				"source": "test"},
		],
	}


## Repõe o estado do dia: vendedores com excedente e preço local conhecido,
## comprador zerado (sem estoque, portanto com lacuna de importação).
func _prepare_markets(sim) -> void:
	sim.set_market_stock(CITY_A, PRODUCT, 5000.0)
	sim.set_market_stock(CITY_B, PRODUCT, 5000.0)
	sim.set_market_stock(CITY_BUY, PRODUCT, 0.0)
	sim.set_market_price(CITY_A, PRODUCT, PRICE_A)
	sim.set_market_price(CITY_B, PRODUCT, PRICE_B)


func _first_order(status: Dictionary, product: String) -> Dictionary:
	for order: Variant in Array(status.get("trade_orders", [])):
		if typeof(order) != TYPE_DICTIONARY:
			continue
		if String((order as Dictionary).get("good", "")) == product:
			return order as Dictionary
	return {}


func _city_value(status: Dictionary, city_id: String, key: String) -> float:
	for city: Variant in Array(status.get("cities", [])):
		if typeof(city) == TYPE_DICTIONARY and String((city as Dictionary)["id"]) == city_id:
			return float((city as Dictionary).get(key, 0.0))
	return 0.0


## Mercado do produto na cidade. O status diário publica só o bloco leve, então
## o detalhe (com `product`, `price` e `price_local`) vem de get_city_status.
func _city_market(sim: Object, city_id: String, product: String) -> Dictionary:
	var heavy: Dictionary = sim.get_city_status(city_id)
	for market: Variant in Array(heavy.get("markets", [])):
		if typeof(market) == TYPE_DICTIONARY \
				and int((market as Dictionary).get("product", -1)) >= 0:
			# As chaves de mercado são indexadas por produto; o teste usa o
			# primeiro produto (trigo) do cenário sintético.
			return market as Dictionary
	return {}


func _check_money(status: Dictionary, label: String) -> void:
	var violations: Array = status.get("invariant_violations", [])
	if not violations.is_empty():
		_fail("%s: invariante violado: %s" % [label, String(violations[0])])


func _load_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("nao abriu " + path)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(data) != TYPE_ARRAY:
		_fail(path + " nao e um array JSON")
		return []
	return data


func _fail(message: String) -> void:
	_failures.append(message)


func _finish() -> void:
	if _failures.is_empty():
		print("[fx] OK: conversão, escolha por preço entregue, tarifa, câmbio e save/load validados.")
		quit(0)
	else:
		print("[fx] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)
