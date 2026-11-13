extends RefCounted
class_name CultureDb

## Leitura somente do dado cultural do cenário (sem tocar na simulação).
##
## O conteúdo é publicado por `data/definitions/culture/` e revisado por outra
## tarefa; por isso tudo é lido **por nome de coluna**, nunca por posição, e
## nenhum id de cultura é presumido. O serviço apenas indexa o que existe:
##
## - `cultures.json` (catálogo: id, nomes pt/en/es, família, cor, `nation`,
##   `home_region`) com `cultures.csv` como alternativa quando o JSON não existe;
## - `nation_cultures.csv`: cultura principal (a nacional), aceitas e composição;
## - `city_cultures.csv`: mistura por cidade (até 5 + outras).
##
## Nada é derivado por regra de jogo: composições chegam da fonte e são
## devolvidas na ordem da fonte (maior participação primeiro, "outras" no fim).
## A cor hex é a da fonte; o nome sai no idioma ativo, ou no idioma pedido em
## `culture_name(id, locale)`. A fonte/ano fica nos dados, mas não é exposta:
## a interface mostra apenas a composição.

const CULTURE_DIR := "res://data/definitions/culture"
const CATALOG_JSON := CULTURE_DIR + "/cultures.json"
const CATALOG_CSV := CULTURE_DIR + "/cultures.csv"
const NATION_CSV := CULTURE_DIR + "/nation_cultures.csv"
const CITY_CSV := CULTURE_DIR + "/city_cultures.csv"

## Id agregado que fecha a composição; recebe rótulo i18n em vez do catálogo.
const OTHER_ID := "outras"

const LOCALE_SUFFIX := {"pt_BR": "pt", "pt": "pt", "en": "en", "es": "es"}

## Cor de emergência quando a fonte não traz cor (mesmo valor de `UiKit.TINTA`).
## Fica local para o serviço não depender da camada visual.
const INK_FALLBACK := Color("#2b2418")

static var _cultures: Dictionary = {}
static var _nations: Dictionary = {}
static var _cities: Dictionary = {}
static var _by_nation: Dictionary = {}
static var _loaded := false


# --- API ---------------------------------------------------------------------

## Carrega os três arquivos uma vez. `force` relê do disco (teste/reimportação).
static func load_data(force: bool = false) -> bool:
	if _loaded and not force:
		return true
	_cultures.clear()
	_nations.clear()
	_cities.clear()
	_by_nation.clear()
	_loaded = true
	_load_catalog()
	_load_nations()
	_load_cities()
	return not _cultures.is_empty()


static func is_loaded() -> bool:
	return _loaded


## Catálogo de uma cultura: {id, name (idioma ativo), family, color (hex),
## color_value (Color), nation (tag ou ""), home_region (Array[String])}.
## Cultura fora do catálogo devolve `{}`.
static func culture(id: String) -> Dictionary:
	load_data()
	var entry: Dictionary = _cultures.get(id, {})
	if entry.is_empty():
		return {}
	return {
		"id": id,
		"name": culture_name(id),
		"family": String(entry.get("family", "")),
		"color": String(entry.get("color", "")),
		"color_value": culture_color(id),
		"nation": String(entry.get("nation", "")),
		"home_region": (entry.get("home_region", []) as Array).duplicate(),
	}


## Id da cultura nacional de uma tag ("" se a nação não tem dado). É a chave
## cultura ↔ nação usada pela futura migração.
static func national_culture(tag: String) -> String:
	load_data()
	return String(_by_nation.get(tag, ""))


## Nome no idioma ativo (ou no `locale` pedido). Ids desconhecidos aparecem
## legíveis ("grandes_lagos" -> "Grandes lagos"); "outras" é traduzido.
static func culture_name(id: String, locale: String = "") -> String:
	load_data()
	if id.is_empty():
		return ""
	if id == OTHER_ID:
		var translated := _i18n("pop.culture.others", locale)
		return translated if not translated.is_empty() else "Outras"
	var entry: Dictionary = _cultures.get(id, {})
	if entry.is_empty():
		return _humanize(id)
	var suffix := _locale_suffix(locale)
	var candidates := ["name_" + suffix]
	for key in candidates:
		var value := String(entry.get(key, "")).strip_edges()
		if not value.is_empty():
			return value
	# Sem o idioma pedido, mantém o nome da fonte em pt (não inventa tradução).
	var fallback := String(entry.get("name_pt", "")).strip_edges()
	return fallback if not fallback.is_empty() else _humanize(id)


static func culture_color(id: String) -> Color:
	load_data()
	var entry: Dictionary = _cultures.get(id, {})
	var hex := String(entry.get("color", "")).strip_edges()
	if hex.is_empty():
		return INK_FALLBACK
	return Color.from_string(hex, INK_FALLBACK)


## Composição da nação, ordenada por participação; "outras" fecha a lista.
## Cada item: {id, name, color, color_value, share (0..1), percent, role}
## com role = "primary" | "accepted" | "".
static func nation_mix(tag: String) -> Array:
	load_data()
	var entry: Dictionary = _nations.get(tag, {})
	if entry.is_empty():
		return []
	return _decorate(entry.get("mix", []), String(entry.get("primary", "")),
			entry.get("accepted_lookup", {}))


