extends RefCounted
class_name ProductIcons

## Ícones de produto/recurso em SVG, gerados por
## `tools/generate_product_icons.py` (monocromáticos; tingir com `modulate`).
## Ainda não estão ligados às telas — este registro é o ponto de entrada.

const DIR := "res://assets/icons/products/"
const FILES := {
	"wheat": "wheat.svg",
	"flour": "flour.svg",
	"bread": "bread.svg",
	"iron": "iron.svg",
	"factory": "factory.svg",
	"crate": "crate.svg",
	"coin": "coin.svg",
	"oil": "oil.svg",
	"sword": "sword.svg",
	"rifle": "rifle.svg",
	"truck": "truck.svg",
	"ship": "ship.svg",
	"plane": "plane.svg",
}

static var _cache: Dictionary = {}


## Textura do ícone (ou null se o id não existir). Em cache.
static func texture(id: String) -> Texture2D:
	if _cache.has(id):
		return _cache[id]
	var file: String = FILES.get(id, "")
	if file.is_empty():
		return null
	var tex: Texture2D = load(DIR + file)
	if tex != null:
		_cache[id] = tex
	return tex


## Cria um TextureRect pronto para a UI (32×32, tingível).
static func make(id: String, color: Color, size: float = 32.0) -> TextureRect:
	var icon := TextureRect.new()
	icon.texture = texture(id)
	icon.modulate = color
	icon.custom_minimum_size = Vector2(size, size)
	icon.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	icon.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	icon.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return icon
