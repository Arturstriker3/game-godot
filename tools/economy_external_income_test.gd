extends SceneTree

## Renda externa por PIB PPP per capita (balance_review §20).
##
## O antigo subsídio UNIFORME de 0,012/pessoa/dia era o resto do mundo pagando
## o mesmo à Noruega e ao Burundi. Agora a renda de serviços/exportações não
## modelados é proporcional ao PIB PPP per capita REAL da nação, normalizada
## para a média mundial ponderada por população continuar na escala de 0,012.
## Verifica:
## 1. perfil: índice = PIB PPP per capita / média mundial, com piso e
##    renormalização (média ponderada por população = 1,0 ± 1e-6);
## 2. nação sem PIB declarado usa a MEDIANA DA REGIÃO e é marcada `estimated`;
## 3. núcleo: external_income = população × 0,012 × min(índice, 1,0), pago por
##    foreign_cash (money_total conservado). O índice acima da média NÃO é pago:
##    a renda externa é PISO DE SUBSISTÊNCIA (F7 calibração), não repasse de
##    riqueza — o excedente tem de vir do valor adicionado medido na cidade;
## 4. save/load preserva o índice (nenhuma nação volta à média por acidente);
## 5. índice fora do intervalo é limitado a [0, 8] em vez de virar renda infinita.
##
## Uso: godot --headless --path . --script res://tools/economy_external_income_test.gd

const GeoScript := preload("res://core/economy/geography_service.gd")

const NATIONS_PATH := "res://data/definitions/scenario/nations.json"
## Espelha kDemoExternalIncomePerCapita em gdextension/src/economy_sim.cpp.
const BASE_PER_CAPITA := 0.012
## Espelha kDemoExternalIncomeIndexCap: acima da média mundial a renda externa
## é piso de subsistência e não cresce (F7 calibração).
const SUBSISTENCE_CAP := 1.0
const EPSILON := 1e-6

var _failures: Array[String] = []


func _init() -> void:
	_test_profile()
	_test_core_payment()
	_test_save_load()
	_test_clamp()

	if _failures.is_empty():
		print("[income] OK: renda externa proporcional ao PIB PPP, piso, conservação e save/load.")
		quit(0)
	else:
		print("[income] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


# --- 1/2. Perfil de renda ----------------------------------------------------

func _test_profile() -> void:
	var geo = GeoScript.new()
	var profile: Dictionary = geo.nation_income_profile()
	var meta: Dictionary = profile.get("meta", {})
	if not bool(meta.get("available", false)):
		_fail("perfil: nations.json sem PIB utilizável (%s)" % meta.get("reason", "?"))
		return
	var by_nation: Dictionary = profile.get("by_nation", {})
	var nations: Array = []
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(NATIONS_PATH))
	if typeof(parsed) == TYPE_ARRAY:
		nations = parsed
	if nations.is_empty():
		_fail("perfil: nations.json não abriu")
		return

	# Média mundial ponderada por população == 1,0 (recalculada no teste, sem
	# confiar no meta do serviço).
	var weight := 0.0
	var weighted := 0.0
	for nation in nations:
		var tag := String(nation.get("id", ""))
		if not by_nation.has(tag):
			_fail("perfil: nação %s sem índice" % tag)
			continue
		var population := float(nation.get("population", 0.0))
		weight += population
		weighted += float(by_nation[tag].get("index", 0.0)) * population
	if weight <= 0.0:
		_fail("perfil: população mundial zero")
		return
	var mean := weighted / weight
	if absf(mean - 1.0) > 1e-6:
		_fail("perfil: média ponderada %.9f != 1,0" % mean)

	# Ricas sem terra importam; pobres no piso.
	var sau := float(by_nation.get("SAU", {}).get("index", 0.0))
	var qat := float(by_nation.get("QAT", {}).get("index", 0.0))
	var isl := float(by_nation.get("ISL", {}).get("index", 0.0))
	var bdi := float(by_nation.get("BDI", {}).get("index", 0.0))
	if sau <= 1.5 or qat <= sau or isl <= 1.5:
		_fail("perfil: ricas sem terra sem índice alto (SAU=%.2f QAT=%.2f ISL=%.2f)" % [
			sau, qat, isl])
	if bdi > 0.12 or bdi <= 0.0:
		_fail("perfil: piso não aplicado ao Burundi (índice %.4f)" % bdi)
	if sau * BASE_PER_CAPITA <= bdi * BASE_PER_CAPITA * 10.0:
		_fail("perfil: renda de SAU (%.4f/pessoa/dia) não é 10× a de BDI (%.4f)" % [
			sau * BASE_PER_CAPITA, bdi * BASE_PER_CAPITA])

	# Nações sem PIB -> mediana regional + marcação `estimated`.
	var estimated: Array = meta.get("estimated_nations", [])
	if estimated.is_empty():
		_fail("perfil: nenhuma nação marcada como estimada (esperado sem PIB)")
	var checked := 0
	for tag: String in estimated:
		if not by_nation.has(tag):
			continue
		var entry: Dictionary = by_nation[tag]
		if not bool(entry.get("estimated", false)):
			_fail("perfil: %s usou mediana regional sem marcar estimated" % tag)
		var region := String(entry.get("region", ""))
		var medians: Dictionary = meta.get("region_median_ppp_per_capita", {})
		if medians.has(region):
			var expected := float(medians[region])
			if absf(float(entry.get("per_capita_ppp", 0.0)) - expected) > 1.0:
				_fail("perfil: %s não usou a mediana da região %s" % [tag, region])
		checked += 1
	if checked == 0:
		_fail("perfil: nenhuma nação estimada pôde ser conferida")
	print("[income] perfil: %d nações, %d estimadas, %d no piso; média ponderada %.9f" % [
		int(meta.get("nations", 0)), estimated.size(),
		(meta.get("floored_nations", []) as Array).size(), mean])
	print("[income] SAU=%.2fx QAT=%.2fx ISL=%.2fx BDI=%.3fx da média" % [sau, qat, isl, bdi])


