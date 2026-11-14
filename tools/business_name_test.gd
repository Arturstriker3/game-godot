extends Node

## Teste headless da base de nomes de empresas (Fase F8 — serviços).
##
## Cobre:
##  - mapa cultura → língua carregado e com as 4 línguas de amostra;
##  - 20 setores por língua, 10 nomes por setor em pt, en, ja e ar;
##  - zero duplicatas em 5.000 gerações (registro de nomes usados);
##  - determinismo: mesma instância/sequência e mesma seed → mesmo nome;
##  - cultura/setor inexistente devolve "" (ausência declarada);
##  - grafia nativa de ja/ar difere da romanizada.
##
## Uso (SEMPRE headless):
##   godot --headless --path . res://tools/business_name_test.tscn

const GeneratorScript: Script = preload("res://core/culture/business_name_generator.gd")

const SAMPLE_CULTURES: Array[String] = ["brasileiro", "britanico", "japones", "emiratense"]
const EXPECTED_LANGUAGE := {
	"brasileiro": "pt",
	"britanico": "en",
	"japones": "ja",
	"emiratense": "ar",
}
const NATIVE_CULTURES: Array[String] = ["japones", "emiratense"]
const PLACES: Array[String] = ["Lisboa", "Londres", "Tóquio", "Dubai",
		"São Paulo", "Nova York", "Osaka", "Cairo"]
const UNIQUE_TOTAL := 5000
const SAMPLES_PER_SECTOR := 2

var _failures: Array[String] = []


func _ready() -> void:
	_run()
	if _failures.is_empty():
		print("[business_names] OK: mapa, 20 setores, 10 nomes/setor, "
				+ "%d únicos em %d gerações, determinismo e grafia nativa."
				% [UNIQUE_TOTAL, UNIQUE_TOTAL])
		get_tree().quit(0)
	else:
		print("[business_names] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		get_tree().quit(1)


func _run() -> void:
	if not GeneratorScript.load_data(true):
		_failures.append("catálogo de nomes não carregou")
		return
	_check_mapping()
	_print_samples()
	_check_unique()
	_check_determinism()
	_check_absence()
	_check_native()


func _check_mapping() -> void:
	var codes: Array = GeneratorScript.language_codes()
	if codes.size() != 32:
		_failures.append("esperava 32 línguas, achei %d" % codes.size())
	for culture in EXPECTED_LANGUAGE:
		var got: String = GeneratorScript.language_for(culture)
		var expected: String = EXPECTED_LANGUAGE[culture]
		if got != expected:
			_failures.append("%s → %s (esperado %s)" % [culture, got, expected])


func _print_samples() -> void:
	var gen: RefCounted = GeneratorScript.new()
	for culture in SAMPLE_CULTURES:
		var code: String = GeneratorScript.language_for(culture)
		var sectors: Array = GeneratorScript.sectors(code)
		if sectors.size() != 20:
			_failures.append("%s (%s): %d setores (esperado 20)"
					% [culture, code, sectors.size()])
		print("[business_names] %s (%s) — %d setores:" % [culture, code, sectors.size()])
		for sector_value in sectors:
			var sector := String(sector_value)
			var names: Array[String] = []
			var seen := {}
			for index in 10:
				var place: String = PLACES[index % PLACES.size()]
				var name: String = gen.generate(culture, sector, place, index)
				if name.is_empty():
					_failures.append("nome vazio: %s/%s/%d" % [culture, sector, index])
					continue
				if seen.has(name.to_lower()):
					_failures.append("repetido em 10: %s/%s" % [culture, sector])
				seen[name.to_lower()] = true
				names.append(name)
			if names.size() != 10:
				_failures.append("%s/%s: %d nomes" % [culture, sector, names.size()])
			var sample: Array = names.slice(0, SAMPLES_PER_SECTOR)
			print("  %-24s %s" % [sector, " | ".join(sample)])


func _check_unique() -> void:
	var gen: RefCounted = GeneratorScript.new()
	var sectors: Array = GeneratorScript.sectors("pt")
	if sectors.is_empty():
		_failures.append("sem setores para medir unicidade")
		return
	var seen := {}
	var duplicates := 0
	var empty := 0
	for index in UNIQUE_TOTAL:
		var culture: String = SAMPLE_CULTURES[index % SAMPLE_CULTURES.size()]
		var sector := String(sectors[index % sectors.size()])
		var place: String = PLACES[(index / 3) % PLACES.size()]
		var name: String = gen.generate(culture, sector, place, index)
		if name.is_empty():
			empty += 1
			continue
		if seen.has(name.to_lower()):
			duplicates += 1
		else:
			seen[name.to_lower()] = true
	print("[business_names] unicidade: %d nomes, %d vazios, %d repetidos"
			% [seen.size(), empty, duplicates])
	if empty > 0:
		_failures.append("%d nomes vazios em %d gerações" % [empty, UNIQUE_TOTAL])
	if duplicates > 0:
		_failures.append("%d nomes repetidos em %d gerações"
				% [duplicates, UNIQUE_TOTAL])
	if seen.size() != UNIQUE_TOTAL:
		_failures.append("esperava %d únicos, obtive %d" % [UNIQUE_TOTAL, seen.size()])


func _check_determinism() -> void:
	var first: RefCounted = GeneratorScript.new()
	var second: RefCounted = GeneratorScript.new()
	for index in 60:
		var sector := String(GeneratorScript.sectors("pt")[index % 20])
		var a: String = first.generate("brasileiro", sector, "Lisboa", index)
		var b: String = second.generate("brasileiro", sector, "Lisboa", index)
		if a != b:
			_failures.append("não determinístico: %s != %s (semente %d)" % [a, b, index])
			break
	var lone: String = GeneratorScript.new().generate("japones", "retail", "Tóquio", 77)
	var lone_again: String = GeneratorScript.new().generate("japones", "retail", "Tóquio", 77)
	if lone != lone_again:
		_failures.append("mesma seed em instâncias novas divergiu: %s != %s"
				% [lone, lone_again])


func _check_absence() -> void:
	var gen: RefCounted = GeneratorScript.new()
	if not gen.generate("cultura_inexistente", "retail", "Lisboa", 1).is_empty():
		_failures.append("cultura inexistente deveria devolver vazio")
	if gen.last_error().is_empty():
		_failures.append("cultura inexistente sem erro declarado")
	if not gen.generate("brasileiro", "setor_inexistente", "Lisboa", 1).is_empty():
		_failures.append("setor inexistente deveria devolver vazio")


func _check_native() -> void:
	var gen: RefCounted = GeneratorScript.new()
	for culture in NATIVE_CULTURES:
		var roman: String = gen.generate(culture, "retail", "Centro", 5)
		var native: String = gen.generate_native(culture, "retail", "Centro", 5)
		if native.is_empty():
			_failures.append("%s: nome nativo vazio" % culture)
		if native == roman:
			_failures.append("%s: nativo igual ao romanizado (%s)" % [culture, native])
		print("[business_names] nativo %s: %s | %s" % [culture, roman, native])
	# Língua latina não tem bloco nativo: a grafia é a mesma.
	var pt_roman: String = gen.generate("brasileiro", "retail", "Recife", 9)
	var pt_native: String = gen.generate_native("brasileiro", "retail", "Recife", 9)
	if pt_roman != pt_native:
		_failures.append("pt: nativo deveria ser igual ao romanizado (%s != %s)"
				% [pt_roman, pt_native])
