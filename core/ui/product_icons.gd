extends RefCounted
class_name ProductIcons

## Ícones de produto/recurso em SVG (silhueta branca), tingíveis via `modulate`.
## Matérias-primas (21) vêm do Icon Forge (arte própria): `assets/icons/resources/`.
## Manufaturados/herdados vêm do game-icons.net (CC BY 3.0): `assets/icons/products/`.

const DIR := "res://assets/icons/products/"
const RESOURCE_DIR := "res://assets/icons/resources/"
## As 21 matérias-primas geradas por tools/icon_forge (mesmo traço da casa).
const RESOURCES := [
	"coal", "copper", "gas", "iron", "oil",
	"bauxite", "lead", "gold", "phosphate", "tungsten",
	"rare_earths", "sulfur", "diamond", "uranium",
	"wheat", "rice", "maize", "potato", "soy", "cotton", "wood",
]
const FILES := {
	"factory": "factory.svg",
	"crate": "crate.svg",
	"coin": "coin.svg",
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
	var path := ""
	if RESOURCES.has(id):
		path = RESOURCE_DIR + id + ".svg"
	else:
		var file: String = FILES.get(id, "")
		if file.is_empty():
			return null
		path = DIR + file
	var tex: Texture2D = load(path)
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