# --- 3. Pagamento no núcleo --------------------------------------------------

func _two_city_world() -> Array:
	return [
		{"id": "SAU-RUH", "name": "Riyadh", "nation": "SAU", "home_nation": "SAU",
			"landmass": "asia", "population": 7_000_000.0, "x": 3100.0, "y": 900.0},
		{"id": "BDI-BJM", "name": "Bujumbura", "nation": "BDI", "home_nation": "BDI",
			"landmass": "africa", "population": 1_000_000.0, "x": 2800.0, "y": 1150.0},
	]


func _test_core_payment() -> void:
	var geo = GeoScript.new()
	var sim = EconomySim.new()
	sim.initialize_demo_world(_two_city_world(), [], [])
	var sau_index := geo.external_income_index("SAU")
	var bdi_index := geo.external_income_index("BDI")
	sim.demo_set_external_income_index("SAU-RUH", sau_index)
	sim.demo_set_external_income_index("BDI-BJM", bdi_index)

	var before: Dictionary = sim.get_demo_status()
	var money_before := float(before.get("money_total", 0.0))
	var foreign_before := float(before.get("foreign_cash", 0.0))
	var status: Dictionary = sim.tick_demo()

	var paid := 0.0
	# F7: a renda externa é piso de subsistência — o índice acima da média é
	# publicado no status, mas NÃO é pago (min(índice, SUBSISTENCE_CAP)).
	var expected_sau := 7_000_000.0 * BASE_PER_CAPITA \
			* minf(sau_index, SUBSISTENCE_CAP)
	var expected_bdi := 1_000_000.0 * BASE_PER_CAPITA \
			* minf(bdi_index, SUBSISTENCE_CAP)
	for city in status.get("cities", []):
		var city_id := String(city.get("id", ""))
		var pop: Dictionary = city.get("pop", {})
		var income := float(pop.get("external_income", -1.0))
		var expected := expected_sau if city_id == "SAU-RUH" else expected_bdi
		if income < 0.0:
			_fail("%s: status sem external_income" % city_id)
			continue
		if absf(income - expected) > maxf(1e-6, expected * EPSILON):
			_fail("%s: renda externa %.6f != esperado %.6f" % [city_id, income, expected])
		paid += income
		if absf(float(city.get("external_income_index", -1.0))
				- (sau_index if city_id == "SAU-RUH" else bdi_index)) > EPSILON:
			_fail("%s: status não expõe o índice aplicado" % city_id)

	# A renda paga por pessoa nunca passa do piso de subsistência (0,012).
	var sau_pc := 0.0
	for city in status.get("cities", []):
		if String(city.get("id", "")) == "SAU-RUH":
			sau_pc = float(city.get("pop", {}).get("external_income", 0.0)) / 7_000_000.0
	if sau_pc > BASE_PER_CAPITA * SUBSISTENCE_CAP + EPSILON:
		_fail("piso: SAU recebeu %.6f/pessoa/dia acima do teto %.6f" % [
			sau_pc, BASE_PER_CAPITA * SUBSISTENCE_CAP])

	# Conservação: o dinheiro saiu do setor externo, não foi criado.
	var money_after := float(status.get("money_total", 0.0))
	var foreign_after := float(status.get("foreign_cash", 0.0))
	if absf(money_after - money_before) > maxf(1e-4, absf(money_before) * 1e-9):
		_fail("conservação: money_total %.6f -> %.6f" % [money_before, money_after])
	if absf((foreign_before - foreign_after) - paid) > maxf(1e-4, paid * 1e-6):
		_fail("conservação: foreign_cash caiu %.6f, renda paga %.6f" % [
			foreign_before - foreign_after, paid])
	if paid <= 0.0:
		_fail("núcleo: nenhuma renda externa paga")
	var violations: Array = status.get("invariant_violations", [])
	if not violations.is_empty():
		_fail("invariantes violados: %s" % str(violations))
	print("[income] núcleo: SAU %.2f + BDI %.2f = %.2f/dia pagos por foreign_cash" % [
		expected_sau, expected_bdi, paid])


