extends RefCounted
class_name ProductIcons

## Ícones de produto/recurso em PNG (silhueta branca de emoji), gerados por
## `tools/generate_game_icons.py`. Tingir com `modulate`.

const DIR := "res://assets/icons/products/"
const FILES := {
	"wheat": "wheat.png",
	"flour": "flour.png",
	"bread": "bread.png",
	"iron": "iron.png",
	"factory": "factory.png",
	"crate": "crate.png",
	"coin": "coin.png",
	"oil": "oil.png",
	"sword": "sword.png",
	"rifle": "rifle.png",
	"truck": "truck.png",
	"ship": "ship.png",
	"plane": "plane.png",
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
