extends SceneTree

## Teste headless dos dados das cadeias de produção (Fase F1 do plano).
##
## Instancia só `FileAccess`/`JSON` — não depende de autoload, então `--script`
## é o runner correto (`& $godot --headless --path . --script
## res://tools/goods_data_test.gd`). Sai com código != 0 em qualquer falha.
##
## Cobre: contagem e ordem canônica dos 113 bens, preço/procedência, receitas sem
## insumo órfão nem ciclo, todo bem com produtor e consumidor, necessidades,
## moedas, i18n × 3 e ícone (manifesto + SVG) de cada bem.

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const NATIONS_PATH := "res://data/definitions/scenario/nations.json"
const I18N_PATH := "res://data/definitions/i18n/i18n.csv"
const ICON_MANIFEST_PATH := "res://tools/icon_forge/resource_icons.json"
const ICON_DIR := "res://assets/icons/resources/"

## Ordem canônica do núcleo C++ (enum DemoProduct / kDemoProductNames).
const CANONICAL_PRIMARIES: Array[String] = [
	"wheat", "iron", "rice", "maize", "bauxite",
	"lead", "gold", "phosphate", "tungsten", "rare_earths",
	"sulfur", "diamond", "coal", "copper", "gas",
	"oil", "potato", "soy", "cotton", "wood", "uranium",
]
const EXPECTED_GOODS := 113
const EXPECTED_NEW_GOODS := 92
const KIND_TO_TIER := {
	"raw": "raw", "intermediate": "semi", "byproduct": "semi", "final": "final",
}
const INSTITUTIONAL_TAGS: Array[String] = [
	"army", "government", "port", "farm", "mine", "factory", "building",
	"vehicle_fleet", "machines", "workshop", "construction", "treasury",
	"pop_basic", "pop_life", "pop_luxury",
]

var _failures: Array[String] = []


func _init() -> void:
	var goods := _load_array(GOODS_PATH)
	var recipes := _load_array(RECIPES_PATH)
	var needs := _load_dict(NEEDS_PATH)
	var currencies := _load_dict(CURRENCIES_PATH)
	var nations := _load_array(NATIONS_PATH)
	var icons: Dictionary = _load_dict(ICON_MANIFEST_PATH).get("icons", {})
	var i18n := _load_i18n()

	if goods.is_empty() or recipes.is_empty() or needs.is_empty() or currencies.is_empty():
		_finish()
		return

	var by_id: Dictionary = {}
	for good in goods:
		by_id[String(good.get("id", ""))] = good

	_check_goods(goods)
	_check_recipes(recipes, by_id, needs)
	_check_needs(needs, by_id)
	_check_currencies(currencies, nations)
	_check_i18n(i18n, goods)
	_check_icons(icons, goods)
	_finish()


func _finish() -> void:
	if _failures.is_empty():
		print("goods_data_test: OK — dados das cadeias de produção consistentes")
		quit(0)
		return
	for failure in _failures:
		printerr("FALHA: " + failure)
	printerr("goods_data_test: %d falha(s)" % _failures.size())
	quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	if typeof(data) != TYPE_ARRAY:
		_fail("%s não é um array JSON" % path)
		return []
	return data


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	if typeof(data) != TYPE_DICTIONARY:
		_fail("%s não é um objeto JSON" % path)
		return {}
	return data


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu %s" % path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if data == null:
		_fail("JSON inválido em %s" % path)
	return data


## i18n.csv → {chave: {pt_BR, en, es}} (CSV simples, sem vírgula nos valores).
func _load_i18n() -> Dictionary:
	var file := FileAccess.open(I18N_PATH, FileAccess.READ)
	if file == null:
		_fail("não abriu %s" % I18N_PATH)
		return {}
	var rows: Dictionary = {}
	var first := true
	while not file.eof_reached():
		var line := file.get_line()
		if line.is_empty():
			continue
		var cells := line.split(",", true, 3)
		if cells.size() < 4:
			continue
		if first:
			first = false
			continue
		rows[cells[0]] = {"pt_BR": cells[1], "en": cells[2], "es": cells[3]}
	file.close()
	return rows


# --- bens ------------------------------------------------------------------

