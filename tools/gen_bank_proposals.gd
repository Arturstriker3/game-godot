extends SceneTree

## Gera nomes propostos de banco público para nações sem banco estatal.
##
## Usa o MESMO sistema de nomes do jogo (`BusinessNameGenerator`, setor
## `finance`), com a semente que o `GroupNamer` usa para grupos econômicos
## (`absi(hash(id)) % 100000`) e a capital do país como `{place}`. Não é nome
## real: é proposta declarada como ficção, gravada no JSON de dados.
##
## Uso:
##   godot --headless --path . --script res://tools/gen_bank_proposals.gd

const CultureDb := preload("res://core/culture/culture_db.gd")
const BusinessNameGenerator := preload("res://core/culture/business_name_generator.gd")

const NATIONS_CSV := "res://data/definitions/world/out/nations.csv"
const OUT_PATH := "res://tools/data/bank_shards/proposals.json"


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
	var generator := BusinessNameGenerator.new()
	var out: Dictionary = {}
	for row: Dictionary in rows:
		var iso3 := String(row.get("id", ""))
		if iso3.is_empty():
			continue
		var capital := String(row.get("capital", ""))
		var culture := CultureDb.national_culture(iso3)
		var language := BusinessNameGenerator.language_for(culture)
		var name := ""
		if not culture.is_empty():
			name = generator.generate(culture, "finance", capital, absi(hash(iso3)) % 100000)
		out[iso3] = {
			"culture": culture,
			"language": language,
			"place": capital,
			"proposed_name": name,
			"error": generator.last_error() if name.is_empty() else "",
		}

	var payload := {
		"note": "Nomes FICTÍCIOS propostos para país sem banco público, gerados por BusinessNameGenerator (setor finance, semente absi(hash(iso3)) %% 100000, capital como place). Não são bancos reais.",
		"generated_at": Time.get_date_string_from_system(true),
		"generator": "core/culture/business_name_generator.gd",
		"sector": "finance",
		"nations": out,
	}
	var text := JSON.stringify(payload, "  ")
	var file := FileAccess.open(OUT_PATH, FileAccess.WRITE)
	if file == null:
		push_error("não consegui escrever %s" % OUT_PATH)
		quit(1)
		return
	file.store_string(text)
	file.close()

	var missing := 0
	for iso3: String in out:
		if String(out[iso3].get("proposed_name", "")).is_empty():
			missing += 1
	print("[proposals] nations=%d sem_nome=%d -> %s" % [out.size(), missing, OUT_PATH])
	quit(0)


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
