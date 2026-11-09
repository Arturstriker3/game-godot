extends Control
class_name Minimap

## Minimapa político no canto inferior direito.
##
## Usa uma imagem estática gerada pelo mesmo pipeline do mapa
## (tools/build_atlas_layers.py -> minimap.png), sem segunda câmera nem
## re-render do mundo. Mostra a área vista pela câmera e, ao clicar, pede para
## a câmera ir até o ponto. Trabalha em UV do mapa (0..1); a conversão para o
## mundo fica com quem conhece a posição do mapa (main_game.gd).

signal point_requested(map_uv: Vector2)

const MAP_TEXTURE: Texture2D = preload("res://map/map_data/atlas/minimap.png")
const MINIMAP_SIZE := Vector2(208.0, 104.0)
const BORDER_WIDTH := 1.0

var _image: TextureRect
var _view: Panel
var _view_uv := Rect2(0.45, 0.2, 0.05, 0.05)


func _ready() -> void:
	custom_minimum_size = MINIMAP_SIZE + Vector2.ONE * BORDER_WIDTH * 2.0
	mouse_filter = Control.MOUSE_FILTER_STOP
	clip_contents = true

	var frame := Panel.new()
	frame.set_anchors_preset(Control.PRESET_FULL_RECT)
	frame.mouse_filter = Control.MOUSE_FILTER_IGNORE
	frame.add_theme_stylebox_override("panel", UiKit.flat(UiKit.BG_DEEP, UiKit.EDGE))
	add_child(frame)

	_image = TextureRect.new()
	_image.texture = MAP_TEXTURE
	_image.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_image.stretch_mode = TextureRect.STRETCH_SCALE
	_image.set_anchors_preset(Control.PRESET_FULL_RECT)
	_image.offset_left = BORDER_WIDTH
	_image.offset_top = BORDER_WIDTH
	_image.offset_right = -BORDER_WIDTH
	_image.offset_bottom = -BORDER_WIDTH
	_image.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_image)

	# Retângulo vazado: mostra a área da câmera sem esconder o mapa.
	_view = Panel.new()
	_view.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_view.add_theme_stylebox_override("panel", UiKit.outline(UiKit.BRASS))
	add_child(_view)
	_layout_view()


## Atualiza a área vista: centro e tamanho em UV do mapa.
func set_view(center_uv: Vector2, size_uv: Vector2) -> void:
	center_uv.x = fposmod(center_uv.x, 1.0)
	_view_uv = Rect2(center_uv - size_uv * 0.5, size_uv)
	_layout_view()


func _layout_view() -> void:
	if _view == null:
		return
	var origin := Vector2.ONE * BORDER_WIDTH
	var min_size := Vector2(6.0, 4.0)
	var rect_size := (_view_uv.size * MINIMAP_SIZE).max(min_size)
	var center := origin + (_view_uv.position + _view_uv.size * 0.5) * MINIMAP_SIZE
	_view.position = center - rect_size * 0.5
	_view.size = rect_size


func _gui_input(event: InputEvent) -> void:
	var pressed := event is InputEventMouseButton \
			and (event as InputEventMouseButton).button_index == MOUSE_BUTTON_LEFT \
			and (event as InputEventMouseButton).pressed
	var dragged := event is InputEventMouseMotion \
			and ((event as InputEventMouseMotion).button_mask & MOUSE_BUTTON_MASK_LEFT) != 0
	if not (pressed or dragged):
		return
	var local := (get_local_mouse_position() - Vector2.ONE * BORDER_WIDTH) / MINIMAP_SIZE
	point_requested.emit(local.clamp(Vector2.ZERO, Vector2.ONE))
	accept_event()
