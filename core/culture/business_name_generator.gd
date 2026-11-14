extends RefCounted
class_name BusinessNameGenerator

## Nomes de empresas de serviço por cultura/língua (Fase F8 — dados).
##
## A base fica em `data/definitions/culture/business_names/<code>.json` (uma
## língua por arquivo) e o mapa cultura → língua em `culture_language.json`.
## Culturas cuja língua não tem arquivo próprio usam a mais próxima, listada no
## mapa com o motivo; cultura fora do mapa devolve "" (a ausência é declarada,
## não inventada).
##
## `generate()` é determinístico para a mesma instância e a mesma sequência de
## chamadas: a semente vem de (cultura, setor, lugar, seed), e a única variação
## é o registro de nomes já usados (`_used`). Ao colidir, tenta outro padrão,
## depois acrescenta o lugar e, por fim, um número.
##
## Línguas de escrita não latina guardam a grafia nativa no campo `native`;
## `generate_native()` devolve o nome na escrita nativa e usa um registro
## separado, para não colidir com a versão romanizada.
##
## Sem autoload: crie uma instância (`BusinessNameGenerator.new()`). O catálogo
## é carregado uma vez em variáveis estáticas, como em `CultureDb`.

const BASE_DIR := "res://data/definitions/culture"
const MAP_PATH := BASE_DIR + "/culture_language.json"
const NAME_DIR := BASE_DIR + "/business_names"

const PLACEHOLDERS: Array[String] = [
	"type", "qualifier", "surname", "sector_word", "place", "legal",
]

static var _mapping: Dictionary = {}
static var _languages: Dictionary = {}
static var _default_language := "en"
static var _loaded := false

var _used: Dictionary = {}
var _used_native: Dictionary = {}
var _last_error := ""


# --- Catálogo ----------------------------------------------------------------

## Carrega mapa e arquivos de língua. `force` relê do disco.
static func load_data(force: bool = false) -> bool:
	if _loaded and not force:
		return true
	_loaded = true
	_mapping.clear()
	_languages.clear()
	_default_language = "en"
	var map_data: Variant = JSON.parse_string(_read_text(MAP_PATH))
	if typeof(map_data) != TYPE_DICTIONARY:
		push_warning("BusinessNameGenerator: culture_language.json ausente ou inválido")
		return false
	var map: Dictionary = map_data
	var cultures: Dictionary = map.get("cultures", {})
	_mapping = cultures.duplicate()
	_default_language = String(map.get("default", "en"))
	var files := DirAccess.get_files_at(NAME_DIR)
	for file_name: String in files:
		if not file_name.ends_with(".json"):
			continue
		var code := file_name.get_basename()
		var data: Variant = JSON.parse_string(_read_text(NAME_DIR + "/" + file_name))
		if typeof(data) != TYPE_DICTIONARY:
			push_warning("BusinessNameGenerator: %s não é um objeto JSON" % file_name)
			continue
		_languages[code] = data
	return not _languages.is_empty()


static func is_loaded() -> bool:
	return _loaded


## Língua de uma cultura ("" se a cultura não estiver no mapa).
static func language_for(culture_id: String) -> String:
	load_data()
	if not _mapping.has(culture_id):
		return ""
	var code := String(_mapping[culture_id])
	if _languages.has(code):
		return code
	if _languages.has(_default_language):
		return _default_language
	return ""


## Códigos de língua carregados, em ordem alfabética.
static func language_codes() -> Array:
	load_data()
	var codes: Array = _languages.keys()
	codes.sort()
	return codes


static func has_language(code: String) -> bool:
	load_data()
	return _languages.has(code)


## Setores de uma língua, na ordem do arquivo ([] se a língua não existe).
static func sectors(code: String) -> Array:
	load_data()
	var lang: Dictionary = _languages.get(code, {})
	var types: Dictionary = lang.get("types", {})
	return types.keys()


static func has_sector(code: String, sector: String) -> bool:
	return sectors(code).has(sector)


static func language_name(code: String) -> String:
	load_data()
	var lang: Dictionary = _languages.get(code, {})
	return String(lang.get("language", code))


# --- Geração -----------------------------------------------------------------

## Nome romanizado (escrita latina). Devolve "" se cultura/setor não existirem.
func generate(culture_id: String, sector: String, place_name: String = "",
		seed_value: int = 0) -> String:
	return _generate(culture_id, sector, place_name, seed_value, false)


## Nome na escrita nativa (para línguas não latinas; igual ao romanizado nas
## demais). Registro próprio, para não colidir com `generate`.
func generate_native(culture_id: String, sector: String, place_name: String = "",
		seed_value: int = 0) -> String:
	return _generate(culture_id, sector, place_name, seed_value, true)


## Esquece os nomes já usados (novo escopo de nomes, ex.: nova partida).
func reset() -> void:
	_used.clear()
	_used_native.clear()


func used_count() -> int:
	return _used.size()


func native_used_count() -> int:
	return _used_native.size()


## Última falha declarada (cultura/setor/colisão), "" quando deu certo.
func last_error() -> String:
	return _last_error