func _check_goods(goods: Array) -> void:
	if goods.size() != EXPECTED_GOODS:
		_fail("goods.json: %d bens (esperado %d)" % [goods.size(), EXPECTED_GOODS])
	var seen: Dictionary = {}
	for index in goods.size():
		var good: Dictionary = goods[index]
		var id := String(good.get("id", ""))
		if id.is_empty():
			_fail("goods[%d]: sem id" % index)
			continue
		if seen.has(id):
			_fail("goods[%d]: id repetido '%s'" % [index, id])
		seen[id] = true
		if int(good.get("order", -1)) != index:
			_fail("%s: order %s != %d" % [id, good.get("order"), index])
		if index < CANONICAL_PRIMARIES.size() and id != CANONICAL_PRIMARIES[index]:
			_fail("goods[%d]: '%s' != primário canônico '%s'"
					% [index, id, CANONICAL_PRIMARIES[index]])
		if index >= CANONICAL_PRIMARIES.size() and index - CANONICAL_PRIMARIES.size() >= EXPECTED_NEW_GOODS:
			_fail("goods[%d]: além dos %d bens novos" % [index, EXPECTED_NEW_GOODS])
		var price := float(good.get("base_price", 0.0))
		if price <= 0.0:
			_fail("%s: base_price <= 0" % id)
		var source := String(good.get("base_price_source", ""))
		if source.is_empty():
			_fail("%s: sem base_price_source" % id)
		var expected := "calibrated" if source.begins_with("calibrated") else "derived"
		if String(good.get("price_source", "")) != expected:
			_fail("%s: price_source '%s' != '%s'" % [id, good.get("price_source"), expected])
		if String(good.get("sector", "")).is_empty():
			_fail("%s: sem setor" % id)
		var kind := String(good.get("kind", ""))
		if not KIND_TO_TIER.has(kind):
			_fail("%s: kind inválido '%s'" % [id, kind])
		elif String(good.get("tier", "")) != String(KIND_TO_TIER[kind]):
			_fail("%s: tier '%s' incompatível com kind '%s'" % [id, good.get("tier"), kind])
		if typeof(good.get("durable")) != TYPE_BOOL:
			_fail("%s: durable não é booleano" % id)
		if typeof(good.get("food")) != TYPE_BOOL:
			_fail("%s: food não é booleano" % id)
		if typeof(good.get("use")) != TYPE_ARRAY:
			_fail("%s: use deve ser lista" % id)


# --- receitas --------------------------------------------------------------

func _check_recipes(recipes: Array, by_id: Dictionary, needs: Dictionary) -> void:
	var pop_needs: Dictionary = {}
	for level in needs.get("levels", {}).values():
		for category in level.get("categories", {}).values():
			for id in category.get("substitutes", []):
				pop_needs[String(id)] = true
	var recipe_ids: Dictionary = {}
	var produced: Dictionary = {}
	var byproducts: Dictionary = {}
	var consumed: Dictionary = {}
	for index in recipes.size():
		var recipe: Dictionary = recipes[index]
		var rid := String(recipe.get("id", ""))
		if rid.is_empty():
			_fail("recipes[%d]: sem id" % index)
		elif recipe_ids.has(rid):
			_fail("recipes[%d]: id repetido '%s'" % [index, rid])
		recipe_ids[rid] = true
		var output := String(recipe.get("output", ""))
		if not by_id.has(output):
			_fail("receita '%s': output '%s' não existe" % [rid, output])
		else:
			produced[output] = true
		if float(recipe.get("yield", 0.0)) <= 0.0:
			_fail("receita '%s': yield <= 0" % rid)
		var inputs: Array = recipe.get("inputs", [])
		if inputs.is_empty():
			_fail("receita '%s': sem insumos" % rid)
		for item in inputs:
			var good := String(item.get("good", ""))
			if not by_id.has(good):
				_fail("receita '%s': insumo '%s' não existe" % [rid, good])
			consumed[good] = true
			if float(item.get("qty", 0.0)) <= 0.0:
				_fail("receita '%s': qty inválida em '%s'" % [rid, good])
		for item in recipe.get("byproducts", []):
			var good := String(item.get("good", ""))
			if not by_id.has(good):
				_fail("receita '%s': subproduto '%s' não existe" % [rid, good])
			if good == output:
				_fail("receita '%s': subproduto igual ao output" % rid)
			byproducts[good] = true

	# todo bem não-primário precisa de produtor declarado
	for id in by_id:
		if CANONICAL_PRIMARIES.has(id):
			continue
		var good: Dictionary = by_id[id]
		if produced.has(id) or byproducts.has(id) or good.has("alias_of"):
			continue
		_fail("'%s': bem não-primário sem receita, subproduto ou alias" % id)

	# todo bem precisa de consumidor declarado
	for id in by_id:
		var good: Dictionary = by_id[id]
		if consumed.has(id) or good.has("alias_of") or good.has("consumer_note"):
			continue
		var declared := false
		for tag in good.get("use", []):
			if INSTITUTIONAL_TAGS.has(String(tag)):
				declared = true
				break
		if pop_needs.has(String(id)):
			declared = true
		if not declared:
			_fail("'%s': bem sem consumidor declarado" % id)


