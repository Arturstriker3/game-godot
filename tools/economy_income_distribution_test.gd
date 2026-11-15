extends SceneTree

## F7 calibração — distribuição de renda por nação ancorada em dados reais.
##
## Verifica o contrato novo:
##  1. `income_distribution.json` cobre as 7 regiões do cenário e é TODO
##     `estimated` (nenhuma nação tem Gini observado no projeto);
##  2. o núcleo recebe o Gini por cidade (`demo_set_income_gini`) e o publica no
##     save (save/load preserva);
##  3. a fatia da POP em basic/life/luxury segue a meta qualitativa do head em
##     60 d com IA (aqui basta 1 tick: a classe é ancorada no PIB PPP e no Gini,
##     não na renda em XGS observada):
##       EUA/DEU  basic ≤ 15%, maioria life, luxury 25–40%;
##       BRA basic 30–45%; IND 60–75%; NGA 75–90%;
##  4. a TFP publicada é o FATOR COMPLETO `derived:gdp_ppp` (não prêmio):
##     EUA ~3,5 > DEU ~2,9 > BRA ~0,9 > IND ~0,46 > NGA ~0,37.
##
## Uso: godot --headless --path . --script res://tools/economy_income_distribution_test.gd

const GeoScript := preload("res://core/economy/geography_service.gd")

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"

## Meta do head (F7 calibração) por nação: [basic_min, basic_max,
## luxury_min, luxury_max]. `life` é o resto e precisa ser maioria nos ricos.
const META := {
	"USA": [0.0, 0.15, 0.25, 0.40],
	"DEU": [0.0, 0.15, 0.25, 0.40],
	"BRA": [0.30, 0.45, 0.0, 1.0],
	"IND": [0.60, 0.75, 0.0, 1.0],
	"NGA": [0.75, 0.90, 0.0, 1.0],
}
const FOCUS := ["USA", "DEU", "BRA", "IND", "NGA"]
const CITY_ID := "%s-CAP"
const POPULATION := 5_000_000.0

var _failures: Array[String] = []


