extends SceneTree

## B1 — teste do padrão REAL da demografia por coortes no mundo real.
##
## Mesmo setup de `tools/economy_world_test.gd` (cities/ports/buildings + as
## definições de goods/recipes/needs/services/currencies/sector_employment e a
## estrutura etária antes de `initialize_world`, geografia real por cima), SEM
## IA e SEM a camada urbana: aqui se mede só a demografia do núcleo. O dado é
## `data/definitions/population/age_structure_2026.json` (WPP2024) e o estado
## observado vem de `get_city_status(id).pop.age_bands` (o status diário é leve
## e NÃO publica as coortes).
##
## B7: o fixture é ALIMENTO-SEGURO — abastece cada cidade com estoque de comida
## para ~400 dias no dia 0. Sem isso, a mortalidade B7 (que agora depende de
## `food`) converteria a fome estrutural pré-existente do mundo sem IA em ~12×
## o excesso de mortes esperado e quebraria a demografia B1 (ver nota em
## `FOOD_STOCK_DAYS` abaixo).
##
##   godot --headless --path . --script res://tools/population_age_test.gd
##
## Custo: duas varreduras pesadas (dia 0 e dia 365, 834 cidades) e 365 ticks.
## A cadência da demografia é mensal (A6, por cidade), então 1 ano ≈ 12
## fechamentos; por isso a taxa anual é medida como razão do dia 0 ao 365, com
## tolerância de 0,35 p.p. — o degrau mensal é parte do modelo.

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
## A mesma definição estática que o autoload carrega (`Economy.set_age_structure`).
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"

## Amostra de nações do contrato B1: duas com (cbr-cdr) negativo e cinco com
## (cbr-cdr) positivo, cobrindo pirâmides jovens e envelhecidas.
const SAMPLE_TAGS: Array[String] = ["JPN", "NGA", "BRA", "DEU", "USA", "IND", "AFG"]
## Ordem das coortes no núcleo (`kAgeBands`) e no JSON.
const BAND_KEYS: Array[String] = [
	"age_0_14", "age_15_24", "age_25_44", "age_45_64", "age_65_plus",
]
const BAND_65_PLUS := 4
const BAND_0_14 := 0

const DAYS := 365
## Fração de cada faixa no dia 0 vs. o JSON: as frações do arquivo têm 9 casas,
## então a única diferença possível é o resíduo do arredondamento.
const FRACTION_TOLERANCE := 2e-4
## Taxa anual medida vs. (cbr-cdr)/1000: cobre o degrau mensal da cadência
## (12 ou 13 fechamentos por cidade em 365 dias e a mistura entre elas).
const ANNUAL_RATE_TOLERANCE := 0.0035
## 4c — deslocamentos mínimos/máximos em fração da população (p.p. / 100).
const JPN_RETIRED_MIN_RISE := 0.002
const DEU_RETIRED_MIN_RISE := 0.001
## NGA: a coorte 0-14 cresce em absoluto (+0,6%/ano), mas o total cresce mais
## (+2,1%/ano), então a FRAÇÃO cai ~0,57 p.p. no primeiro ano. O limite mede
## "não colapsa" (o dado WPP tem TFR 4,56 e 40% em 0-14), não estabilidade da
## fração: 0,7 p.p. cobre os 12/13 fechamentos mensais por cidade.
const NGA_YOUNG_MAX_DROP := 0.007
## Sem IA e sem tocar mercado o dinheiro é conservado (o núcleo reverte
## qualquer tick que viole o invariante); o mesmo teto do economy_world_test.
const MONEY_DRIFT_TOLERANCE := 0.01
## B7 — FIÁVEL ALIMENTO-SEGURO: a mortalidade agora depende da satisfação
## alimentar (`food`). O mundo SEM IA tem fome estrutural pré-existente
## (satisfação ~0,01–0,66 no dia 365) que, sob a B7, vira ~12× o excesso de
## mortes esperado e quebra a demografia B1. Para isolar a B1, cada cidade
## recebe UMA vez no dia 0 estoque de `wheat` (fator calórico 1,0) para ~400
## dias de necessidade (`sim_population × kNeedPerCapita × 400`). A injeção
## cria BENS, não dinheiro — `money_total` segue conservado.
const FOOD_STOCK_DAYS := 400
const NEED_PER_CAPITA_T_PER_DAY := 0.0003
## Um mundo quebrado pode falhar em centenas de cidades: as primeiras mensagens
## bastam para diagnosticar e o total continua sendo contado.
const MAX_REPORTED_FAILURES := 40

