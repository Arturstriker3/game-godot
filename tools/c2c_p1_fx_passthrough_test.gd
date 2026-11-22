extends SceneTree

## C2c-P1 (D-109) — REPASSE SIMÉTRICO DO CÂMBIO AO PREÇO IMPORTADO.
##
## Cenário mínimo determinístico (duas cidades, duas nações, trigo):
##   SELL (NS, moeda XS) — trigo a 40 na âncora, com estoque
##   BUY  (NB, moeda XB) — sem produtor de trigo, importa
##
## O preço interno de BUY (na âncora) tem de seguir o CIF da importação
## (`origin_anchor = preço do vendedor × competitividade`) NOS DOIS SENTIDOS:
##   1. câmbio que ENCARECE a origem (competitividade 1,10 → CIF 44) SOBE o
##      preço; antes da P1 o CIF era apenas teto e o preço ficava em 40;
##   2. câmbio que BARATEIA a origem (0,90 → CIF 36) DESCE o preço;
##   3. os dois caminhos são SIMÉTRICOS a partir do mesmo preço de abertura;
##   4. nenhum salto de um dia para o outro: o passo é o da D-092
##      (max(5% do base, 6% do preço de abertura));
##   5. cotação velha não move preço: sem importação no dia (`imported` = 0)
##      o preço permanece — não se inventa cotação;
##   6. cidade COM produtor: o CIF do dia é teto do alvo custo+markup e o piso
##      é o custo unitário, NÃO o base do catálogo — importação mais barata
##      aparece no preço interno (antes o piso do base prendia em 40).
##
## Roda pelo runner:
##   powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_tests.ps1 `
##     -List c2c_p1_fx_passthrough_test

const GOODS_PATH := "res://data/definitions/economy/goods.json"

const NATION_SELL := "NS"
const NATION_BUY := "NB"
const CITY_SELL := "SELL"
const CITY_BUY := "BUY"
const PRODUCT := "wheat"
const SELL_PRICE := 40.0
const OFFSET := 100.0

var _failures: Array[String] = []
var _wheat := 0
var _base := 40.0


func _init() -> void:
	var goods := _load_array(GOODS_PATH)
	if goods.is_empty():
		_fail("goods.json vazio")
		_finish()
		return
	_wheat = _product_index(goods, PRODUCT)
	if _wheat < 0:
		_fail("goods.json sem o produto '%s'" % PRODUCT)
		_finish()
		return
	_base = float((goods[_wheat] as Dictionary).get("base_price", 40.0))
	_test_up()
	_test_down()
	_test_symmetry()
	_test_no_jump()
	_test_stale_quote()
	_test_producer_ceiling()
	_finish()


## 1. Câmbio que encarece a origem sobe o preço importado.
func _test_up() -> void:
	var sim := _make_sim()
	_warmup(sim)
	var start := _price(sim)
	var prev := start
	var worst_step := 0.0
	var fell := false
	for _day in 20:
		_tick(sim, 1.10)
		var price := _price(sim)
		worst_step = maxf(worst_step, absf(price - prev))
		if price < prev - 1e-9:
			fell = true
		prev = price
	var cif := SELL_PRICE * 1.10
	print("[c2c-p1] alta: 40,00 -> %.4f com CIF %.2f (passo maximo diario %.4f)" % [
		prev, cif, worst_step])
	_check(not fell, "alta: o preço CAIU num dia com CIF mais caro")
	_check(prev > start + 2.0,
		"alta: preço %.4f não subiu até o CIF %.2f (repasse de alta ainda bloqueado)" % [
			prev, cif])
	_check(prev <= cif + 1e-6,
		"alta: preço %.4f passou do CIF %.2f (sem razão para exceder o custo)" % [
			prev, cif])
	_check(worst_step <= maxf(_base * 0.05, start * 0.06) + 1e-6,
		"alta: salto diário %.4f acima do passo da D-092" % worst_step)