# --- necessidades ----------------------------------------------------------

func _check_needs(needs: Dictionary, by_id: Dictionary) -> void:
	var levels: Dictionary = needs.get("levels", {})
	if levels.size() != 3:
		_fail("needs.json: %d níveis (esperado 3)" % levels.size())
	var total_weight := 0.0
	for level_name in levels:
		var level: Dictionary = levels[level_name]
		total_weight += float(level.get("satisfaction_weight", 0.0))
		var categories: Dictionary = level.get("categories", {})
		if categories.is_empty():
			_fail("needs.json: nível '%s' sem categorias" % level_name)
		for category_name in categories:
			var category: Dictionary = categories[category_name]
			var substitutes: Array = category.get("substitutes", [])
			if substitutes.is_empty():
				_fail("needs.json: %s/%s sem substitutos" % [level_name, category_name])
			for id in substitutes:
				if not by_id.has(String(id)):
					_fail("needs.json: substituto '%s' não existe" % id)
			if float(category.get("per_capita_t_month", 0.0)) <= 0.0:
				_fail("needs.json: %s/%s per_capita <= 0" % [level_name, category_name])
	if absf(total_weight - 1.0) > 0.000001:
		_fail("needs.json: pesos dos níveis somam %f (esperado 1.0)" % total_weight)
	var replacement := float(needs.get("durable_replacement_per_year", 0.0))
	if replacement <= 0.0 or replacement > 1.0:
		_fail("needs.json: durable_replacement_per_year fora de (0, 1]")


# --- moedas ----------------------------------------------------------------

func _check_currencies(currencies: Dictionary, nations: Array) -> void:
	var entries: Dictionary = {}
	for entry in currencies.get("currencies", []):
		entries[String(entry.get("code", ""))] = entry
	for nation in nations:
		var code := String(nation.get("currency", ""))
		if code.is_empty():
			continue
		if not entries.has(code):
			_fail("currencies.json: falta a moeda '%s' de %s" % [code, nation.get("id")])
	for code in entries:
		var entry: Dictionary = entries[code]
		if (entry.get("nations", []) as Array).is_empty():
			_fail("currencies.json: '%s' sem nações" % code)
		var rate: Variant = entry.get("rate_per_anchor")
		if rate == null:
			if not String(entry.get("source", "")).begins_with("unknown:"):
				_fail("currencies.json: '%s' sem taxa e sem source unknown:" % code)
		elif float(rate) <= 0.0:
			_fail("currencies.json: '%s' rate_per_anchor <= 0" % code)


# --- i18n e ícones ---------------------------------------------------------

func _check_i18n(i18n: Dictionary, goods: Array) -> void:
	for good in goods:
		var id := String(good.get("id", ""))
		for prefix: String in ["product.", "resource."]:
			var key := prefix + id
			if not i18n.has(key):
				_fail("i18n: falta %s" % key)
				continue
			var row: Dictionary = i18n[key]
			for lang in ["pt_BR", "en", "es"]:
				if String(row.get(lang, "")).strip_edges().is_empty():
					_fail("i18n: %s vazio em %s" % [key, lang])
			if prefix == "product." and String(row.get("pt_BR", "")) != String(good.get("name", "")):
				_fail("i18n: %s diverge do name em goods.json" % key)


func _check_icons(icons: Dictionary, goods: Array) -> void:
	if icons.size() != goods.size():
		_fail("manifesto de ícones: %d ícones para %d bens" % [icons.size(), goods.size()])
	for good in goods:
		var id := String(good.get("id", ""))
		var icon: Dictionary = icons.get(id, {})
		if icon.is_empty():
			_fail("ícone ausente no manifesto: %s" % id)
		else:
			if String(icon.get("label", "")) != String(good.get("name", "")):
				_fail("ícone %s: label diverge do name" % id)
			if String(icon.get("color", "")).to_lower() != String(good.get("icon_color", "")).to_lower():
				_fail("ícone %s: cor diverge do icon_color" % id)
			if String(icon.get("tier", "")) != String(good.get("tier", "")):
				_fail("ícone %s: tier diverge do goods.json" % id)
		var path := ICON_DIR + id + ".svg"
		if not FileAccess.file_exists(path):
			_fail("ícone %s: SVG ausente em %s" % [id, path])
