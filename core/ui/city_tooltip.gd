extends PanelContainer
class_name CityTooltip

## Ficha pequena de papel que acompanha uma cidade sob o mouse.
##
## "Papelaria de guerra": papel claro (`PAPEL_CLARO`), borda de tinta de 1 px e
## sombra zero. O componente é burro de propósito — recebe título, bandeira,
## subtítulo e linhas já formatadas pelo HUD e só desenha. Nenhum valor é
## calculado aqui; o estado vazio honesto é uma linha ausente, nunca "0".
##
## Puramente apresentação: não capta mouse, não abre ficha e nunca é mostrado
## enquanto há sobreposição aberta (quem chama decide, ver `hud.gd`).

## Largura fixa da ficha (o suficiente para nome de cidade + coluna de valor).
const WIDTH := 248.0
## Distância entre o ponto da cidade e a borda esquerda da ficha.
const OFFSET := 16.0
## Folga mínima em relação às bordas da tela.
const MARGIN := 8.0
## Abaixo da barra superior (64 px) com um respiro.
const TOP_CLEARANCE := 74.0

const LABEL_COLOR := UiKit.TINTA_SUAVE
const VALUE_COLOR := UiKit.TINTA

var _title: Label
var _head: HBoxContainer
var _flag: Control
var _subtitle: Label
var _rows: VBoxContainer


func _init() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	visible = false
	custom_minimum_size = Vector2(WIDTH, 0.0)
	add_theme_stylebox_override("panel", _paper_style())

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 4)
	box.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(box)

	var head := HBoxContainer.new()
	head.add_theme_constant_override("separation", 6)
	head.mouse_filter = Control.MOUSE_FILTER_IGNORE
	box.add_child(head)
	_head = head

	_title = _label("", 15, VALUE_COLOR, UiKit.PAPER_DISPLAY_FONT)
	_title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(_title)

	_subtitle = _label("", 12, LABEL_COLOR, UiKit.PAPER_BODY_FONT)
	box.add_child(_subtitle)

	_rows = VBoxContainer.new()
	_rows.add_theme_constant_override("separation", 2)
	_rows.mouse_filter = Control.MOUSE_FILTER_IGNORE
	box.add_child(_rows)


## Ficha de papel: sem sombra (as fichas de tooltip não projetam sombra).
static func _paper_style() -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = UiKit.PAPEL_CLARO
	style.border_color = UiKit.TINTA
	style.set_border_width_all(1)
	style.set_corner_radius_all(0)
	style.shadow_size = 0
	style.content_margin_left = 9
	style.content_margin_right = 9
	style.content_margin_top = 7
	style.content_margin_bottom = 7
	return style


static func _label(text: String, size: int, color: Color, font: Font) -> Label:
	var node := Label.new()
	node.text = text
	node.add_theme_font_override("font", font)
	node.add_theme_font_size_override("font_size", size)
	node.add_theme_color_override("font_color", color)
	node.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return node


## Bandeira pequena (20×14) na proporção do país, com moldura de tinta de 1 px.
## Trocada a cada cidade: a ficha acompanha o cursor.
func _set_flag(texture: Texture2D) -> void:
	if _flag != null:
		_head.remove_child(_flag)
		_flag.queue_free()
		_flag = null
	if texture == null:
		return
	_flag = UiKit.flag_view(texture, Vector2(20.0, 14.0))
	_head.add_child(_flag)
	_head.move_child(_flag, 0)


## Conteúdo já formatado pelo HUD. `rows` é uma lista de
## `{"label", "value", "color"}`; linhas sem valor simplesmente não existem.
func show_content(title: String, flag: Texture2D, subtitle: String,
		rows: Array) -> void:
	_title.text = title
	_set_flag(flag)
	_subtitle.text = subtitle
	_subtitle.visible = not subtitle.is_empty()

	for child in _rows.get_children():
		_rows.remove_child(child)
		child.queue_free()

	for row in rows:
		var text := String(row.get("value", ""))
		if text.is_empty():
			continue
		var line := HBoxContainer.new()
		line.add_theme_constant_override("separation", 8)
		line.mouse_filter = Control.MOUSE_FILTER_IGNORE

		var key := _label(String(row.get("label", "")), 12, LABEL_COLOR,
				UiKit.PAPER_BODY_FONT)
		key.custom_minimum_size.x = 82.0
		line.add_child(key)

		var value := _label(text, 13, row.get("color", VALUE_COLOR),
				UiKit.PAPER_BODY_FONT)
		value.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		value.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
		line.add_child(value)
		_rows.add_child(line)

	reset_size()
	visible = true


## Reancora a ficha no ponto da cidade, virando de lado e subindo quando o
## ponto está perto da borda direita/inferior da tela.
func set_anchor_point(point: Vector2) -> void:
	reset_size()
	var view := get_viewport_rect().size
	var target := point + Vector2(OFFSET, -10.0)
	if target.x + size.x > view.x - MARGIN:
		target.x = point.x - size.x - OFFSET
	if target.y + size.y > view.y - MARGIN:
		target.y = view.y - size.y - MARGIN
	target.x = clampf(target.x, MARGIN, maxf(MARGIN, view.x - size.x - MARGIN))
	target.y = maxf(target.y, TOP_CLEARANCE)
	position = target


func hide_tooltip() -> void:
	visible = false
