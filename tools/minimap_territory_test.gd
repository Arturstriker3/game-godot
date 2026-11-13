extends SceneTree

## Verifica o minimapa dinâmico (dono por província, mundo inteiro):
##  (a) a imagem é RGB8 512x256 (mesma resolução do minimap.png);
##  (b) cobre MUITO mais que as nações legadas — pelo menos 100 cores de dono
##      distintas em terra (o PNG estático só pintava GBR/FRA/DEU no tom);
##  (c) a cor no centro de nações de continentes diferentes bate com
##      `nation_color(tag) * 0,92` (a mesma cor do mapa político).
##
## Uso:
##   godot --headless --path . --script res://tools/minimap_territory_test.gd

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const MIN_COLORS := 100
## Centro de nações espalhadas pelo mundo (âncora do território, px do atlas).
const SAMPLES := ["FRA", "GBR", "DEU", "POL", "BRA", "USA", "IND", "CHN"]

var _failures: Array[String] = []


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	print("=== minimap_territory_test ===")
	var territory_script: Script = load("res://map/territory_map.gd")
	var territory = territory_script.new()
	territory.load_data()
	if not territory.is_loaded():
		_failures.append("TerritoryMap não carregou provinces.json")
		_finish()
		return

	var cities: Array = _load_array(CITIES_PATH)
	territory.assign(cities)
	territory.update(cities, {})
	var image: Image = territory.minimap_image()
	if image == null:
		_failures.append("minimap_image() devolveu null")
		_finish()
		return
	if image.get_width() != 512 or image.get_height() != 256:
		_failures.append("tamanho %dx%d != 512x256"
				% [image.get_width(), image.get_height()])
	if image.get_format() != Image.FORMAT_RGB8:
		_failures.append("formato %d != RGB8" % image.get_format())

	var colors: Dictionary = {}
	for y in image.get_height():
		for x in image.get_width():
			colors[image.get_pixel(x, y).to_html(false)] = true
	print("cores na imagem: %d" % colors.size())
	if colors.size() < MIN_COLORS:
		_failures.append("só %d cores no mundo inteiro (esperado >= %d)"
				% [colors.size(), MIN_COLORS])

	for tag: String in SAMPLES:
		var anchor: Dictionary = territory.nation_anchor(tag)
		if anchor.is_empty():
			_failures.append("%s sem âncora de território" % tag)
			continue
		var uv := Vector2(float(anchor["x"]) / 5000.0, float(anchor["y"]) / 2500.0)
		var px := Vector2i(clampi(int(uv.x * image.get_width()), 0,
				image.get_width() - 1), clampi(int(uv.y * image.get_height()), 0,
				image.get_height() - 1))
		var got := image.get_pixel(px.x, px.y)
		var expected: Color = territory.nation_color(tag) * 0.92
		var delta := maxf(absf(got.r - expected.r),
				maxf(absf(got.g - expected.g), absf(got.b - expected.b)))
		print("%s %s got=%s expected=%s Δ=%.3f"
				% [tag, px, got.to_html(false), expected.to_html(false), delta])
		if delta > 0.02:
			_failures.append("%s cor %s != %s (Δ%.3f)"
					% [tag, got.to_html(false), expected.to_html(false), delta])
	_finish()


func _load_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


func _finish() -> void:
	if _failures.is_empty():
		print("PASS: minimapa cobre todas as nações com a cor do mapa político")
		quit(0)
		return
	for failure in _failures:
		push_error("[minimap] " + failure)
	quit(1)