## 2. Câmbio que barateia a origem desce o preço importado.
func _test_down() -> void:
	var sim := _make_sim()
	_warmup(sim)
	var start := _price(sim)
	var prev := start
	var rose := false
	for _day in 20:
		_tick(sim, 0.90)
		var price := _price(sim)
		if price > prev + 1e-9:
			rose = true
		prev = price
	var cif := SELL_PRICE * 0.90
	print("[c2c-p1] baixa: 40,00 -> %.4f com CIF %.2f" % [prev, cif])
	_check(not rose, "baixa: o preço SUBIU num dia com CIF mais barato")
	_check(prev < start - 2.0,
		"baixa: preço %.4f não desceu até o CIF %.2f" % [prev, cif])
	_check(prev >= cif - 1e-6,
		"baixa: preço %.4f abaixo do CIF %.2f (fora do custo de importação)" % [
			prev, cif])


## 3. Simetria: mesma distância ao CIF, mesmo caminho para cima e para baixo.
func _test_symmetry() -> void:
	var up := _make_sim()
	_warmup(up)
	var start_up := _price(up)
	var down := _make_sim()
	_warmup(down)
	var start_down := _price(down)
	for _day in 12:
		_tick(up, 1.10)
		_tick(down, 0.90)
	var delta_up := _price(up) - start_up
	var delta_down := start_down - _price(down)
	print("[c2c-p1] simetria: +%.6f (alta) x -%.6f (baixa), diferenca %s" % [
		delta_up, delta_down, str(absf(delta_up - delta_down))])
	_check(delta_up > 1.0 and delta_down > 1.0,
		"simetria: um dos lados não se moveu (%.6f / %.6f)" % [delta_up, delta_down])
	_check(absf(delta_up - delta_down) <= 1e-9,
		"simetria: alta %.6f != baixa %.6f (repasse assimétrico)" % [
			delta_up, delta_down])


## 4. Nenhum salto de um dia para o outro: o limite da D-092 vale para o CIF.
func _test_no_jump() -> void:
	var sim := _make_sim()
	_warmup(sim)
	# Preço muito abaixo do CIF: um dia não pode percorrer o vão inteiro.
	sim.set_market_price(CITY_BUY, PRODUCT, 20.0)
	var step := maxf(_base * 0.05, 20.0 * 0.06)
	_tick(sim, 1.0)
	var price := _price(sim)
	var expected := 20.0 + step
	print("[c2c-p1] passo: 20,00 -> %.4f (esperado %.4f; vão até o CIF 40)" % [
		price, expected])
	_check(absf(price - expected) <= 1e-6,
		"passo: um dia andou %.4f (esperado o limite da D-092 = %.4f)" % [
			price - 20.0, step])


## 5. Cotação velha não move preço: sem importação no dia, o preço permanece.
func _test_stale_quote() -> void:
	var sim := _make_sim()
	_warmup(sim)
	for _day in 10:
		_tick(sim, 1.10)
	var before := _price(sim)
	_check(before > SELL_PRICE + 1.0,
		"stale: preço não subiu antes do teste (%.4f)" % before)
	# Vendedor sem estoque: nenhuma importação fecha, nenhuma cotação nova.
	# Os primeiros dias ainda liquidam o que estava em trânsito (cotação real
	# daquele dia); depois disso o mercado para e o preço tem de ficar parado.
	var after_trade := 0.0
	for day in 6:
		_tick(sim, 1.10, 0.0)
		if day == 1:
			after_trade = _price(sim)
	var after := _price(sim)
	print("[c2c-p1] stale: %.6f -> %.6f (comercio liquida) -> %.6f sem cotação" % [
		before, after_trade, after])
	_check(absf(after - after_trade) <= 1e-9,
		"stale: preço andou %.6f com cotação velha (não se inventa cotação)" % [
			after - after_trade])