## Mistura de uma cidade (mesmo formato de `nation_mix`, sem papéis: a fonte
## não declara principal nem aceitas por cidade).
static func city_mix(city_id: String) -> Array:
	load_data()
	var entry: Dictionary = _cities.get(city_id, {})
	if entry.is_empty():
		return []
	return _decorate(entry.get("mix", []), "", {})


## Cultura principal da nação (mesmo formato dos itens de `nation_mix`), com
## `share`/`percent` quando ela está na composição. `{}` se não há dado.
static func primary(tag: String) -> Dictionary:
	load_data()
	var entry: Dictionary = _nations.get(tag, {})
	if entry.is_empty():
		return {}
	var id := String(entry.get("primary", ""))
	if id.is_empty():
		return {}
	for item in nation_mix(tag):
		if String(item.get("id", "")) == id:
			return item
	var item := _decorate([{"id": id, "share": 0.0}], id, {})
	return item[0] if not item.is_empty() else {}


## Culturas aceitas declaradas pela fonte, na ordem do arquivo.
static func accepted(tag: String) -> Array:
	load_data()
	var entry: Dictionary = _nations.get(tag, {})
	var result: Array = []
	for id: Variant in entry.get("accepted", []):
		result.append(culture(String(id)))
	return result


static func has_nation(tag: String) -> bool:
	load_data()
	return _nations.has(tag)


static func has_city(city_id: String) -> bool:
	load_data()
	return _cities.has(city_id)


## Tags com composição publicada (ordem do arquivo).
static func nation_tags() -> Array:
	load_data()
	return _nations.keys()


## Ids com mistura publicada (ordem do arquivo).
static func city_ids() -> Array:
	load_data()
	return _cities.keys()


static func culture_count() -> int:
	load_data()
	return _cultures.size()


## Ids do catálogo na ordem de leitura (JSON/CSV).
static func culture_ids() -> Array:
	load_data()
	return _cultures.keys()


# --- Composição ---------------------------------------------------------------

## Aplica nome/cor/papel a cada item e ordena por participação, com "outras" no
## fim. A soma das participações não é reescalada: o que a fonte diz é o que a
## tela mostra.
static func _decorate(entries: Array, primary_id: String,
		accepted_lookup: Dictionary) -> Array:
	var locale := _active_locale()
	var ordered: Array = []
	var others: Array = []
	for entry: Variant in entries:
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		var item: Dictionary = entry
		var id := String(item.get("id", ""))
		if id.is_empty():
			continue
		var percent := float(item.get("percent", 0.0))
		var role := ""
		if not primary_id.is_empty() and id == primary_id:
			role = "primary"
		elif accepted_lookup.has(id):
			role = "accepted"
		var decorated := {
			"id": id,
			"name": culture_name(id, locale),
			"color": String((_cultures.get(id, {}) as Dictionary).get("color", "")),
			"color_value": culture_color(id),
			"share": clampf(percent / 100.0, 0.0, 1.0),
			"percent": percent,
			"role": role,
		}
		if id == OTHER_ID:
			others.append(decorated)
		else:
			ordered.append(decorated)
	ordered.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["percent"]) > float(b["percent"]))
	ordered.append_array(others)
	return ordered


# --- Carga --------------------------------------------------------------------

static func _load_catalog() -> void:
	var rows: Array = []
	if FileAccess.file_exists(CATALOG_JSON):
		rows = _read_json_array(CATALOG_JSON)
	if rows.is_empty() and FileAccess.file_exists(CATALOG_CSV):
		rows = _read_csv(CATALOG_CSV)
	for row: Variant in rows:
		if typeof(row) != TYPE_DICTIONARY:
			continue
		var entry: Dictionary = row
		var id := _text(entry, ["id", "culture"])
		if id.is_empty():
			continue
		var nation := _text(entry, ["nation"])
		var home: Array = []
		for region in _text(entry, ["home_region"]).split(";", false):
			var clean := region.strip_edges()
			if not clean.is_empty():
				home.append(clean)
		_cultures[id] = {
			"id": id,
			"name_pt": _text(entry, ["name_pt", "pt"]),
			"name_en": _text(entry, ["name_en", "en"]),
			"name_es": _text(entry, ["name_es", "es"]),
			"family": _text(entry, ["family", "familia"]),
			"nation": nation,
			"home_region": home,
			"color": _text(entry, ["color", "cor"]),
		}
		if not nation.is_empty() and not _by_nation.has(nation):
			_by_nation[nation] = id


static func _load_nations() -> void:
	for row: Variant in _read_csv(NATION_CSV):
		var entry: Dictionary = row
		var tag := _text(entry, ["nation", "tag", "id"])
		if tag.is_empty():
			continue
		var accepted_list: Array = []
		var accepted_lookup := {}
		for id in _text(entry, ["accepted_cultures", "accepted"]).split(";", false):
			var clean := id.strip_edges()
			if clean.is_empty():
				continue
			accepted_list.append(clean)
			accepted_lookup[clean] = true
		_nations[tag] = {
			"primary": _text(entry, ["primary_culture", "primary"]),
			"accepted": accepted_list,
			"accepted_lookup": accepted_lookup,
			"mix": _parse_composition(_text(entry, ["composition", "mix"])),
		}


