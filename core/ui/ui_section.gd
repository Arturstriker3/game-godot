extends PanelContainer
class_name UiSection

## Cartão de seção reutilizável: cabeçalho em latão, corpo e linhas de métrica.

var body: VBoxContainer
var hint: Label


func _init(title_text: String = "", hint_text: String = "") -> void:
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL

	var style := UiKit.flat(UiKit.PANEL, UiKit.EDGE, 14.0)
	style.content_margin_top = 12.0
	style.content_margin_bottom = 12.0
	add_theme_stylebox_override("panel", style)

	body = VBoxContainer.new()
	body.add_theme_constant_override("separation", 7)
	add_child(body)

	if title_text.is_empty():
		return

	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", 8)
	header.add_child(UiKit.caption(title_text, UiKit.BRASS))
	header.add_child(UiKit.spacer())
	hint = UiKit.label(hint_text, 11, UiKit.TEXT_MUTED)
	header.add_child(hint)
	body.add_child(header)
	body.add_child(UiKit.rule())


## Linha rótulo/valor alinhada à direita, com retorno do valor para updates.
func add_metric(label_text: String, value_text: String, color: Color = UiKit.TEXT) -> Label:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)

	var caption_node := UiKit.label(label_text, 13, UiKit.TEXT_DIM)
	caption_node.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(caption_node)

	var value := UiKit.label(value_text, 14, color)
	value.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	value.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(value)

	body.add_child(row)
	return value


func add_content(node: Control) -> void:
	body.add_child(node)


func add_rule(color: Color = UiKit.EDGE_SOFT) -> void:
	body.add_child(UiKit.rule(color))


## Observação curta em cinza, usada para explicar a unidade dos números.
func add_note(text: String) -> void:
	body.add_child(UiKit.label(text, 11, UiKit.TEXT_MUTED))