func _init() -> void:
	_test_profile()
	_test_shares()
	_test_save_load()
	_test_productivity_order()
	if _failures.is_empty():
		print("[gini] OK: distribuição ancorada no PIB PPP + Gini estimado, save e TFP.")
		quit(0)
	else:
		print("[gini] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_DICTIONARY else {}


# --- 1. Perfil de Gini -------------------------------------------------------

func _test_profile() -> void:
	var geo = GeoScript.new()
	var profile: Dictionary = geo.gini_profile()
	var meta: Dictionary = profile.get("meta", {})
	if not bool(meta.get("available", false)):
		_fail("perfil: income_distribution.json indisponível (%s)" % meta.get(
			"reason", "?"))
		return
	if not bool(meta.get("estimated", false)):
		_fail("perfil: Gini sem dado por nação deveria ser marcado estimated")
	var regions: Dictionary = profile.get("by_region", {})
	if regions.size() != 7:
		_fail("perfil: %d regiões (esperado 7 do nations.json)" % regions.size())
	for region: String in regions:
		var value := float(regions[region])
		if value < 0.20 or value > 0.75:
			_fail("perfil: Gini de %s fora da faixa plausível (%.2f)" % [region, value])
	# Toda nação precisa resolver para uma região (nada de Gini 0 silencioso).
	var nations := _load_array("res://data/definitions/scenario/nations.json")
	var missing := 0
	for nation in nations:
		var entry: Dictionary = geo.nation_gini(String(nation.get("id", "")))
		if not bool(entry.get("available", false)):
			missing += 1
			if missing <= 3:
				_fail("perfil: nação %s sem região de Gini" % String(nation.get("id", "")))
	print("[gini] perfil: %d regiões, %d nações resolvidas, todo `estimated`" % [
		regions.size(), nations.size() - missing])
	print("[gini] regiões: %s" % str(regions))


# --- 2/3. Fatias por nação ---------------------------------------------------

func _new_world() -> Object:
	var geo = GeoScript.new()
	var cities: Array = []
	for tag: String in FOCUS:
		cities.append({
			"id": CITY_ID % tag,
			"name": tag + " Capital",
			"nation": tag,
			"home_nation": tag,
			"landmass": "testland",
			"population": POPULATION,
			"sim_population": POPULATION,
			"x": 100.0 + float(cities.size()) * 10.0,
			"y": 100.0,
		})
	var sim = EconomySim.new()
	sim.demo_set_goods(_load_array(GOODS_PATH))
	sim.demo_set_recipes(_load_array(RECIPES_PATH))
	sim.demo_set_needs(_load_dict(NEEDS_PATH))
	sim.initialize_demo_world(cities, [], [])
	for city in cities:
		var tag := String(city["nation"])
		sim.demo_set_external_income_index(String(city["id"]),
			geo.external_income_index(tag))
		var gini: Dictionary = geo.nation_gini(tag)
		sim.demo_set_income_gini(String(city["id"]), float(gini.get("gini", 0.0)))
	sim.tick_demo()
	return sim


func _shares_by_nation(sim: Object) -> Dictionary:
	var out := {}
	for city in sim.get_demo_status().get("cities", []):
		var pop: Dictionary = city.get("pop", {})
		var shares: Array = pop.get("income_level_share", [])
		if shares.size() < 4:
			_fail("%s: status sem income_level_share[4]" % String(city.get("id", "")))
			continue
		out[String(city.get("nation", ""))] = {
			"basic": float(shares[1]),
			"life": float(shares[2]),
			"luxury": float(shares[3]),
			"sum": float(shares[1]) + float(shares[2]) + float(shares[3]),
		}
	return out


func _test_shares() -> void:
	var sim = _new_world()
	var shares := _shares_by_nation(sim)
	print("[gini] fatias basic/life/luxury:")
	for tag: String in FOCUS:
		if not shares.has(tag):
			_fail("%s: sem fatias no status" % tag)
			continue
		var row: Dictionary = shares[tag]
		print("  %-4s %.1f%% / %.1f%% / %.1f%%" % [
			tag, float(row["basic"]) * 100.0, float(row["life"]) * 100.0,
			float(row["luxury"]) * 100.0])
		if absf(float(row["sum"]) - 1.0) > 1e-6:
			_fail("%s: fatias somam %.6f (esperado 1,0)" % [tag, float(row["sum"])])
		var meta: Array = META[tag]
		if float(row["basic"]) < float(meta[0]) - 1e-6 \
				or float(row["basic"]) > float(meta[1]) + 1e-6:
			_fail("%s: basic %.3f fora da meta [%.2f, %.2f]" % [
				tag, float(row["basic"]), float(meta[0]), float(meta[1])])
		if float(row["luxury"]) < float(meta[2]) - 1e-6 \
				or float(row["luxury"]) > float(meta[3]) + 1e-6:
			_fail("%s: luxury %.3f fora da meta [%.2f, %.2f]" % [
				tag, float(row["luxury"]), float(meta[2]), float(meta[3])])
	# Ricos: maioria life.
	for tag in ["USA", "DEU"]:
		if shares.has(tag) and float(shares[tag]["life"]) < 0.50:
			_fail("%s: life %.3f não é maioria (esperado >= 0,50)" % [
				tag, float(shares[tag]["life"])])
	# Ordem nos países pobres: basic cresce com a pobreza do país. (EUA × DEU
	# não entram: Gini menor na Europa compensa a renda menor, como nos dados.)
	var order := ["BRA", "IND", "NGA"]
	for index in range(1, order.size()):
		var previous := float(shares[order[index - 1]]["basic"])
		var current := float(shares[order[index]]["basic"])
		if current <= previous:
			_fail("ordem: basic de %s (%.3f) não supera %s (%.3f)" % [
				order[index], current, order[index - 1], previous])


# --- 4. Save/load ------------------------------------------------------------

func _test_save_load() -> void:
	var sim = _new_world()
	var save: Dictionary = sim.demo_to_dict()
	var geo = GeoScript.new()
	for city in save.get("cities", []):
		var tag := String(city.get("nation", ""))
		var expected := float(geo.nation_gini(tag).get("gini", 0.0))
		if absf(float(city.get("income_gini", -1.0)) - expected) > 1e-9:
			_fail("save: %s não gravou o Gini %.3f (%.3f)" % [
				tag, expected, float(city.get("income_gini", -1.0))])
	var loaded = EconomySim.new()
	loaded.demo_set_goods(_load_array(GOODS_PATH))
	loaded.demo_set_recipes(_load_array(RECIPES_PATH))
	loaded.demo_set_needs(_load_dict(NEEDS_PATH))
	loaded.demo_from_dict(save)
	# Save antigo (sem o campo) mantém a repartição legada (Gini 0), nunca 1,0.
	var legacy := save.duplicate(true)
	for city in legacy.get("cities", []):
		city.erase("income_gini")
	var restored = EconomySim.new()
	restored.demo_set_goods(_load_array(GOODS_PATH))
	restored.demo_set_recipes(_load_array(RECIPES_PATH))
	restored.demo_set_needs(_load_dict(NEEDS_PATH))
	restored.demo_from_dict(legacy)
	for city in restored.demo_to_dict().get("cities", []):
		if absf(float(city.get("income_gini", -1.0))) > 1e-9:
			_fail("save antigo: %s ganhou Gini %.3f do nada" % [
				String(city.get("nation", "")), float(city.get("income_gini", -1.0))])


# --- 5. TFP como fator completo ---------------------------------------------

func _test_productivity_order() -> void:
	var sim = _new_world()
	var values := {}
	for city in sim.get_demo_status().get("cities", []):
		values[String(city.get("nation", ""))] = float(
			city.get("productivity_index", 0.0))
	var expected := {
		"USA": 3.5, "DEU": 2.9, "BRA": 0.91, "IND": 0.46, "NGA": 0.37,
	}
	for tag: String in FOCUS:
		if not values.has(tag):
			_fail("TFP: %s ausente no status" % tag)
			continue
		if absf(float(values[tag]) - float(expected[tag])) > 0.05:
			_fail("TFP: %s %.3f != %.2f (derived:gdp_ppp)" % [
				tag, float(values[tag]), float(expected[tag])])
	# Fator completo: países pobres têm produtividade < 1 (não prêmio max(1,.)).
	if float(values.get("NGA", 1.0)) >= 1.0 or float(values.get("IND", 1.0)) >= 1.0:
		_fail("TFP: país abaixo da média mundial recebeu prêmio >= 1,0")
	print("[gini] TFP: USA %.2f DEU %.2f BRA %.2f IND %.2f NGA %.2f" % [
		float(values.get("USA", 0.0)), float(values.get("DEU", 0.0)),
		float(values.get("BRA", 0.0)), float(values.get("IND", 0.0)),
		float(values.get("NGA", 0.0))])
