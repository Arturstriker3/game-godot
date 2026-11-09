extends Control
class_name UiChart

## Gráfico de linha de um ou mais séries já amostradas.
##
## O componente não conhece a simulação: recebe PackedFloat32Array prontos e
## apenas desenha grade, preenchimento, linhas e o marcador do último valor.

const GRID_COLOR := Color("#1B2723")
const AXIS_FONT_SIZE := 10

var _series: Array = []
var _baseline_zero := false


func _init(height: float = 72.0) -> void:
	custom_minimum_size = Vector2(0.0, height)
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	clip_contents = true


## Substitui as séries exibidas. Cada item aceita "values", "color" e "fill".
func set_series(series: Array) -> void:
	_series = series
	queue_redraw()


## Mantém o eixo Y começando em zero; útil para volumes e preços.
func set_baseline_zero(enabled: bool) -> void:
	_baseline_zero = enabled
	queue_redraw()


func _draw() -> void:
	if size.x <= 0.0 or size.y <= 0.0:
		return

	for index in 3:
		var y := size.y * (float(index) / 2.0)
		draw_line(Vector2(0.0, y), Vector2(size.x, y), GRID_COLOR, 1.0)

	var low := INF
	var high := -INF
	var has_data := false
	for entry in _series:
		var values: PackedFloat32Array = entry.get("values", PackedFloat32Array())
		if values.size() < 2:
			continue
		has_data = true
		for value in values:
			low = minf(low, value)
			high = maxf(high, value)

	if not has_data:
		draw_string(UiKit.CAPTION_FONT, Vector2(2.0, size.y * 0.5 + 4.0), "sem histórico",
				HORIZONTAL_ALIGNMENT_LEFT, -1.0, AXIS_FONT_SIZE, UiKit.TEXT_MUTED)
		return

	if _baseline_zero:
		low = minf(low, 0.0)
	if high - low < 1e-9:
		high = low + (1.0 if absf(low) < 1.0 else absf(low) * 0.1)
	var span := high - low
	var drawable := size.y - 4.0

	for entry in _series:
		var values: PackedFloat32Array = entry.get("values", PackedFloat32Array())
		if values.size() < 2:
			continue
		var color: Color = entry.get("color", UiKit.BRASS)
		var step := size.x / float(values.size() - 1)
		var points := PackedVector2Array()
		for index in values.size():
			var ratio := (values[index] - low) / span
			points.append(Vector2(step * float(index), size.y - 2.0 - ratio * drawable))
		if bool(entry.get("fill", true)):
			var area := points.duplicate()
			area.append(Vector2(size.x, size.y))
			area.append(Vector2(0.0, size.y))
			draw_colored_polygon(area, Color(color, 0.10))
		draw_polyline(points, color, 1.5, true)
		draw_circle(points[points.size() - 1], 2.2, color)