# --- 4. Save/load ------------------------------------------------------------

func _test_save_load() -> void:
	var sim = EconomySim.new()
	sim.initialize_demo_world(_two_city_world(), [], [])
	sim.demo_set_external_income_index("SAU-RUH", 2.5)
	sim.demo_set_external_income_index("BDI-BJM", 0.1)
	var saved: Dictionary = sim.demo_to_dict()

	var loaded = EconomySim.new()
	loaded.demo_from_dict(saved)
	var status: Dictionary = loaded.get_demo_status()
	var found := {}
	for city in status.get("cities", []):
		found[String(city.get("id", ""))] = float(city.get("external_income_index", -1.0))
	if absf(float(found.get("SAU-RUH", -1.0)) - 2.5) > EPSILON:
		_fail("save/load: índice de SAU não preservado (%.4f)" % float(
			found.get("SAU-RUH", -1.0)))
	if absf(float(found.get("BDI-BJM", -1.0)) - 0.1) > EPSILON:
		_fail("save/load: índice de BDI não preservado (%.4f)" % float(
			found.get("BDI-BJM", -1.0)))

	# Save antigo (sem o campo) volta à média mundial, nunca a zero.
	var legacy := saved.duplicate(true)
	for city in legacy.get("cities", []):
		city.erase("external_income_index")
	var restored = EconomySim.new()
	restored.demo_from_dict(legacy)
	for city in restored.get_demo_status().get("cities", []):
		if absf(float(city.get("external_income_index", -1.0)) - 1.0) > EPSILON:
			_fail("save antigo: %s não voltou à média 1,0" % String(city.get("id", "")))


# --- 5. Limite do dado -------------------------------------------------------

func _test_clamp() -> void:
	var sim = EconomySim.new()
	sim.initialize_demo_world(_two_city_world(), [], [])
	sim.demo_set_external_income_index("SAU-RUH", 99.0)
	sim.demo_set_external_income_index("BDI-BJM", -4.0)
	var found := {}
	for city in sim.get_demo_status().get("cities", []):
		found[String(city.get("id", ""))] = float(city.get("external_income_index", -1.0))
	if absf(float(found.get("SAU-RUH", -1.0)) - 8.0) > EPSILON:
		_fail("limite: índice 99 não foi limitado a 8 (%.3f)" % float(
			found.get("SAU-RUH", -1.0)))
	if absf(float(found.get("BDI-BJM", -1.0))) > EPSILON:
		_fail("limite: índice negativo virou %.3f" % float(found.get("BDI-BJM", -1.0)))
