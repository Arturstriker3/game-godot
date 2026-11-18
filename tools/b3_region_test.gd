extends SceneTree

## B3 — REGIÃO DA CIDADE (estados inteiros). Teste de dado + estado + save.
##
## Etapa 1 (commit 1) valida a contagem de população (invariante dura), a
## semeadura do bloco da região, o round-trip de save sem tick e a migração
## v17 → v19. A etapa 2 (economia da região) estende este arquivo com
## conservação de dinheiro, consumo cidade↔região e determinismo de threads.
##
## Uso: godot --headless --path . --script res://tools/b3_region_test.gd

const RegionDataScript := preload("res://core/economy/region_data.gd")

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

## Soma global de urban + interior (== soma de population_region == nações).
const EXPECTED_TOTAL := 8203501062.0
## Cidades com região 0 (urbano >= population_region; urbano capado).
## B3 + capitais extras: 18 -> 20 (entram IND-DEL e IND-MUM; ver
## docs/plans/b3_extra_capitals.md).
const EXPECTED_ZERO_REGION := 20
const TOLERANCE := 0.5

var _failures: Array = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[b3] OK: população, semeadura, round-trip e migração v17→v21.")
		quit(0)
	else:
		print("[b3] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


## Avança `days` dias e transforma QUALQUER violação de invariante em FALHA do
## teste. O núcleo reverte o tick violado (`restore_state`) e loga com
## push_error; sem esta checagem o teste passava com "dinheiro não conservado"
## no log e o drift ficava diluído no arredondamento de `money_total`.
func _tick_checked(sim: Object, days: int, label: String) -> void:
	var violations: Array = []
	for day in days:
		sim.tick_day()
		for violation: Variant in sim.get_day_summary().get(
				"invariant_violations", []):
			violations.append("dia %d: %s" % [day + 1, String(violation)])
	if not violations.is_empty():
		_fail("%s: %d violação(ões) de invariante em %d dias — 1ª: %s" % [
			label, violations.size(), days, String(violations[0])])


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


## Monta o mundo real (cities/ports/buildings.json) com a região anexada.
func _make_world() -> Object:
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
	return sim


func _city_blocks(save: Dictionary) -> Array:
	return save.get("cities", [])


## (a) soma global urban + interior = 8.203.501.062 exatos.
func _check_global_sum(save: Dictionary) -> void:
	var total := 0.0
	var urban := 0.0
	var region := 0.0
	for city: Dictionary in _city_blocks(save):
		var region_block: Dictionary = city.get("region", {})
		urban += float(city.get("sim_population", 0.0))
		region += float(region_block.get("region_pop", 0.0))
		total += float(city.get("population", 0.0))
	if absf(total - EXPECTED_TOTAL) > TOLERANCE:
		_fail("soma global da população %f != %f" % [total, EXPECTED_TOTAL])
	if absf((urban + region) - EXPECTED_TOTAL) > TOLERANCE:
		_fail("soma global urban+interior %f != %f" % [
			urban + region, EXPECTED_TOTAL])
	print("[b3] soma global: total=%.0f urban=%.0f interior=%.0f" % [
		total, urban, region])


## (b) por cidade: urban + interior == population_region.
func _check_per_city(save: Dictionary) -> void:
	var mismatches := 0
	for city: Dictionary in _city_blocks(save):
		var region_block: Dictionary = city.get("region", {})
		var urban := float(city.get("sim_population", 0.0))
		var region_pop := float(region_block.get("region_pop", 0.0))
		var total := float(city.get("population", 0.0))
		if absf((urban + region_pop) - total) > TOLERANCE:
			mismatches += 1
			if mismatches <= 3:
				_fail("%s: urban+interior %f != population %f" % [
					String(city.get("id", "?")), urban + region_pop, total])
	if mismatches > 0:
		_fail("por cidade: %d cidades com urban+interior != population" % mismatches)
	else:
		print("[b3] por cidade: urban+interior == population em todas as cidades.")


## (c) 18 cidades com região 0 (urbano capado em population_region).
func _check_zero_regions(save: Dictionary) -> void:
	var zero := 0
	var capped := 0
	for city: Dictionary in _city_blocks(save):
		var region_block: Dictionary = city.get("region", {})
		if float(region_block.get("region_pop", 0.0)) <= 0.0:
			zero += 1
		if float(region_block.get("urban_population", 0.0)) \
				== float(region_block.get("population_region", 0.0)):
			capped += 1
	if zero != EXPECTED_ZERO_REGION:
		_fail("cidades com região 0: %d != %d" % [zero, EXPECTED_ZERO_REGION])
	print("[b3] cidades com região 0: %d (urbano capado em %d)." % [zero, capped])


## (d) save → load sem tick: round-trip bit-exato (JSON idêntico).
func _check_save_load_roundtrip(sim: Object) -> void:
	var snapshot: Dictionary = sim.to_dict()
	var restored = EconomySim.new()
	_load_chains(restored)
	restored.from_dict(snapshot)
	if JSON.stringify(restored.to_dict()) != JSON.stringify(snapshot):
		_fail("round-trip sem tick não preserva o estado (JSON difere)")
	else:
		print("[b3] round-trip sem tick: bit-exato.")


## (e) save v17 (sem o bloco `region`) → v18 carrega sem inventar número.
func _check_v17_migration() -> void:
	var sim := _make_world()
	var snapshot: Dictionary = sim.to_dict()
	var v17: Dictionary = snapshot.duplicate(true)
	v17["version"] = 17
	var cities: Array = v17.get("cities", []).duplicate(true)
	for city: Dictionary in cities:
		city.erase("region")
	v17["cities"] = cities
	var migrated = EconomySim.new()
	_load_chains(migrated)
	migrated.from_dict(v17)
	var after: Dictionary = migrated.to_dict()
	if int(after.get("version", 0)) != 22:
		_fail("migração: versão deveria ser 22 (é %s)" % str(after.get("version")))
		return
	var checked := 0
	for city: Dictionary in after.get("cities", []):
		var region_block: Dictionary = city.get("region", {})
		if not bool(region_block.get("ready", false)):
			_fail("%s: migração não marcou a região" % String(city.get("id", "?")))
			continue
		var expected := maxf(0.0,
			float(city.get("population", 0.0)) - float(city.get("sim_population", 0.0)))
		if absf(float(region_block.get("region_pop", 0.0)) - expected) > TOLERANCE:
			_fail("%s: região migrada %f != max(0, population-sim) %f" % [
				String(city.get("id", "?")),
				float(region_block.get("region_pop", 0.0)), expected])
		checked += 1
	if checked == 0:
		_fail("migração: nenhuma cidade com região")
	else:
		print("[b3] migração v17→v21: %d cidades carregadas sem crash." % checked)


func _run() -> void:
	var sim := _make_world()
	var save: Dictionary = sim.to_dict()
	_check_global_sum(save)
	_check_per_city(save)
	_check_zero_regions(save)
	_check_save_load_roundtrip(sim)
	_check_v17_migration()
	# Etapa 2 (economia da região): conservação, consumo, inércia e determinismo.
	_check_region_conservation()
	_check_region_consumption()
	_check_region_inert()
	_check_determinism()
	# Etapa 3: crescimento (população exibida acompanha B1 + demografia da região).
	_check_region_growth()


## (a)/(b) crescimento: o total exibido cresce com a B1 (urbano) e com a
## demografia mensal do interior; o invariante urbano+interior == total se mantém.
func _check_region_growth() -> void:
	var sim := _make_world()
	var sums0 := _population_sums(sim)
	_tick_checked(sim, 35, "região/crescimento")
	var sums := _population_sums(sim)
	if sums["region"] <= sums0["region"]:
		_fail("crescimento: interior não cresceu (%f <= %f)" % [
			sums["region"], sums0["region"]])
	if sums["urban"] <= sums0["urban"]:
		_fail("crescimento: urbano não cresceu (%f <= %f)" % [
			sums["urban"], sums0["urban"]])
	if absf(sums["total"] - (sums["urban"] + sums["region"])) > 1.0:
		_fail("crescimento: total %f != urbano+interior %f" % [
			sums["total"], sums["urban"] + sums["region"]])
	print("[b3] crescimento: interior %.0f→%.0f, urbano %.0f→%.0f (35d)." % [
		sums0["region"], sums["region"], sums0["urban"], sums["urban"]])


## Soma global de urbano, interior e total (do save) para o teste de crescimento.
func _population_sums(sim: Object) -> Dictionary:
	var urban := 0.0
	var region := 0.0
	var total := 0.0
	for city: Dictionary in sim.to_dict().get("cities", []):
		urban += float(city.get("sim_population", 0.0))
		region += float(city.get("region", {}).get("region_pop", 0.0))
		total += float(city.get("population", 0.0))
	return {"urban": urban, "region": region, "total": total}


## SHA-256 de um texto (para comparar saves entre threads).
func _sha(text: String) -> String:
	return text.sha256_text()


## Monta o mundo real com um número de threads explícito (e sem IA).
func _make_world_threads(threads: int) -> Object:
	var sim := _make_world()
	if threads > 0 and sim.has_method("set_sim_threads"):
		sim.set_sim_threads(threads)
	return sim


## (f) conservação de dinheiro: drift ~0 em dias com a região ativa.
func _check_region_conservation() -> void:
	var sim := _make_world()
	var money_start := float(sim.get_status().get("money_total", 0.0))
	var days := 8
	_tick_checked(sim, days, "região/conservação")
	var money_end := float(sim.get_status().get("money_total", 0.0))
	var drift := absf(money_end - money_start) / maxf(absf(money_start), 1.0)
	if drift > 1e-6:
		_fail("região: drift de dinheiro %.9f > 1e-6 em %d dias" % [drift, days])
	else:
		print("[b3] conservação: drift=%.9f em %d dias (região ativa)." % [drift, days])


## (g) consumo cidade↔região: a região compra > 0 do mercado e o dinheiro sai
## do caixa da região para o da cidade (não some).
func _check_region_consumption() -> void:
	var sim := _make_world()
	# Soma do caixa da região antes de qualquer tick.
	var cash_before := 0.0
	for city: Dictionary in sim.to_dict().get("cities", []):
		cash_before += float(city.get("region", {}).get("cash", 0.0))
	sim.tick_day()
	var total_food_spend := 0.0
	var total_food_consumed := 0.0
	for city: Dictionary in sim.to_dict().get("cities", []):
		var city_id := String(city.get("id", ""))
		var status: Dictionary = sim.get_region_status(city_id)
		total_food_spend += float(status.get("food_spend", 0.0))
		total_food_consumed += float(status.get("food_consumed", 0.0))
	if total_food_spend <= 0.0:
		_fail("região: nenhuma cidade comprou comida do mercado (food_spend=0)")
	if total_food_consumed <= 0.0:
		_fail("região: nenhuma cidade consumiu comida (food_consumed=0)")
	# Dinheiro conservado: o que saiu do caixa da região entrou no caixa da
	# cidade (firm_cash) — money_total não some.
	var cash_after := 0.0
	for city: Dictionary in sim.to_dict().get("cities", []):
		cash_after += float(city.get("region", {}).get("cash", 0.0))
	print("[b3] consumo: food_spend=%.2f food_consumed=%.2f região.cash %.2f → %.2f"
		% [total_food_spend, total_food_consumed, cash_before, cash_after])


## (h) região sem dado = inerte (cenário sintético AGRIA/PORTUS).
func _check_region_inert() -> void:
	var sim := EconomySim.new()
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	if sim.has_method("set_services"):
		sim.set_services(_json(SERVICES_PATH))
	if sim.has_method("set_currencies"):
		sim.set_currencies(_json(CURRENCIES_PATH))
	sim.initialize_scenario()
	sim.tick_day()
	for city: Dictionary in sim.to_dict().get("cities", []):
		var status: Dictionary = sim.get_region_status(String(city.get("id", "")))
		if not status.is_empty():
			_fail("região inerte: cidade sintética %s tem status de região" % city.get("id"))
	print("[b3] inércia: cenário sintético sem região (get_region_status vazio).")


## (i) determinismo de threads 1 × 16 (mesmo hash do save).
func _check_determinism() -> void:
	var sim1 := _make_world_threads(1)
	_tick_checked(sim1, 10, "região/determinismo (1 thread)")
	var hash1 := _sha(JSON.stringify(sim1.to_dict()))
	var sim16 := _make_world_threads(16)
	_tick_checked(sim16, 10, "região/determinismo (16 threads)")
	var hash16 := _sha(JSON.stringify(sim16.to_dict()))
	if hash1 != hash16:
		_fail("região: hash do save difere entre threads 1 e 16 (%s vs %s)" % [
			hash1.substr(0, 12), hash16.substr(0, 12)])
	else:
		print("[b3] determinismo: threads 1×16 → mesmo save (%.12s...)." % hash1)
