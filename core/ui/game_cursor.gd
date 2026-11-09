extends RefCounted
class_name GameCursor

## Cursor temático do jogo: seta (mapa), lupa (hover sobre cidade, indica "abrir
## dados"), mão aberta (hover sobre porto/construção, indica "pegar") e
## quatro setas (arrastar/pan). Imagens em assets/ui (32×32, com transparência).

const DEFAULT_PATH := "res://assets/ui/cursor_default.png"
const CITY_PATH := "res://assets/ui/cursor_loupe.png"
const PORT_PATH := "res://assets/ui/cursor_pointer.png"
## Reservado para o estado "segurando" (arrastar) — ainda não ligado.
const GRAB_PATH := "res://assets/ui/cursor_grab.png"
const DRAG_PATH := "res://assets/ui/cursor_drag.png"

const DEFAULT_HOTSPOT := Vector2(3, 2)
const CITY_HOTSPOT := Vector2(16, 16)
const PORT_HOTSPOT := Vector2(16, 20)
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
	var port_tex: Texture2D = load(PORT_PATH)
	var drag_tex: Texture2D = load(DRAG_PATH)
	if default_tex != null:
		Input.set_custom_mouse_cursor(default_tex, Input.CURSOR_ARROW, DEFAULT_HOTSPOT)
	if city_tex != null:
		Input.set_custom_mouse_cursor(city_tex, Input.CURSOR_HELP, CITY_HOTSPOT)
	if port_tex != null:
		Input.set_custom_mouse_cursor(port_tex, Input.CURSOR_POINTING_HAND, PORT_HOTSPOT)
	if drag_tex != null:
		Input.set_custom_mouse_cursor(drag_tex, Input.CURSOR_DRAG, DRAG_HOTSPOT)
	_set_shape(Input.CURSOR_ARROW)


## Estado do cursor: arrastando > cidade (lupa: abrir dados) > porto/construção
## (pointer de botão web: pegar/selecionar) > padrão.
static func set_state(dragging: bool, hover_kind: String = "") -> void:
	if dragging:
		_set_shape(Input.CURSOR_DRAG)
	elif hover_kind == "city":
		_set_shape(Input.CURSOR_HELP)
	elif hover_kind == "port":
		_set_shape(Input.CURSOR_POINTING_HAND)
	else:
		_set_shape(Input.CURSOR_ARROW)


## Compatibilidade: hover de cidade sem arrasto.
static func set_city_hover(active: bool) -> void:
	set_state(false, "city" if active else "")


static func _set_shape(shape: int) -> void:
	if shape == _shape:
		return
	_shape = shape
	Input.set_default_cursor_shape(shape)