func _generate(culture_id: String, sector: String, place_name: String,
		seed_value: int, native: bool) -> String:
	_last_error = ""
	var code := language_for(culture_id)
	if code.is_empty():
		_last_error = "cultura sem língua: %s" % culture_id
		return ""
	var lang: Dictionary = _languages[code]
	var lang_types: Dictionary = lang.get("types", {})
	if not lang_types.has(sector):
		_last_error = "setor desconhecido: %s (%s)" % [sector, code]
		return ""
	var pools := _pools(lang, sector, native)
	var patterns: Array = lang.get("patterns", [])
	var place := place_name.strip_edges()
	var usable: Array = []
	for pattern: Variant in patterns:
		var text := String(pattern)
		if text.contains("{place}") and place.is_empty():
			continue
		usable.append(text)
	if usable.is_empty():
		_last_error = "nenhum padrão compatível: %s (%s)" % [sector, code]
		return ""

	var rng := RandomNumberGenerator.new()
	rng.seed = _seed_for(culture_id, sector, place, seed_value)
	var start := rng.randi_range(0, usable.size() - 1)
	var registry: Dictionary = _used_native if native else _used
	var first := ""
	for offset in usable.size():
		var pattern := String(usable[(start + offset) % usable.size()])
		var candidate := _fill(pattern, pools, place, rng)
		if candidate.is_empty():
			continue
		if first.is_empty():
			first = candidate
		if not registry.has(candidate.to_lower()):
			registry[candidate.to_lower()] = true
			return candidate

	# Colidiu em todos os padrões: acrescenta o lugar, depois um número.
	var base := first
	if base.is_empty():
		base = _fill(String(usable[start]), pools, place, rng)
	var with_place := base
	if not place.is_empty():
		with_place = "%s - %s" % [base, place]
	if not registry.has(with_place.to_lower()):
		registry[with_place.to_lower()] = true
		return with_place
	var index := 2
	while index < 10000:
		var numbered := "%s %d" % [with_place, index]
		if not registry.has(numbered.to_lower()):
			registry[numbered.to_lower()] = true
			return numbered
		index += 1

	_last_error = "não foi possível gerar nome único: %s (%s)" % [sector, code]
	return ""


## Conjunto de termos de um setor: `native` lê o bloco de grafia nativa quando
## existe; nos demais casos usa o romanizado.
func _pools(lang: Dictionary, sector: String, native: bool) -> Dictionary:
	var source: Dictionary = lang
	if native:
		var native_block: Dictionary = lang.get("native", {})
		if not native_block.is_empty():
			source = native_block
	var words: Dictionary = source.get("sector_words", {})
	var type_map: Dictionary = source.get("types", {})
	return {
		"type": type_map.get(sector, []),
		"sector_word": [String(words.get(sector, ""))],
		"qualifier": source.get("qualifiers", []),
		"surname": source.get("surnames", []),
		"legal": source.get("legal_suffixes", []),
	}


## Substitui cada `{placeholder}` pela escolha do rng. `{surname}` repetido no
## mesmo padrão recebe sobrenomes diferentes (sócios).
func _fill(pattern: String, pools: Dictionary, place_name: String,
		rng: RandomNumberGenerator) -> String:
	var out := ""
	var cursor := 0
	var used_surnames := {}
	while cursor < pattern.length():
		var open := pattern.find("{", cursor)
		if open == -1:
			out += pattern.substr(cursor)
			break
		out += pattern.substr(cursor, open - cursor)
		var close := pattern.find("}", open)
		if close == -1:
			out += pattern.substr(open)
			break
		var key := pattern.substr(open + 1, close - open - 1)
		out += _token(key, pools, place_name, rng, used_surnames)
		cursor = close + 1
	return _clean(out)


func _token(key: String, pools: Dictionary, place_name: String,
		rng: RandomNumberGenerator, used_surnames: Dictionary) -> String:
	if key == "place":
		return place_name
	if key == "surname":
		var surnames: Array = pools.get("surname", [])
		if surnames.is_empty():
			return ""
		if surnames.size() > used_surnames.size():
			var picked := ""
			for attempt in 8:
				picked = String(surnames[rng.randi_range(0, surnames.size() - 1)])
				if not used_surnames.has(picked):
					break
			used_surnames[picked] = true
			return picked
		return String(surnames[rng.randi_range(0, surnames.size() - 1)])
	var values: Array = pools.get(key, [])
	if values.is_empty():
		return ""
	return String(values[rng.randi_range(0, values.size() - 1)])


static func _clean(text: String) -> String:
	var result := text.strip_edges()
	while result.contains("  "):
		result = result.replace("  ", " ")
	return result


## FNV-1a de 32 bits sobre os argumentos. Só depende da chamada (não do registro),
## para que a mesma seed escolha o mesmo padrão — romanizado e nativo partem do
## mesmo sorteio e diferem só pela escrita.
static func _seed_for(culture_id: String, sector: String, place_name: String,
		seed_value: int) -> int:
	var text := "%s|%s|%s|%d" % [culture_id, sector, place_name, seed_value]
	var value := 0x811c9dc5
	for index in text.length():
		value = value ^ text.unicode_at(index)
		value = (value * 16777619) & 0xffffffff
	return value


static func _read_text(path: String) -> String:
	if not FileAccess.file_exists(path):
		return ""
	return FileAccess.get_file_as_string(path)
