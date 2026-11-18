extends SceneTree

## Gera os nomes FICTÍCIOS de banco de todas as nações (Parte C4).
##
## Por política de marcas, `banks.json` não guarda nome real de banco: guarda só
## o dado estrutural (existe banco estatal? qual o país de origem do capital
## estrangeiro?) com fonte. Os nomes exibidos vêm do MESMO sistema de nomes do
## jogo (`BusinessNameGenerator`, setor `finance`), com semente determinística
## por nação e papel (`int(abs(hash("<ISO3>:<slot>"))) % 100000`) e a capital do
## país como `{place}`. O banco estrangeiro é gerado uma vez por país de origem:
## as nações com a mesma origem recebem o mesmo nome (é o mesmo grupo).
##
## A saída vai para `tools/data/bank_shards/generated_names.json` e é aplicada
## ao dado por `tools/apply_bank_names.py`.
##
## Uso:
##   godot --headless --path . --script res://tools/gen_bank_names.gd

const CultureDb := preload("res://core/culture/culture_db.gd")
const BusinessNameGenerator := preload("res://core/culture/business_name_generator.gd")

const NATIONS_CSV := "res://data/definitions/world/out/nations.csv"
const BANKS_JSON := "res://data/definitions/economy/banks.json"
const OUT_DIR := "res://tools/data/bank_shards"
const OUT_PATH := OUT_DIR + "/generated_names.json"

const SECTOR := "finance"


func _initialize() -> void:
	var ok := true
	if not CultureDb.load_data():
		push_error("CultureDb não carregou")
		ok = false
	if not BusinessNameGenerator.load_data():
		push_error("BusinessNameGenerator não carregou")
		ok = false
	if not ok:
		quit(1)
		return

	var rows := _read_csv(NATIONS_CSV)
	var banks := _read_banks()
	var by_iso3 := _rows_by_iso3(rows)
	var generator := BusinessNameGenerator.new()
	var missing_culture: Array = []
	var no_name: Array = []
	var nations: Dictionary = {}
	var origins: Dictionary = {}

	var iso3_list: Array = by_iso3.keys()
	iso3_list.sort()
	for iso3: String in iso3_list:
		var row: Dictionary = by_iso3[iso3]
		var capital := String(row.get("capital", "")).strip_edges()
		var culture := CultureDb.national_culture(iso3)
		if culture.is_empty():
			missing_culture.append(iso3)
		var entry: Dictionary = {}
		for slot in ["public", "private"]:
			entry[slot] = _generate(generator, iso3, slot, culture, capital, missing_culture, no_name)
		# Estrangeiro: só nações com origem predominante declarada; o nome é do
		# grupo de origem, compartilhado por quem tem a mesma origem.
		var foreign_name: Variant = null
		var foreign: Dictionary = _foreign_block(banks, iso3)
		if String(foreign.get("status", "")) == "present":
			var origin := String(foreign.get("origin_iso3", "")).strip_edges()
			if not origin.is_empty():
				if not origins.has(origin):
					var origin_row: Dictionary = by_iso3.get(origin, {})
					var origin_capital := String(origin_row.get("capital", "")).strip_edges()
					var origin_culture := CultureDb.national_culture(origin)
					if origin_culture.is_empty() and not missing_culture.has(origin):
						missing_culture.append(origin)
					origins[origin] = _generate(
						generator, origin, "foreign", origin_culture, origin_capital,
						missing_culture, no_name
					)
				foreign_name = origins[origin]
		entry["foreign"] = foreign_name
		nations[iso3] = entry

	var payload := {
		"note": "Nomes FICTÍCIOS de banco gerados por BusinessNameGenerator (setor finance) a partir da cultura da nação e da capital. Não contém nome real; use tools/apply_bank_names.py para aplicar em banks.json.",
		"generated_at": Time.get_date_string_from_system(true),
		"generator": "core/culture/business_name_generator.gd",
		"sector": SECTOR,
		"seed_formula": "int(abs(hash(\"<ISO3>:<slot>\"))) % 100000",
		"nations": nations,
		"origins": origins,
	}

	DirAccess.make_dir_recursive_absolute(OUT_DIR)
	var file := FileAccess.open(OUT_PATH, FileAccess.WRITE)
	if file == null:
		push_error("não consegui escrever %s" % OUT_PATH)
		quit(1)
		return
	file.store_string(JSON.stringify(payload, "  ") + "\n")
	file.close()

	print("nations=%d missing_culture=%s no_name=%s used=%d" % [
		nations.size(), str(missing_culture), str(no_name), generator.used_count(),
	])
	quit(0)


## Gera um nome para (nó/papel) com a semente determinística do briefing e
## registra a ausência quando a cultura não existe ou o gerador falha.
func _generate(generator: BusinessNameGenerator, iso3: String, slot: String,
		culture: String, place: String, missing_culture: Array, no_name: Array) -> String:
	if culture.is_empty():
		if not missing_culture.has(iso3):
			missing_culture.append(iso3)
		no_name.append("%s:%s" % [iso3, slot])
		return ""
	var seed_value := int(absi(hash(iso3 + ":" + slot)) % 100000)
	var name := generator.generate(culture, SECTOR, place, seed_value)
	if name.is_empty():
		no_name.append("%s:%s" % [iso3, slot])
	return name


func _foreign_block(banks: Dictionary, iso3: String) -> Dictionary:
	var nations: Dictionary = banks.get("nations", {})
	var entry: Variant = nations.get(iso3, {})
	if typeof(entry) != TYPE_DICTIONARY:
		return {}
	var block: Variant = (entry as Dictionary).get("foreign", {})
	return block if typeof(block) == TYPE_DICTIONARY else {}


func _read_banks() -> Dictionary:
	var data: Variant = JSON.parse_string(FileAccess.get_file_as_string(BANKS_JSON))
	return data if typeof(data) == TYPE_DICTIONARY else {}


func _rows_by_iso3(rows: Array) -> Dictionary:
	var out: Dictionary = {}
	for row: Dictionary in rows:
		var iso3 := String(row.get("id", "")).strip_edges()
		if not iso3.is_empty():
			out[iso3] = row
	return out


func _read_csv(path: String) -> Array:
	var text := FileAccess.get_file_as_string(path)
	var lines := text.split("\n", false)
	if lines.is_empty():
		return []
	var header := _split_csv(lines[0])
	var rows: Array = []
	for index in range(1, lines.size()):
		var line := lines[index].strip_edges()
		if line.is_empty():
			continue
		var values := _split_csv(line)
		var row: Dictionary = {}
		for col in header.size():
			row[header[col]] = values[col] if col < values.size() else ""
		rows.append(row)
	return rows


func _split_csv(line: String) -> Array:
	var out: Array = []
	var current := ""
	var in_quotes := false
	for index in line.length():
		var ch := line[index]
		if ch == "\"":
			in_quotes = not in_quotes
			continue
		if ch == "," and not in_quotes:
			out.append(current)
			current = ""
			continue
		current += ch
	out.append(current)
	return out