var _failures: Array[String] = []
var _hidden_failures := 0


func _init() -> void:
	_test()
	if _failures.is_empty():
		print("[age] OK: demografia B1 validada em %d dias no cenario real (sem IA, sem urbano)." % DAYS)
		quit(0)
	else:
		print("[age] FALHAS (%d):" % (_failures.size() + _hidden_failures))
		for failure in _failures:
			print(" - " + failure)
		if _hidden_failures > 0:
			print(" - ... %d falhas nao listadas" % _hidden_failures)
		quit(1)


func _fail(msg: String) -> void:
	if _failures.size() < MAX_REPORTED_FAILURES:
		_failures.append(msg)
	else:
		_hidden_failures += 1


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("nao abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	if typeof(data) != TYPE_ARRAY:
		_fail(path + " nao e um array JSON")
		return []
	return data


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	if typeof(data) != TYPE_DICTIONARY:
		_fail(path + " nao e um objeto JSON")
		return {}
	return data


## Frações e taxas brutas por nação, na ordem de `BAND_KEYS`. Nações cujas
## faixas não fecham em 1 (±2%) são descartadas pelo núcleo; aqui só se lê o
## arquivo — inconsistência aparece como coorte zerada na varredura.
func _parse_age_structure(data: Dictionary) -> Dictionary:
	var nations: Dictionary = data.get("nations", {})
	var out: Dictionary = {}
	for tag: Variant in nations:
		var entry: Variant = nations[tag]
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		var bands: Dictionary = (entry as Dictionary).get("bands", {})
		var fractions: Array[float] = []
		var valid := true
		for key: String in BAND_KEYS:
			if not bands.has(key):
				valid = false
				break
			fractions.append(float(bands[key]))
		if not valid or fractions.size() != BAND_KEYS.size():
			continue
		out[String(tag)] = {
			"fractions": fractions,
			"cbr": float((entry as Dictionary).get("cbr_per_1000", 0.0)),
			"cdr": float((entry as Dictionary).get("cdr_per_1000", 0.0)),
		}
	return out


func _zero_bands() -> Array:
	var bands: Array = []
	for _index in BAND_KEYS.size():
		bands.append(0.0)
	return bands


func _sum_bands(bands: Array) -> float:
	var total := 0.0
	for value: Variant in bands:
		total += float(value)
	return total


## Fração da faixa no agregado da nação (bandas somadas, população somada).
func _band_share(bands: Array, index: int) -> float:
	var total := _sum_bands(bands)
	return float(bands[index]) / total if total > 0.0 else 0.0


## Cidades no formato que o GeographyService.apply_to_sim espera.
func _cities_with_center(cities: Array) -> Array:
	var result: Array = []
	for city: Variant in cities:
		result.append({
			"id": String((city as Dictionary).get("id", "")),
			"nation": String((city as Dictionary).get("nation", "")),
			"center": Vector2(float((city as Dictionary).get("x", 0.0)),
				float((city as Dictionary).get("y", 0.0))),
		})
	return result


## Bloco pesado de uma cidade: coortes publicadas + a população da POP. Não se
## inventa coorte: cidade sem idade publicada vira falha explícita.
func _read_city(sim: Object, city_id: String) -> Dictionary:
	var heavy: Dictionary = sim.get_city_status(city_id)
	if heavy.is_empty():
		_fail("%s: get_city_status vazio" % city_id)
		return {"ok": false}
	var pop: Dictionary = heavy.get("pop", {})
	if not pop.has("age_bands"):
		_fail("%s: pop sem age_bands (binario sem B1? rode o build debug)" % city_id)
		return {"ok": false}
	var raw: Variant = pop["age_bands"]
	if typeof(raw) != TYPE_ARRAY:
		_fail("%s: age_bands nao e Array" % city_id)
		return {"ok": false}
	var bands: Array = raw
	if bands.size() != BAND_KEYS.size():
		_fail("%s: age_bands com %d faixas (esperado %d)" % [
			city_id, bands.size(), BAND_KEYS.size()])
		return {"ok": false}
	for index in bands.size():
		if not is_finite(float(bands[index])):
			_fail("%s: age_bands[%d] nao finito" % [city_id, index])
			return {"ok": false}
	return {
		"ok": true,
		"bands": bands,
		"population": float(pop.get("population", 0.0)),
	}


## Dia 0: as coortes são as frações nacionais do JSON (o resíduo do
## arredondamento volta para 25-44 no núcleo, abaixo da tolerância).
func _check_fractions(city_id: String, bands: Array, total: float,
		fractions: Array, when: String) -> void:
	if total <= 0.0:
		_fail("%s: %s soma das coortes %.6f (sem dado etario)" % [
			city_id, when, total])
		return
	for index in bands.size():
		var measured := float(bands[index]) / total
		var expected := float(fractions[index])
		var delta := absf(measured - expected)
		if delta > FRACTION_TOLERANCE:
			_fail("%s: %s faixa %s = %.6f vs JSON %.6f (dif %.2e > %.0e)" % [
				city_id, when, BAND_KEYS[index], measured, expected, delta,
				FRACTION_TOLERANCE])


func _test() -> void:
	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	if cities.is_empty() or ports.is_empty() or buildings.is_empty():
		_fail("cenario vazio (cidades=%d, portos=%d, construcoes=%d)" % [
			cities.size(), ports.size(), buildings.size()])
		return
	var goods := _load_array(GOODS_PATH)
	if goods.is_empty():
		_fail("goods.json vazio")
		return
	var recipes := _load_array(RECIPES_PATH)
	var needs := _load_dict(NEEDS_PATH)
	var services := _load_dict(SERVICES_PATH)
	var currencies := _load_dict(CURRENCIES_PATH)
	var sectors := _load_dict(SECTOR_EMPLOYMENT_PATH)
	var age_data := _load_dict(AGE_STRUCTURE_PATH)
	var structures := _parse_age_structure(age_data)
	if structures.is_empty():
		_fail("sem estrutura etaria legivel em " + AGE_STRUCTURE_PATH)
		return

	var sim = EconomySim.new()
	if not sim.has_method("set_age_structure"):
		_fail("binario sem set_age_structure (B1) — rode o build debug")
		return
	# Definições ANTES de criar o mundo, como o autoload do jogo faz: as
	# construções semeiam com a receita correta e o preço local nasce na moeda
	# da nação. A estrutura etária é a última, para a semeadura das coortes em
	# `initialize_world` já enxergar o mapa por nação.
	sim.set_goods(goods)
	sim.set_recipes(recipes)
	sim.set_needs(needs)
	sim.set_services(services)
	sim.set_currencies(currencies)
	if not sectors.is_empty():
		sim.set_sector_employment(sectors)
	sim.set_age_structure(age_data)
	sim.initialize_world(cities, ports, buildings)
	# Geografia real por cima (aptidão, depósitos, renda externa/Gini): é o
	# mesmo mundo do jogo. Sem ela o teste mediria um cenário que não existe.
	var geo: Object = GeographyServiceScript.new()
	var applied: Dictionary = geo.apply_to_sim(sim, _cities_with_center(cities))
	if applied.is_empty():
		_fail("geografia: apply_to_sim vazio — setup diferente do jogo")

	# Cidade → nação e população de SIMULAÇÃO inicial (a escala das coortes é
	# `sim_population`, não a população exibida).
	var nation_of: Dictionary = {}
	var expected_population: Dictionary = {}
	for city: Variant in cities:
		var entry: Dictionary = city
		var city_id := String(entry.get("id", ""))
		nation_of[city_id] = String(entry.get("nation", ""))
		var real_population := float(entry.get("population", 0.0))
		var sim_population := real_population
		if entry.has("sim_population"):
			sim_population = float(entry.get("sim_population", real_population))
		expected_population[city_id] = sim_population if sim_population > 0.0 \
				else real_population

	# --- B7: abastece o mundo de comida (fixture alimento-seguro, ver nota no
	# topo). A injeção cria BENS, não dinheiro, então `money_total` segue
	# conservado — o teste B1 mede só a demografia, sem a fome estrutural.
	for city: Variant in cities:
		var cid := String((city as Dictionary).get("id", ""))
		var people := float(expected_population.get(cid, 0.0))
		if people > 0.0:
			sim.set_market_stock(cid, "wheat",
				people * NEED_PER_CAPITA_T_PER_DAY * FOOD_STOCK_DAYS)
	print("[age] fixture alimento-seguro: %d dias de wheat injetados por cidade (B7: mortalidade depende de food)." % FOOD_STOCK_DAYS)

	var sample: Dictionary = {}
	for tag: String in SAMPLE_TAGS:
		if not structures.has(tag):
			_fail("%s: sem estrutura etaria no %s" % [tag, AGE_STRUCTURE_PATH])
			continue
		var structure: Dictionary = structures[tag]
		sample[tag] = {
			"cbr": float(structure["cbr"]),
			"cdr": float(structure["cdr"]),
			"fractions": structure["fractions"],
			"pop0": 0.0,
			"pop365": 0.0,
			"bands0": _zero_bands(),
			"bands365": _zero_bands(),
		}

	var status: Dictionary = sim.get_status()
	var initial_day := int(status.get("day", -1))
	if initial_day != 0:
		_fail("dia inicial %d (esperado 0)" % initial_day)
	var money_initial := float(status.get("money_total", 0.0))

	# --- Save/load SEM tick: o derivado (students, dependents, coortes) tem
	# de fechar exato logo após o load, sem esperar o primeiro dia. O bug que
	# este bloco cobre: `students` era montado antes de `children` existir no
	# load v16 e divergia até o primeiro `labor_day`. ----------------------
	var save_before: Dictionary = sim.to_dict()
	var reload = EconomySim.new()
	reload.set_goods(goods)
	reload.set_recipes(recipes)
	reload.set_needs(needs)
	reload.set_services(services)
	reload.set_currencies(currencies)
	if not sectors.is_empty():
		reload.set_sector_employment(sectors)
	reload.set_age_structure(age_data)
	reload.from_dict(save_before)
	var save_after: Dictionary = reload.to_dict()
	var probe_id := ""
	for city: Variant in cities:
		var tag := String((city as Dictionary).get("nation", ""))
		if sample.has(tag):
			probe_id = String((city as Dictionary).get("id", ""))
			break
	var probe_index := -1
	var before_cities: Array = save_before.get("cities", [])
	for index in before_cities.size():
		if String((before_cities[index] as Dictionary).get("id", "")) == probe_id:
			probe_index = index
			break
	if probe_id == "" or probe_index < 0:
		_fail("round-trip sem tick: cidade-sonda nao encontrada")
	else:
		var before_pop: Dictionary = (before_cities[probe_index] as Dictionary).get(
				"pop", {})
		var after_pop: Dictionary = (save_after.get("cities", [])[probe_index]
				as Dictionary).get("pop", {})
		var pop_scale := maxf(1.0, float(before_pop.get("population", 0.0)))
		for key in ["population", "students", "dependents", "children",
				"working_age", "retirees", "recruit_reserve"]:
			if absf(float(before_pop.get(key, 0.0)) - float(after_pop.get(key, 0.0))) \
					> 1e-6 * pop_scale:
				_fail("round-trip sem tick: %s.%s diverge (%.6f vs %.6f)" % [
					probe_id, key, float(before_pop.get(key, 0.0)),
					float(after_pop.get(key, 0.0))])
		var bands_before: Array = before_pop.get("age_bands", [])
		var bands_after: Array = after_pop.get("age_bands", [])
		if bands_before.size() != 5 or bands_after.size() != 5:
			_fail("round-trip sem tick: %s sem 5 coortes" % probe_id)
		else:
			for band in 5:
				if absf(float(bands_before[band]) - float(bands_after[band])) \
						> 1e-6 * pop_scale:
					_fail("round-trip sem tick: %s.coorte%d diverge (%.6f vs %.6f)" % [
						probe_id, band, float(bands_before[band]),
						float(bands_after[band])])

	# --- Migração v15 -> v16: sem as coortes no save, elas são semeadas da
	# estrutura nacional e o derivado (children/students/reserva) fica
	# consistente. Modifica a cópia `save_after` (descartável). ------------
	save_after["version"] = 15
	for entry: Variant in save_after.get("cities", []):
		var legacy_pop: Dictionary = (entry as Dictionary).get("pop", {})
		legacy_pop.erase("age_bands")
		legacy_pop.erase("health_weight_ref")
		legacy_pop.erase("education_weight_ref")
	var migrated = EconomySim.new()
	migrated.set_goods(goods)
	migrated.set_recipes(recipes)
	migrated.set_needs(needs)
	migrated.set_services(services)
	migrated.set_currencies(currencies)
	if not sectors.is_empty():
		migrated.set_sector_employment(sectors)
	migrated.set_age_structure(age_data)
	migrated.from_dict(save_after)
	var migrated_pop: Dictionary = migrated.get_city_status(probe_id).get("pop", {})
	var migrated_bands: Array = migrated_pop.get("age_bands", [])
	if migrated_bands.size() != 5:
		_fail("migracao v15: %s sem 5 coortes apos o load" % probe_id)
	else:
		var migrated_total := 0.0
		for value: Variant in migrated_bands:
			migrated_total += float(value)
		if absf(migrated_total - float(migrated_pop.get("population", 0.0))) \
				> maxf(1.0, 1e-9 * maxf(migrated_total, 1.0)):
			_fail("migracao v15: soma das coortes %.6f != populacao %.6f" % [
				migrated_total, float(migrated_pop.get("population", 0.0))])
		if float(migrated_pop.get("students", 0.0)) \
				< float(migrated_bands[0]) * 0.999:
			_fail("migracao v15: students %.1f nao inclui a coorte 0-14 (%.1f)" % [
				float(migrated_pop.get("students", 0.0)), float(migrated_bands[0])])

	# --- Varredura do dia 0 (coortes semeadas por initialize_world) ---------
	var bands0: Dictionary = {}
	var world_population0 := 0.0
	var day0_sum_mismatch := 0
	for city: Variant in cities:
		var city_id := String((city as Dictionary).get("id", ""))
		var entry: Dictionary = _read_city(sim, city_id)
		if not bool(entry.get("ok", false)):
			continue
		var bands: Array = entry["bands"]
		var population := float(entry["population"])
		var total := _sum_bands(bands)
		bands0[city_id] = bands
		world_population0 += population
		if absf(total - population) > maxf(1.0, 1e-9 * maxf(population, 1.0)):
			day0_sum_mismatch += 1
			_fail("%s: dia 0 soma das coortes %.6f != pop.population %.6f" % [
				city_id, total, population])
		var expected := float(expected_population.get(city_id, 0.0))
		if absf(total - expected) > maxf(1.0, 1e-9 * maxf(expected, 1.0)):
			_fail("%s: dia 0 soma das coortes %.6f != sim_population %.6f do cities.json" % [
				city_id, total, expected])
		var tag := String(nation_of.get(city_id, ""))
		if not sample.has(tag):
			continue
		var row: Dictionary = sample[tag]
		if total > 0.0:
			_check_fractions(city_id, bands, total, row["fractions"], "dia 0")
		row["pop0"] = float(row["pop0"]) + population
		var accumulator: Array = row["bands0"]
		for index in bands.size():
			accumulator[index] = float(accumulator[index]) + float(bands[index])
	print("[age] dia 0: %d cidades, populacao de simulacao %.0f, dinheiro %.2f, coortes em cache %d" % [
		cities.size(), world_population0, money_initial, bands0.size()])
	if day0_sum_mismatch == 0:
		print("[age] dia 0: soma das coortes == pop.population em todas as cidades")

	# --- 365 dias, sem IA ----------------------------------------------------
	var violations := 0
	var first_violation := ""
	for day_index in DAYS:
		status = sim.tick_day()
		var day_violations: Array = status.get("invariant_violations", [])
		if day_violations.size() > 0:
			violations += 1
			if first_violation == "":
				first_violation = "dia %d: %s" % [
					day_index + 1, String(day_violations[0])]
	var money_final := float(status.get("money_total", 0.0))
	# D-126: o crédito de giro cria M1 registrado (`bank_money_delta_total`); a
	# conservação é contra o inicial + essa criação.
	var bank_created_total := float(status.get("bank_money_delta_total", 0.0))
	if violations > 0:
		print("[age] aviso: %d dias com invariante violado (revertido pelo nucleo); primeiro: %s" % [
			violations, first_violation])

	# --- Varredura do dia 365 -----------------------------------------------
	var world_population365 := 0.0
	var sum_mismatch := 0
	for city: Variant in cities:
		var city_id := String((city as Dictionary).get("id", ""))
		var entry: Dictionary = _read_city(sim, city_id)
		if not bool(entry.get("ok", false)):
			continue
		var bands: Array = entry["bands"]
		var population := float(entry["population"])
		var total := _sum_bands(bands)
		world_population365 += population
		# 4d: a população da POP É a soma das coortes em toda cidade do mundo.
		if absf(total - population) > 1e-9 * maxf(population, 1.0):
			sum_mismatch += 1
			_fail("%s: dia %d soma das coortes %.6f != pop.population %.6f" % [
				city_id, DAYS, total, population])
		var tag := String(nation_of.get(city_id, ""))
		if not sample.has(tag):
			continue
		var row: Dictionary = sample[tag]
		row["pop365"] = float(row["pop365"]) + population
		var accumulator: Array = row["bands365"]
		for index in bands.size():
			accumulator[index] = float(accumulator[index]) + float(bands[index])
	if sum_mismatch == 0:
		print("[age] dia %d: soma das coortes == pop.population em todas as cidades" % DAYS)

	# --- 4a/4b: sinal e taxa anual por nação da amostra ---------------------
	print("[age] nacao  pop_dia0        pop_dia365      delta%    65+ dia0/365   0-14 dia0/365")
	var rate_rows: Array[String] = []
	for tag: String in SAMPLE_TAGS:
		if not sample.has(tag):
			continue
		var row: Dictionary = sample[tag]
		var pop0 := float(row["pop0"])
		var pop365 := float(row["pop365"])
		var delta_percent := (pop365 / pop0 - 1.0) * 100.0 if pop0 > 0.0 else 0.0
		var retired_before := _band_share(row["bands0"], BAND_65_PLUS)
		var retired_after := _band_share(row["bands365"], BAND_65_PLUS)
		var young_before := _band_share(row["bands0"], BAND_0_14)
		var young_after := _band_share(row["bands365"], BAND_0_14)
		print("  %-4s %15.0f %15.0f %+8.2f%%   %6.2f%%/%6.2f%%  %6.2f%%/%6.2f%%" % [
			tag, pop0, pop365, delta_percent,
			retired_before * 100.0, retired_after * 100.0,
			young_before * 100.0, young_after * 100.0])
		if pop0 <= 0.0 or pop365 <= 0.0:
			_fail("%s: populacao agregada invalida (%.3f -> %.3f)" % [
				tag, pop0, pop365])
			continue
		var expected_rate := (float(row["cbr"]) - float(row["cdr"])) / 1000.0
		var measured_rate := pop365 / pop0 - 1.0
		# 4a: o sinal do crescimento agregado é o sinal de (cbr-cdr).
		if (measured_rate > 0.0) != (expected_rate > 0.0):
			_fail("%s: crescimento %.5f tem sinal diferente de (cbr-cdr)/1000 %.5f (cbr %.3f, cdr %.3f)" % [
				tag, measured_rate, expected_rate, float(row["cbr"]),
				float(row["cdr"])])
		# 4b: taxa anual do primeiro ano dentro de 0,35 p.p. do fluxo medido.
		if absf(measured_rate - expected_rate) > ANNUAL_RATE_TOLERANCE:
			_fail("%s: taxa anual %.5f vs (cbr-cdr)/1000 %.5f (dif %.5f > %.4f; cbr %.3f, cdr %.3f)" % [
				tag, measured_rate, expected_rate, absf(measured_rate - expected_rate),
				ANNUAL_RATE_TOLERANCE, float(row["cbr"]), float(row["cdr"])])
		rate_rows.append("  %-4s taxa %.4f%% vs esperado %.4f%% (dif %+.4f p.p.)" % [
			tag, measured_rate * 100.0, expected_rate * 100.0,
			(measured_rate - expected_rate) * 100.0])
	if not rate_rows.is_empty():
		print("[age] taxa anual do primeiro ano (medida vs (cbr-cdr)/1000):")
		for line in rate_rows:
			print(line)

	# --- 4c: composição — pirâmides envelhecida, jovem e estável -----------
	if sample.has("JPN"):
		var jpn: Dictionary = sample["JPN"]
		var before := _band_share(jpn["bands0"], BAND_65_PLUS)
		var after := _band_share(jpn["bands365"], BAND_65_PLUS)
		if after < before + JPN_RETIRED_MIN_RISE:
			_fail("JPN: fracao 65+ %.4f -> %.4f (esperado subir >= %.4f = 0,2 p.p.)" % [
				before, after, JPN_RETIRED_MIN_RISE])
	if sample.has("NGA"):
		var nga: Dictionary = sample["NGA"]
		var before := _band_share(nga["bands0"], BAND_0_14)
		var after := _band_share(nga["bands365"], BAND_0_14)
		if before - after > NGA_YOUNG_MAX_DROP:
			_fail("NGA: fracao 0-14 caiu %.2f p.p. (%.4f -> %.4f; limite %.2f p.p.)" % [
				(before - after) * 100.0, before, after,
				NGA_YOUNG_MAX_DROP * 100.0])
	if sample.has("DEU"):
		var deu: Dictionary = sample["DEU"]
		var before := _band_share(deu["bands0"], BAND_65_PLUS)
		var after := _band_share(deu["bands365"], BAND_65_PLUS)
		if after < before + DEU_RETIRED_MIN_RISE:
			_fail("DEU: fracao 65+ %.4f -> %.4f (esperado subir >= %.4f = 0,1 p.p.)" % [
				before, after, DEU_RETIRED_MIN_RISE])

	# --- 4e: mundo vivo e dinheiro intocado --------------------------------
	print("[age] mundo: populacao %.0f -> %.0f (delta %+.0f) | dinheiro %.2f -> %.2f (M1 criado %+.4f, drift %+.4f)" % [
		world_population0, world_population365,
		world_population365 - world_population0, money_initial, money_final,
		bank_created_total, money_final - money_initial - bank_created_total])
	if absf(world_population365 - world_population0) <= 0.0:
		_fail("populacao mundial nao mudou em %d dias (%.3f)" % [
			DAYS, world_population0])
	if absf(money_final - money_initial - bank_created_total) > MONEY_DRIFT_TOLERANCE:
		_fail("dinheiro nao conservado em %d dias: %.2f -> %.2f (M1 criado %.2f, drift %.4f > %.2f)" % [
			DAYS, money_initial, money_final, bank_created_total,
			money_final - money_initial - bank_created_total,
			MONEY_DRIFT_TOLERANCE])
