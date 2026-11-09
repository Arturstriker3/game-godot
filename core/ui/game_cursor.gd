extends RefCounted
class_name GameCursor

## Cursor temático do jogo: seta (mapa), silhueta de cidade (hover) e quatro
## setas (arrastar/pan). Imagens em assets/ui (32×32, com transparência).

const DEFAULT_PATH := "res://assets/ui/cursor_default.png"
const CITY_PATH := "res://assets/ui/cursor_city.png"
const DRAG_PATH := "res://assets/ui/cursor_drag.png"

const DEFAULT_HOTSPOT := Vector2(3, 2)
const CITY_HOTSPOT := Vector2(16, 16)
const DRAG_HOTSPOT := Vector2(16, 16)

static var _installed := false
static var _shape := -1


## Registra as imagens nos shapes do Godot. Idempotente.
static func install() -> void:
	if _installed:
		return
	_installed = true
	var default_tex: Texture2D = load(DEFAULT_PATH)
	var city_tex: Texture2D = load(CITY_PATH)
	var drag_tex: Texture2D = load(DRAG_PATH)
	if default_tex != null:
		Input.set_custom_mouse_cursor(default_tex, Input.CURSOR_ARROW, DEFAULT_HOTSPOT)
	if city_tex != null:
		Input.set_custom_mouse_cursor(city_tex, Input.CURSOR_HELP, CITY_HOTSPOT)
	if drag_tex != null:
		Input.set_custom_mouse_cursor(drag_tex, Input.CURSOR_DRAG, DRAG_HOTSPOT)
	_set_shape(Input.CURSOR_ARROW)


## Estado do cursor: arrastando > sobre cidade > padrão.
static func set_state(dragging: bool, over_city: bool) -> void:
	if dragging:
		_set_shape(Input.CURSOR_DRAG)
	elif over_city:
		_set_shape(Input.CURSOR_HELP)
	else:
		_set_shape(Input.CURSOR_ARROW)


## Compatibilidade: hover de cidade sem arrasto.
static func set_city_hover(active: bool) -> void:
	set_state(false, active)


static func _set_shape(shape: int) -> void:
	if shape == _shape:
		return
	_shape = shape
	Input.set_default_cursor_shape(shape)
