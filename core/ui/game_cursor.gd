extends RefCounted
class_name GameCursor

## Cursor temático do jogo: seta de mapa por padrão e anel de "inspecionar
## cidade" quando o mouse passa sobre uma cidade. As imagens ficam em
## assets/ui/ e são geradas offline (32×32, com transparência).

const DEFAULT_PATH := "res://assets/ui/cursor_default.png"
const CITY_PATH := "res://assets/ui/cursor_city.png"
const DEFAULT_HOTSPOT := Vector2(3, 2)
const CITY_HOTSPOT := Vector2(16, 16)

static var _installed := false


## Registra as imagens nos shapes do Godot. Idempotente.
static func install() -> void:
	if _installed:
		return
	_installed = true
	var default_tex: Texture2D = load(DEFAULT_PATH)
	var city_tex: Texture2D = load(CITY_PATH)
	if default_tex != null:
		Input.set_custom_mouse_cursor(default_tex, Input.CURSOR_ARROW, DEFAULT_HOTSPOT)
	if city_tex != null:
		Input.set_custom_mouse_cursor(city_tex, Input.CURSOR_HELP, CITY_HOTSPOT)
	Input.set_default_cursor_shape(Input.CURSOR_ARROW)


## Alterna entre a seta do mapa e o anel de cidade. O shape HELP é usado para
## não conflitar com o "pointing hand" dos botões da interface.
static func set_city_hover(active: bool) -> void:
	Input.set_default_cursor_shape(Input.CURSOR_HELP if active else Input.CURSOR_ARROW)
