extends RefCounted
class_name GameCursor

## Cursor temático do jogo: seta (mapa livre), mão apontando (cidade, porto e
## jazida selecionáveis — os mesmos itens que abrem ficha ao clique) e quatro
## setas (arrastar/pan). Imagens em assets/ui (32×32, com transparência).

const DEFAULT_PATH := "res://assets/ui/cursor_default.png"
const PORT_PATH := "res://assets/ui/cursor_pointer.png"
const DRAG_PATH := "res://assets/ui/cursor_drag.png"

const DEFAULT_HOTSPOT := Vector2(3, 2)
const PORT_HOTSPOT := Vector2(14, 3)
const DRAG_HOTSPOT := Vector2(16, 16)

static var _installed := false
static var _shape := -1


## Registra as imagens nos shapes do Godot. Idempotente.
static func install() -> void:
	if _installed:
		return
	_installed = true
	var default_tex: Texture2D = load(DEFAULT_PATH)
	var port_tex: Texture2D = load(PORT_PATH)
	var drag_tex: Texture2D = load(DRAG_PATH)
	if default_tex != null:
		Input.set_custom_mouse_cursor(default_tex, Input.CURSOR_ARROW, DEFAULT_HOTSPOT)
	if port_tex != null:
		Input.set_custom_mouse_cursor(port_tex, Input.CURSOR_POINTING_HAND, PORT_HOTSPOT)
	if drag_tex != null:
		Input.set_custom_mouse_cursor(drag_tex, Input.CURSOR_DRAG, DRAG_HOTSPOT)
	_set_shape(Input.CURSOR_ARROW)


## Estado do cursor: arrastando > item selecionável (mão) > seta.
## Cidade, porto e jazida abrem ficha/painel ao clique, então usam a mesma mão
## dos controles da interface; fora deles a seta do mapa volta.
static func set_state(dragging: bool, hover_kind: String = "") -> void:
	if dragging:
		_set_shape(Input.CURSOR_DRAG)
	elif hover_kind.is_empty():
		_set_shape(Input.CURSOR_ARROW)
	else:
		_set_shape(Input.CURSOR_POINTING_HAND)


## Compatibilidade: hover de cidade sem arrasto.
static func set_city_hover(active: bool) -> void:
	set_state(false, "city" if active else "")


static func _set_shape(shape: int) -> void:
	# O GUI do Godot sobrescreve o cursor global (o LineEdit de busca usa
	# CURSOR_IBEAM, um Control sob o mouse usa o próprio shape), mas
	# `Input.get_current_cursor_shape()` continua devolvendo o DEFAULT e não
	# revela essa sobreposição. Um cache que pulasse a reaplicação deixava a
	# seta "presa" na volta ao mapa. Reaplicar todo frame é barato (um setter
	# nativo) e garante a mão enquanto o mouse está sobre cidade/porto/jazida.
	_shape = shape
	Input.set_default_cursor_shape(shape)


## Shape pretendido pelo jogo (o último que o GameCursor aplicou). Em headless
## o DisplayServer não devolve cursor real (`get_current_cursor_shape()` fica 0),
## então o teste usa este valor como fonte da intenção e compara com o real
## sempre que o servidor souber respondê-lo.
static func current_shape() -> int:
	return _shape


## O DisplayServer desta sessão rastreia o shape pedido? Sob `--headless` não;
## o teste registra a limitação em vez de fingir que `Input` respondeu.
static func tracks_native_shape() -> bool:
	return DisplayServer.get_name() != "headless"