static func _load_cities() -> void:
	for row: Variant in _read_csv(CITY_CSV):
		var entry: Dictionary = row
		var city_id := _text(entry, ["city_id", "id"])
		if city_id.is_empty():
			continue
		_cities[city_id] = {
			"mix": _parse_composition(_text(entry, ["cultures", "composition"])),
		}


## "russo:45;tatar:4" -> [{id, percent}]. Duplicatas ficam com a primeira
## ocorrência; valores fora de 0..100 não são corrigidos, só preservados.
static func _parse_composition(text: String) -> Array:
	var result: Array = []
	var seen := {}
	for chunk in text.split(";", false):
		var part := chunk.strip_edges()
		if part.is_empty():
			continue
		var colon := part.rfind(":")
		if colon <= 0:
			continue
		var id := part.substr(0, colon).strip_edges()
		var value := part.substr(colon + 1).strip_edges()
		if id.is_empty() or seen.has(id):
			continue
		seen[id] = true
		result.append({"id": id, "percent": float(value.to_float())})
	return result


# --- CSV/JSON -----------------------------------------------------------------

## CSV com aspas: campos como "Censo 2020 (NBS, municipal)" não podem ser
## partidos na vírgula. O cabeçalho vira o mapa de nomes de coluna.
static func _read_csv(path: String) -> Array:
	if not FileAccess.file_exists(path):
		push_warning("CultureDb: arquivo ausente %s" % path)
		return []
	var text := FileAccess.get_file_as_string(path)
	if text.is_empty():
		return []
	if text.begins_with("\ufeff"):
		text = text.substr(1)
	var grid := _split_csv(text)
	if grid.is_empty():
		return []
	var header: Array = []
	for name: Variant in grid[0]:
		header.append(String(name).strip_edges().to_lower())
	var rows: Array = []
	for index in range(1, grid.size()):
		var fields: Array = grid[index]
		var record := {}
		for column in header.size():
			record[header[column]] = String(fields[column]) if column < fields.size() else ""
		rows.append(record)
	return rows


static func _split_csv(text: String) -> Array:
	var rows: Array = []
	var fields: Array = []
	var field := ""
	var quoted := false
	var index := 0
	while index < text.length():
		var ch := text[index]
		if quoted:
			if ch == "\"":
				if index + 1 < text.length() and text[index + 1] == "\"":
					field += "\""
					index += 1
				else:
					quoted = false
			else:
				field += ch
		elif ch == "\"":
			quoted = true
		elif ch == ",":
			fields.append(field)
			field = ""
		elif ch == "\n":
			fields.append(field)
			rows.append(fields)
			fields = []
			field = ""
		elif ch != "\r":
			field += ch
		index += 1
	if not field.is_empty() or not fields.is_empty():
		fields.append(field)
		rows.append(fields)
	return rows


static func _read_json_array(path: String) -> Array:
	var data: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	if typeof(data) == TYPE_ARRAY:
		return data
	push_warning("CultureDb: %s não é um array JSON" % path)
	return []


static func _text(entry: Dictionary, keys: Array) -> String:
	for key: Variant in keys:
		if entry.has(key):
			return String(entry[key]).strip_edges()
	return ""


# --- Utilidades ---------------------------------------------------------------

static func _locale_suffix(locale: String = "") -> String:
	var code := locale
	if code.is_empty():
		code = _active_locale()
	return String(LOCALE_SUFFIX.get(code, "pt"))


static func _active_locale() -> String:
	var tree := Engine.get_main_loop() as SceneTree
	if tree == null:
		return "pt_BR"
	var i18n := tree.root.get_node_or_null("I18n")
	return String(i18n.get("locale")) if i18n != null else "pt_BR"


## Texto de uma chave no idioma pedido (ou no ativo). Lê a tabela do autoload
## sem trocar o idioma da partida.
static func _i18n(key: String, locale: String = "") -> String:
	var tree := Engine.get_main_loop() as SceneTree
	if tree == null:
		return ""
	var i18n := tree.root.get_node_or_null("I18n")
	if i18n == null:
		return ""
	var code := locale if not locale.is_empty() else String(i18n.get("locale"))
	var tables: Dictionary = i18n.get("STRINGS")
	var table: Dictionary = tables.get(code, {})
	var text := String(table.get(key, ""))
	if text.is_empty():
		var fallback: Dictionary = tables.get("pt_BR", {})
		text = String(fallback.get(key, ""))
	return "" if text == key else text


## "grandes_lagos" -> "Grandes lagos" (só quando o catálogo não tem o id).
static func _humanize(id: String) -> String:
	var text := id.replace("_", " ").strip_edges()
	return text.substr(0, 1).to_upper() + text.substr(1)