## 6. Cidade com produtor: importação mais barata fura o piso do catálogo.
func _test_producer_ceiling() -> void:
	var buildings: Array = [{
		"city_id": CITY_BUY, "id": "BUY-FARM", "name": "fazenda",
		"resource": PRODUCT, "output_tons_per_day": 1.0,
	}]
	var sim := _make_sim(buildings)
	_warmup(sim)
	var start := _price(sim)
	# CIF 10% mais barato que o base do catálogo: com o piso rígido do base o
	# preço ficaria em ~base+markup e a importação não apareceria.
	var prev := start
	for _day in 30:
		_tick(sim, 0.90)
		prev = _price(sim)
	var cif := SELL_PRICE * 0.90
	print("[c2c-p1] produtor: %.4f -> %.4f com CIF %.2f (base %.2f)" % [
		start, prev, cif, _base])
	_check(prev < start - 1.0,
		"produtor: preço %.4f não caiu com CIF %.2f abaixo do base %.2f" % [
			prev, cif, _base])
	_check(prev >= cif - 1e-6,
		"produtor: preço %.4f abaixo do CIF %.2f (produz abaixo do custo?)" % [
			prev, cif])


# ---------------------------------------------------------------------------
# Cenário e utilitários
# ---------------------------------------------------------------------------

func _make_sim(buildings: Array = []) -> Object:
	var sim = EconomySim.new()
	sim.set_goods(_load_array(GOODS_PATH))
	sim.set_currencies({
		"version": 1,
		"anchor": "XGS",
		"regime": "peg",
		"currencies": [
			{"code": "XS", "nations": [NATION_SELL], "rate_per_anchor": 1.0,
				"source": "test"},
			{"code": "XB", "nations": [NATION_BUY], "rate_per_anchor": 1.0,
				"source": "test"},
		],
	})
	sim.initialize_world(_cities(), [], buildings)
	return sim


func _cities() -> Array:
	return [
		{
			"id": CITY_SELL, "name": "Sell", "nation": NATION_SELL,
			"population": 5000.0, "x": 2000.0 - OFFSET, "y": 1250.0,
		},
		{
			"id": CITY_BUY, "name": "Buy", "nation": NATION_BUY,
			"population": 20000.0, "x": 2000.0, "y": 1250.0,
		},
	]


## Competitividade do vendedor = taxa ÷ taxa-base (moeda flutuante). A taxa é
## reposta todo dia: o drift do `exchange::tick` não entra no teste.
func _rate(sim: Object, factor: float) -> void:
	sim.set_currency_rate("XS", factor, true, 1.0)


## Estado do dia: vendedor com excedente ao preço de catálogo, comprador sem
## estoque (com lacuna de importação).
func _prepare(sim: Object, sell_stock: float = 5000.0) -> void:
	sim.set_market_stock(CITY_SELL, PRODUCT, sell_stock)
	sim.set_market_stock(CITY_BUY, PRODUCT, 0.0)
	sim.set_market_price(CITY_SELL, PRODUCT, SELL_PRICE)


func _warmup(sim: Object) -> void:
	for _day in 3:
		_tick(sim, 1.0)


func _tick(sim: Object, comp: float, sell_stock: float = 5000.0) -> void:
	_rate(sim, comp)
	_prepare(sim, sell_stock)
	var status: Dictionary = sim.tick_day()
	var violations: Array = status.get("invariant_violations", [])
	if not violations.is_empty():
		_fail("invariante violado: %s" % String(violations[0]))


func _price(sim: Object) -> float:
	var heavy: Dictionary = sim.get_city_status(CITY_BUY)
	for market: Variant in Array(heavy.get("markets", [])):
		if typeof(market) != TYPE_DICTIONARY:
			continue
		if int((market as Dictionary).get("product", -1)) == _wheat:
			return float((market as Dictionary).get("price", 0.0))
	return 0.0


func _product_index(goods: Array, id: String) -> int:
	for index in goods.size():
		if typeof(goods[index]) == TYPE_DICTIONARY \
				and String((goods[index] as Dictionary).get("id", "")) == id:
			return index
	return -1


func _load_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(data) != TYPE_ARRAY:
		_fail(path + " não é um array JSON")
		return []
	return data


func _fail(message: String) -> void:
	_failures.append(message)


func _check(cond: bool, message: String) -> void:
	if not cond:
		_fail(message)


func _finish() -> void:
	if _failures.is_empty():
		print("[c2c-p1] OK: repasse simétrico do câmbio (alta/baixa), sem salto "
			+ "diário, cotação do dia e teto de importação na cidade produtora.")
		quit(0)
	else:
		print("[c2c-p1] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)
