extends SceneTree

## B6 — PATRIMÔNIO IMOBILIÁRIO DA REGIÃO (D-095): terra produtiva, moradia do
## interior e anel periurbano; propriedade por quintil; Gini de riqueza; save
## v24 com a cadeia aditiva v22→v23 (B6)→v24 (G).
##
## Valida:
##   (a) valorização por OBRA DE INFRA (infra B5 sobe → valor da terra sobe);
##   (b) valorização por PREÇO DE COMMODITY (boom sobe, queda desce);
##   (c) TRANSBORDO para o anel quando a cidade-polo lota (o excedente migra,
##       não é barrado; pessoas conservadas);
##   (d) distribuição por QUINTIL (soma = total; concentração segue o Gini
##       fundiário dado; sem dado usa a repartição de renda);
##   (e) CONSERVAÇÃO DE DINHEIRO com o aluguel do anel (money_total inalterado,
##       débito/crédito casados);
##   (f) SAVE v24: round-trip bit-exato sem tick, migração v22→v24 (região
##       inerte até o dado ser reaplicado; alíquota 0) e a CADEIA v23→v24 (save
##       da versão que já tinha patrimônio e ainda não tinha agricultura).
##
## Uso: godot --headless --path . --script res://tools/b6_property_test.gd

const RegionDataScript := preload("res://core/economy/region_data.gd")
const LandPropertyScript := preload("res://core/economy/land_property.gd")
const AgricultureScript := preload("res://core/economy/agriculture_service.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"
const LAND_PROPERTY_PATH := "res://data/definitions/economy/land_property.json"

const SAVE_VERSION := 30
const INFRA_LEVEL := 3
const INFRA_VALUE_MULTIPLIER := 1.5   # 1 + kPropertyInfraValueGain (0,50)
const RING_MONTHLY_ABSORB := 0.005    # kPropertyRingMonthlyAbsorb

var _failures: Array = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[b6] OK: terra/moradia/anel, quintis, conservação e save v24.")
		quit(0)
	else:
		print("[b6] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _check(cond: bool, message: String) -> void:
	if not cond:
		_fail(message)


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_chains(sim: Object) -> void:
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	if sim.has_method("set_services"):
		sim.set_services(_json(SERVICES_PATH))
	if sim.has_method("set_currencies"):
		sim.set_currencies(_json(CURRENCIES_PATH))
	if sim.has_method("set_sector_employment"):
		sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	if sim.has_method("set_age_structure"):
		sim.set_age_structure(_json(AGE_STRUCTURE_PATH))


## Mundo real com (ou sem) a B6 aplicada. `with_property = false` é o controle
## da B4 sozinha (sem hectares/anel) — prova que o transbordo é da B6.
func _make_world(with_property: bool = true) -> Object:
	var sim = EconomySim.new()
	_load_chains(sim)
	var cities: Array = _json(CITIES_PATH)
	var ports: Array = _json(PORTS_PATH)
	var buildings: Array = _json(BUILDINGS_PATH)
	RegionDataScript.attach(cities)
	sim.initialize_world(cities, ports, buildings)
	if sim.has_method("set_urban"):
		sim.set_urban(_json(URBAN_LAND_PATH), _json(HOUSING_CITY_PATH),
			_json(HOUSING_PATH), _json(MUNICIPAL_FINANCE_PATH))
	if with_property:
		_check(LandPropertyScript.apply(sim) > 0,
			"(setup) LandProperty.apply deveria semear as regiões")
	return sim


func _cities(save: Dictionary) -> Dictionary:
	var out := {}
	for city: Dictionary in save.get("cities", []):
		out[String(city.get("id", ""))] = city
	return out


func _city_index(save: Dictionary, city_id: String) -> int:
	var cities: Array = save.get("cities", [])
	for index in cities.size():
		if String(cities[index].get("id", "")) == city_id:
			return index
	return -1


func _region(save: Dictionary, city_id: String) -> Dictionary:
	var index := _city_index(save, city_id)
	if index < 0:
		return {}
	var cities: Array = save.get("cities", [])
	return cities[index].get("region", {})


## Preço corrente de um produto no mercado da cidade (o bloco `markets` é um
## Array na ordem de `get_products()`).
func _market_price(sim: Object, city_id: String, product: String) -> float:
	var products: Array = sim.get_products()
	var product_index := products.find(product)
	if product_index < 0:
		return 0.0
	var markets: Array = sim.get_city_status(city_id).get("markets", [])
	if product_index >= markets.size():
		return 0.0
	return float(markets[product_index].get("price", 0.0))


## Mora no anel? A B6 põe `ring_population` no bloco `property` da região.
func _property(sim: Object, city_id: String) -> Dictionary:
	return sim.get_region_status(city_id).get("property", {})


## Dias até o próximo fechamento mensal (A6) da cidade de índice `index`.
func _days_until_close(day: int, index: int) -> int:
	var delta := (index - (day % 30) + 30) % 30
	return 30 if delta == 0 else delta


## Primeira cidade com patrimônio pronto, interior povoado e um produtor
## primário (o índice de commodities precisa de uma fazenda/mina).
func _pick_property_city(sim: Object, save: Dictionary) -> String:
	var land: Dictionary = LandPropertyScript.region_land()
	for city: Dictionary in save.get("cities", []):
		var city_id := String(city.get("id", ""))
		if not land.has(city_id):
			continue
		var region: Dictionary = city.get("region", {})
		if not bool(region.get("ready", false)):
			continue
		if float(region.get("region_pop", 0.0)) <= 0.0:
			continue
		if float(city.get("firm_cash", 0.0)) <= 0.0:
			continue
		if _property(sim, city_id).is_empty():
			continue
		if _primary_product_if_any(sim, city_id) != "":
			return city_id
	return ""


## Produto primário (fazenda/mina) presente na cidade, se houver. O bloco da
## construção traz `recipe` (String vazia na extração), `input_product` (-1) e
## `resource` — que É o produto da fazenda (o mesmo nome do mercado).
func _primary_product_if_any(sim: Object, city_id: String) -> String:
	var products: Array = sim.get_products()
	var status: Dictionary = sim.get_city_status(city_id)
	for building: Dictionary in status.get("buildings", []):
		if String(building.get("recipe", "")) != "":
			continue
		if int(building.get("input_product", -1)) >= 0:
			continue
		if int(building.get("output_product", -1)) < 0:
			continue
		var resource := String(building.get("resource", ""))
		if products.has(resource):
			return resource
	return ""


## Primeiro caminho que difere entre dois valores (dict/array/escalar). "" = igual.
func _first_diff(a: Variant, b: Variant, path: String = "") -> String:
	if typeof(a) != typeof(b):
		return "%s: tipo %d != %d" % [path, typeof(a), typeof(b)]
	match typeof(a):
		TYPE_DICTIONARY:
			var da: Dictionary = a
			var db: Dictionary = b
			var keys: Array = da.keys()
			for key: Variant in db.keys():
				if not keys.has(key):
					keys.append(key)
			for key: Variant in keys:
				if not da.has(key) or not db.has(key):
					return "%s/%s: presente em um só" % [path, str(key)]
				var inner := _first_diff(da[key], db[key], path + "/" + str(key))
				if inner != "":
					return inner
		TYPE_ARRAY:
			var aa: Array = a
			var ab: Array = b
			if aa.size() != ab.size():
				return "%s: tamanho %d != %d" % [path, aa.size(), ab.size()]
			for index in aa.size():
				var inner := _first_diff(aa[index], ab[index],
					path + "[%d]" % index)
				if inner != "":
					return inner
		TYPE_FLOAT, TYPE_INT:
			if typeof(a) == TYPE_FLOAT or typeof(b) == TYPE_FLOAT:
				if absf(float(a) - float(b)) > maxf(absf(float(a)) * 1e-12, 1e-12):
					return "%s: %s != %s" % [path, String.num(float(a), 17),
						String.num(float(b), 17)]
			elif a != b:
				return "%s: %s != %s" % [path, str(a), str(b)]
		_:
			if str(a) != str(b):
				return "%s: %s != %s" % [path, str(a), str(b)]
	return ""


## Bloco `property` de todas as regiões (`{city_id: property}`), para comparar
## o estado da B6 entre dois saves.
func _property_blocks(save: Dictionary) -> Dictionary:
	var out := {}
	for city: Dictionary in save.get("cities", []):
		var property: Dictionary = city.get("region", {}).get("property", {})
		if not property.is_empty():
			out[String(city.get("id", ""))] = property
	return out


func _states_levels(status: Dictionary, level: int) -> Dictionary:
	var out := {}
	for state: Dictionary in status.get("states", []):
		out[String(state.get("id", ""))] = {
			"road": level, "power": level, "water": level,
		}
	return out


func _run() -> void:
	_check_land_data()
	_check_land_value_by_infra()
	_check_land_value_by_commodity()
	_check_ring_overflow()
	_check_quintiles()
	_check_money_conservation()
	_check_save()


## (d0) O DADO: hectares por estado conservam o total nacional (regra de
## rateio declarada no arquivo) e o Gini sai do observado/fallback declarado.
func _check_land_data() -> void:
	var audit: Dictionary = LandPropertyScript.audit()
	var document: Dictionary = _json(LAND_PROPERTY_PATH)
	var nations: Dictionary = document.get("nations", {})
	var attributed: Dictionary = audit.get("national_ha", {})
	_check(int(audit.get("nations_with_land", 0)) == 178,
		"(d0) o dado deveria ter 178 nações com hectares (é %s)" % audit.get(
			"nations_with_land"))
	_check(not attributed.has("XKX"),
		"(d0) XKX não tem hectare no dado e não pode aparecer atribuído")
	var mismatched: Array = []
	for tag: String in attributed:
		var row: Dictionary = nations.get(tag, {})
		var expected := float(row.get("agricultural_land_ha", 0.0))
		var actual := float(attributed[tag])
		if absf(actual - expected) > maxf(expected * 1e-9, 1e-6):
			mismatched.append("%s: %.3f != %.3f" % [tag, actual, expected])
	_check(mismatched.is_empty(),
		"(d0) rateio não conserva o total nacional em %d nações: %s" % [
			mismatched.size(), str(mismatched.slice(0, 3))])
	var bra: Dictionary = nations.get("BRA", {})
	var land: Dictionary = LandPropertyScript.region_land()
	var bra_city := ""
	var bra_total := 0.0
	for city_id: String in land:
		if not city_id.begins_with("BRA-"):
			continue
		bra_city = city_id
	# Hectares do BRA atribuídos (soma dos estados do arquivo) × dado.
	_check(absf(float(attributed.get("BRA", 0.0))
			- float(bra.get("agricultural_land_ha", 0.0)))
			<= float(bra.get("agricultural_land_ha", 1.0)) * 1e-9,
		"(d0) BRA: hectares atribuídos != dado")
	if bra_city != "":
		_check(bool(land[bra_city].get("gini_observed", false)),
			"(d0) BRA tem Gini fundiário observado no dado")
		_check(absf(float(land[bra_city].get("land_gini", 0.0))
				- float(bra.get("land_gini", 0.0))) < 1e-9,
			"(d0) BRA: Gini da região != Gini do dado")
	print("[b6] (d0) dado: %d nações com hectare, %d atribuídas, total conservado; BRA %.0f ha (Gini %.4f)." % [
		int(audit.get("nations_with_land", 0)), int(audit.get("nations_attributed", 0)),
		float(attributed.get("BRA", 0.0)),
		float(land.get(bra_city, {}).get("land_gini", 0.0))])


## (a) Obra de infra eleva o valor da terra (A/B com o MESMO mundo, infra 0 × 3).
func _check_land_value_by_infra() -> void:
	var low := _make_world()
	var save_low: Dictionary = low.to_dict()
	var city_id := _pick_property_city(low, save_low)
	if city_id == "":
		_fail("(a) nenhuma cidade com patrimônio e produtor primário")
		return
	var high := _make_world(false)
	var levels := _states_levels(low.get_region_status(city_id), INFRA_LEVEL)
	high.set_region_infra(levels)
	LandPropertyScript.apply(high)
	for day in 31:
		low.tick_day()
		high.tick_day()
	var property_low := _property(low, city_id)
	var property_high := _property(high, city_id)
	var multiplier_low := float(property_low.get("infra_value_multiplier", 0.0))
	var multiplier_high := float(property_high.get("infra_value_multiplier", 0.0))
	var value_low := float(property_low.get("land_value_per_ha", 0.0))
	var value_high := float(property_high.get("land_value_per_ha", 0.0))
	_check(absf(multiplier_low - 1.0) < 1e-9,
		"(a) infra 0 deveria dar multiplicador 1,0 (é %.6f)" % multiplier_low)
	_check(absf(multiplier_high - INFRA_VALUE_MULTIPLIER) < 1e-9,
		"(a) infra 3/3/3 deveria dar multiplicador %.2f (é %.6f)" % [
			INFRA_VALUE_MULTIPLIER, multiplier_high])
	_check(value_low > 0.0, "(a) valor da terra ficou 0 com renda medida")
	_check(value_high > value_low * 1.05,
		"(a) infra não valorizou a terra: %.6f (0) vs %.6f (3)" % [
			value_low, value_high])
	print("[b6] (a) infra: valor/ha %.6f → %.6f (×%.3f), multiplicador %.2f." % [
		value_low, value_high, value_high / maxf(value_low, 1e-9),
		multiplier_high])


## (b) Boom de commodity sobe o índice e o valor; queda desce.
func _check_land_value_by_commodity() -> void:
	var sim := _make_world()
	for day in 31:
		sim.tick_day()
	var base: Dictionary = sim.to_dict()
	var city_id := _pick_property_city(sim, base)
	if city_id == "":
		_fail("(b) nenhuma cidade com patrimônio e produtor primário")
		return
	var product := _primary_product_if_any(sim, city_id)
	if product == "":
		_fail("(b) cidade sem produto primário")
		return
	var base_price := _market_price(sim, city_id, product)
	if base_price <= 0.0:
		_fail("(b) produto %s sem preço" % product)
		return
	var index := _city_index(base, city_id)
	var day := int(base.get("day", 0))
	var days_until := _days_until_close(day, index)
	var value_before := float(_property(sim, city_id).get("land_value_per_ha", 0.0))

	var boom := _restore(base)
	var bust := _restore(base)
	for step in days_until - 1:
		boom.tick_day()
		bust.tick_day()
	# O preço entra na fila A3: aplicado no início do dia do fechamento, ANTES
	# da fase de patrimônio (que roda bem antes da regra de preço do dia).
	boom.set_market_price(city_id, product, base_price * 3.0)
	bust.set_market_price(city_id, product, base_price * 0.30)
	boom.tick_day()
	bust.tick_day()

	var property_boom := _property(boom, city_id)
	var property_bust := _property(bust, city_id)
	var index_boom := float(property_boom.get("commodity_index", 0.0))
	var index_bust := float(property_bust.get("commodity_index", 0.0))
	var value_boom := float(property_boom.get("land_value_per_ha", 0.0))
	var value_bust := float(property_bust.get("land_value_per_ha", 0.0))
	_check(index_boom > 1.0,
		"(b) boom deveria elevar o índice de commodities (é %.6f)" % index_boom)
	_check(index_bust < 1.0,
		"(b) queda deveria reduzir o índice de commodities (é %.6f)" % index_bust)
	_check(value_boom > value_before,
		"(b) boom não subiu o valor da terra (%.6f → %.6f)" % [
			value_before, value_boom])
	_check(value_bust < value_before,
		"(b) queda não desceu o valor da terra (%.6f → %.6f)" % [
			value_before, value_bust])
	print("[b6] (b) commodity %s: índice %.3f (boom) / %.3f (queda); valor/ha %.6f → %.6f / %.6f." % [
		product, index_boom, index_bust, value_before, value_boom, value_bust])


func _restore(save: Dictionary) -> Object:
	var sim = EconomySim.new()
	_load_chains(sim)
	sim.from_dict(save)
	return sim


## Load pelo SNAPSHOT (definições + save), que é o caminho real do jogo: as
## definições (inclusive os planos de safra) vêm no próprio snapshot.
func _restore_snapshot(snapshot: Dictionary) -> Object:
	var sim = EconomySim.new()
	sim.from_snapshot(snapshot)
	return sim


## (c) Anel periurbano: com a B4 sozinha o excedente do êxodo é barrado pela
## vaga; com a B6 ele assenta no anel. Pessoas conservadas nos dois mundos.
func _check_ring_overflow() -> void:
	var with_ring := _make_world(true)
	var control := _make_world(false)
	var days := 120
	for day in days:
		with_ring.tick_day()
		control.tick_day()
	var save: Dictionary = with_ring.to_dict()
	var control_save: Dictionary = control.to_dict()
	var ring_city := ""
	var ring_city_in := 0.0
	for city: Dictionary in save.get("cities", []):
		var region: Dictionary = city.get("region", {})
		var property: Dictionary = region.get("property", {})
		var ring_in := float(property.get("migration_ring_in_total", 0.0))
		if ring_in > 0.0 and ring_city == "":
			ring_city = String(city.get("id", ""))
			ring_city_in = ring_in
	_check(ring_city != "",
		"(c) nenhuma região assentou o excedente no anel em %d dias" % days)
	if ring_city == "":
		return
	# Pessoas conservadas: Σ urbano + Σ interior == Σ população exibida. O anel
	# é um SUBCONJUNTO do interior (não entra duas vezes).
	for label: String in ["com B6", "controle"]:
		var data: Dictionary = save if label == "com B6" else control_save
		var urban := 0.0
		var region := 0.0
		var total := 0.0
		var ring_total := 0.0
		for city: Dictionary in data.get("cities", []):
			urban += float(city.get("sim_population", 0.0))
			region += float(city.get("region", {}).get("region_pop", 0.0))
			total += float(city.get("population", 0.0))
			ring_total += float(city.get("region", {}).get("property", {}).get(
				"ring_population", 0.0))
		_check(absf((urban + region) - total) <= 1.0,
			"(c) %s: Σ urbano+interior %.0f != Σ população %.0f" % [
				label, urban + region, total])
		_check(ring_total <= region + 1.0,
			"(c) %s: anel (%.0f) maior que o interior (%.0f)" % [
				label, ring_total, region])
	# O transbordo NÃO é barrado: com B6 o total de gente que saiu do campo
	# (para o urbano + para o anel) é pelo menos o do controle, e parte foi para
	# o anel — o fluxo, não o estoque.
	var moved_with := 0.0
	var ring_flow := 0.0
	var ring_total := 0.0
	var moved_control := 0.0
	for city: Dictionary in save.get("cities", []):
		var region: Dictionary = city.get("region", {})
		moved_with += float(region.get("migration", {}).get("rural_in_total", 0.0))
		ring_flow += float(region.get("property", {}).get(
			"migration_ring_in_total", 0.0))
		ring_total += float(region.get("property", {}).get(
			"ring_population", 0.0))
	for city: Dictionary in control_save.get("cities", []):
		moved_control += float(city.get("region", {}).get("migration", {}).get(
			"rural_in_total", 0.0))
	_check(ring_total > 0.0, "(c) anel vazio no fim de %d dias" % days)
	_check(ring_flow > 0.0, "(c) nenhum fluxo para o anel em %d dias" % days)
	_check(moved_control > 0.0,
		"(c) o controle (sem B6) não teve êxodo — o A/B não testa o transbordo")
	_check(moved_with + ring_flow >= moved_control * 0.999,
		"(c) B6 moveu menos gente que o controle (%.0f + %.0f vs %.0f)" % [
			moved_with, ring_flow, moved_control])
	print("[b6] (c) anel: %s recebeu %.0f; fluxo p/ o anel %.0f; população do anel %.0f; êxodo urbano %.0f (controle %.0f)." % [
		ring_city, ring_city_in, ring_flow, ring_total, moved_with, moved_control])


## (d) Distribuição por quintil: soma = total; concentração segue o Gini
## fundiário quando observado; sem dado usa a repartição de renda da região.
func _check_quintiles() -> void:
	var sim := _make_world()
	for day in 31:
		sim.tick_day()
	var save: Dictionary = sim.to_dict()
	var land: Dictionary = LandPropertyScript.region_land()
	var observed_city := ""
	var fallback_city := ""
	for city: Dictionary in save.get("cities", []):
		var city_id := String(city.get("id", ""))
		if not land.has(city_id):
			continue
		var region: Dictionary = city.get("region", {})
		if float(region.get("region_pop", 0.0)) <= 0.0:
			continue
		var entry: Dictionary = land[city_id]
		if bool(entry.get("gini_observed", false)) and observed_city == "":
			observed_city = city_id
		if not bool(entry.get("gini_observed", false)) and fallback_city == "":
			fallback_city = city_id
	var checked := 0
	for city_id: String in [observed_city, fallback_city]:
		if city_id == "":
			continue
		var property := _property(sim, city_id)
		var status: Dictionary = sim.get_region_status(city_id)
		var land_value := float(property.get("land_value", 0.0))
		var row: Array = property.get("land_value_by_quintile", [])
		var sum := 0.0
		for value: Variant in row:
			sum += float(value)
		_check(absf(sum - land_value) <= maxf(land_value * 1e-9, 1e-6),
			"(d) soma da terra por quintil (%.6f) != terra total (%.6f) em %s" % [
				sum, land_value, city_id])
		var wealth_row: Array = property.get("wealth_by_quintile", [])
		var wealth_sum := 0.0
		for value: Variant in wealth_row:
			wealth_sum += float(value)
		var savings_sum := 0.0
		for value: Variant in status.get("strata_savings", []):
			savings_sum += float(value)
		var total_value := float(property.get("total_value", 0.0))
		_check(absf(wealth_sum - (total_value + savings_sum))
				<= maxf(total_value * 1e-6, 1e-6),
			"(d) patrimônio por quintil != ativos + poupança em %s (%.3f vs %.3f)" % [
				city_id, wealth_sum, total_value + savings_sum])
		# Concentração: Q5 > Q4 > ... > Q1 na terra (lognormal do Gini).
		if row.size() == 5 and land_value > 0.0:
			for quintile in range(1, 5):
				_check(float(row[quintile]) >= float(row[quintile - 1]),
					"(d) quintil %d < %d na terra de %s" % [
						quintile + 1, quintile, city_id])
			if city_id == observed_city:
				var income_row: Array = status.get("strata_income_share", [])
				var land_ratio := float(row[4]) / maxf(float(row[0]), 1e-12)
				var income_ratio := float(income_row[4]) \
					/ maxf(float(income_row[0]), 1e-12)
				_check(bool(property.get("land_gini_observed", false)),
					"(d) %s deveria ter Gini fundiário OBSERVADO" % city_id)
				_check(land_ratio >= income_ratio,
					"(d) terra menos concentrada que a renda em %s (%.2f < %.2f)" % [
						city_id, land_ratio, income_ratio])
		# Sem dado: a terra segue a repartição de RENDA da região.
		if city_id == fallback_city:
			var income_share: Array = status.get("strata_income_share", [])
			var same := income_share.size() == row.size()
			if same:
				for quintile in row.size():
					if absf(float(row[quintile])
							- land_value * float(income_share[quintile])) > 1e-3:
						same = false
			_check(not bool(property.get("land_gini_observed", false)),
				"(d) %s não deveria ter Gini observado" % city_id)
			_check(same, "(d) fallback de %s não seguiu a renda da região" % city_id)
		checked += 1
	_check(checked >= 1, "(d) nenhuma cidade com patrimônio para conferir quintis")
	print("[b6] (d) quintis: %d cidades conferidas (observado=%s, fallback=%s)." % [
		checked, observed_city, fallback_city])


## (e) O aluguel do anel é débito/crédito DENTRO da região: `money_total` não
## muda e o caixa da região (escalar) continua igual à soma da poupança dos
## quintis.
func _check_money_conservation() -> void:
	var sim := _make_world()
	var money_start := float(sim.get_status().get("money_total", 0.0))
	var days := 120
	for day in days:
		sim.tick_day()
	var money_end := float(sim.get_status().get("money_total", 0.0))
	var drift := absf(money_end - money_start) / maxf(absf(money_start), 1.0)
	_check(drift <= 1e-6,
		"(e) dinheiro não conservado: drift %.9f em %d dias" % [drift, days])
	_check(sim.get_status().get("invariant_violations", []).is_empty(),
		"(e) invariantes violados: %s" % str(
			sim.get_status().get("invariant_violations", [])))
	var rent_city := ""
	var rent_paid := 0.0
	for city: Dictionary in sim.to_dict().get("cities", []):
		var property: Dictionary = city.get("region", {}).get("property", {})
		if float(property.get("ring_rent_paid_month", 0.0)) > 0.0:
			rent_city = String(city.get("id", ""))
			rent_paid = float(property.get("ring_rent_paid_month", 0.0))
			break
	_check(rent_city != "", "(e) nenhuma região pagou aluguel de anel em %d dias" % days)
	if rent_city != "":
		var status: Dictionary = sim.get_region_status(rent_city)
		var savings_sum := 0.0
		for value: Variant in status.get("strata_savings", []):
			savings_sum += float(value)
		var cash := float(status.get("cash", 0.0))
		_check(absf(savings_sum - cash) <= maxf(absf(cash) * 1e-9, 1e-6),
			"(e) caixa da região (%.6f) != Σ poupança dos quintis (%.6f) em %s" % [
				cash, savings_sum, rent_city])
		print("[b6] (e) aluguel do anel: %s pagou %.6f/mês; caixa == Σ poupança; drift %.12f." % [
			rent_city, rent_paid, drift])
	else:
		print("[b6] (e) drift de dinheiro %.12f em %d dias." % [drift, days])


## (f) Save v24: round-trip sem tick bit-exato e migração v22→v23→v24.
func _check_save() -> void:
	var sim := _make_world()
	for day in 31:
		sim.tick_day()
	var save: Dictionary = sim.to_dict()
	_check(int(save.get("version", 0)) == SAVE_VERSION,
		"(f) versão do save deveria ser %d (é %s)" % [
			SAVE_VERSION, save.get("version")])
	var city_id := _pick_property_city(sim, save)
	if city_id == "":
		_fail("(f) nenhuma cidade com patrimônio")
		return
	var property := _property(sim, city_id)
	_check(bool(property.get("ready", false)),
		"(f) região deveria ter patrimônio pronto")
	# Round-trip sem tick: o BLOCO da B6 volta idêntico e o ciclo é ESTÁVEL (o
	# segundo save/load não muda nada). O save do mundo carrega campos DERIVADOS
	# que o load RECOMPUTA (`pop.labor_force`/`dependents` a partir das coortes,
	# diferença de 1e-5 relativa) — pré-existente, fora da B6; por isso a
	# comparação byte a byte do save inteiro é feita na 2ª geração.
	var restored := _restore(save)
	var restored_save: Dictionary = restored.to_dict()
	var property_diff := _first_diff(_property_blocks(save),
		_property_blocks(restored_save))
	_check(property_diff == "",
		"(f) bloco `property` não é bit-exato no round-trip (%s)" % property_diff)
	var second := _restore(restored_save)
	_check(JSON.stringify(second.to_dict()) == JSON.stringify(restored_save),
		"(f) round-trip não é estável: 2ª geração difere (%s)" % _first_diff(
			restored_save, second.to_dict()))
	# Migração v22: sem o bloco `property` a região nasce INERTE e a alíquota 0;
	# o dado é reaplicado pelo GDScript (nada inventado para trás).
	var v22: Dictionary = save.duplicate(true)
	v22["version"] = 22
	for city: Dictionary in v22.get("cities", []):
		(city.get("region", {}) as Dictionary).erase("property")
	var migrated := _restore(v22)
	var inert := _property(migrated, city_id)
	_check(not bool(inert.get("ready", true)),
		"(f) v22 não deveria inventar patrimônio sem o dado")
	_check(float(inert.get("tax_rate", -1.0)) == 0.0,
		"(f) v22 deveria nascer com alíquota 0 (é %s)" % inert.get("tax_rate"))
	_check(LandPropertyScript.apply(migrated) > 0,
		"(f) reaplicar o dado no v22 deveria semear as regiões")
	var after: Dictionary = migrated.to_dict()
	_check(int(after.get("version", 0)) == SAVE_VERSION,
		"(f) v22 migrado deveria gravar v%d" % SAVE_VERSION)
	var reseeded := _property(migrated, city_id)
	_check(bool(reseeded.get("ready", false)),
		"(f) v22 reaplicado deveria ter patrimônio")
	_check(absf(float(reseeded.get("land_ha", 0.0))
			- float(property.get("land_ha", 0.0)))
			<= maxf(float(property.get("land_ha", 0.0)) * 1e-12, 1e-6),
		"(f) hectares do v22 reaplicado != dado (%.6f vs %.6f)" % [
			float(reseeded.get("land_ha", 0.0)),
			float(property.get("land_ha", 0.0))])
	_check(float(reseeded.get("tax_rate", -1.0)) == 0.0,
		"(f) v22 reaplicado deveria manter alíquota 0")

	# CADEIA v23→v24: a v23 já tinha o patrimônio (`property`) e ainda NÃO tinha
	# a agricultura. O save de um mundo COM safra em curso, gravado como v23
	# (bloco `agriculture` removido), tem de voltar v24 com o bloco `property`
	# BIT-EXATO sem tick e o estado da safra ZERADO: os planos voltam das
	# definições, mas nada de colheita/plantio é inventado para trás.
	var agri := _make_world()
	AgricultureScript.new().apply_to_sim(agri, _json(CITIES_PATH))
	for step in 31:
		agri.tick_day()
	var snapshot: Dictionary = agri.to_snapshot()
	var agri_save: Dictionary = snapshot.get("save", {})
	var agri_plans := Array((agri_save.get("agriculture", {}) as Dictionary)
		.get("plans", [])).size()
	var v23_snapshot: Dictionary = snapshot.duplicate(true)
	var v23_save: Dictionary = v23_snapshot.get("save", {})
	v23_save["version"] = 23
	v23_save.erase("agriculture")
	v23_snapshot["save"] = v23_save
	var chained := _restore_snapshot(v23_snapshot)
	var chained_save: Dictionary = chained.to_dict()
	_check(int(chained_save.get("version", 0)) == SAVE_VERSION,
		"(f) v23 deveria ser lido e gravado como v%d (é %s)" % [
			SAVE_VERSION, chained_save.get("version")])
	var chained_diff := _first_diff(_property_blocks(agri_save),
		_property_blocks(chained_save))
	_check(chained_diff == "",
		"(f) v23: bloco `property` não é bit-exato no round-trip (%s)" % chained_diff)
	var chained_status: Dictionary = chained.get_agriculture_status()
	_check(not chained_status.is_empty(),
		"(f) v23 deveria reaplicar os planos do bloco `definitions`")
	var zeroed: Dictionary = chained_save.get("agriculture", {}) as Dictionary
	var zeroed_plans := Array(zeroed.get("plans", [])).size()
	_check(zeroed_plans == 0,
		"(f) v23 não deveria inventar estado de safra (%d entradas)" % zeroed_plans)
	_check(int(zeroed.get("month_key", -1)) == 0,
		"(f) v23 deveria nascer com month_key 0 (é %s)" % str(
			zeroed.get("month_key")))
	var chained_first: Dictionary = chained.tick_day()
	var chained_violations: Array = chained_first.get("invariant_violations", [])
	_check(chained_violations.is_empty(),
		"(f) v23: invariante violado no 1º dia: %s" % str(
			chained_violations.slice(0, 1)))
	print("[b6] (f) save v24: round-trip bit-exato; v22→v23 inerte e reaplicado (%.0f ha); v23→v24 property bit-exato, safra zerada (origem %d planos com estado), 1º dia limpo." % [
		float(reseeded.get("land_ha", 0.0)), agri_plans])
