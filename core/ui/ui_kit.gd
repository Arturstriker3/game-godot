extends RefCounted
class_name UiKit

## Tokens visuais e construtores de widgets do HUD.
##
## A linguagem é HOI3 modernizada: ardósia naval profunda, bordas de aço,
## latão apenas para estado ativo e números-chave, serifa de display nos títulos.

const BG_DEEP := Color("#0A1112")
const PANEL := Color("#101A18")
const PANEL_RAISED := Color("#16221F")
const PANEL_HOVER := Color("#1E2E29")
const EDGE := Color("#2C3B35")
const EDGE_SOFT := Color("#22302B")
const TEXT := Color("#E2DFD2")
const TEXT_DIM := Color("#9AA69B")
const TEXT_MUTED := Color("#7F8C82")
const BRASS := Color("#C9A24A")
const STEEL := Color("#6F8F8C")
const POSITIVE := Color("#7FA86B")
const NEGATIVE := Color("#D26A5F")
const WARNING := Color("#D08A3E")

const BODY_FONT: Font = preload("res://assets/fonts/BarlowCondensed-Regular.ttf")
const CAPTION_FONT: Font = preload("res://assets/fonts/BarlowCondensed-SemiBold.ttf")
const DISPLAY_BASE: Font = preload("res://assets/fonts/Cinzel-Variable.ttf")

static var _display_font: Font = null


## Cinzel no peso 600, criada uma única vez para todos os títulos.
static func display_font() -> Font:
	if _display_font == null:
		var variation := FontVariation.new()
		variation.base_font = DISPLAY_BASE
		variation.variation_opentype = {"wght": 600}
		_display_font = variation
	return _display_font

const NATION_COLORS := {
	"GBR": Color("#C74A4F"),
	"FRA": Color("#4A78C7"),
	"DEU": Color("#8A8F96"),
	"NED": Color("#D8833A"),
	"BEL": Color("#CBB64A"),
}


## Tema global: aplicado uma vez na raiz do HUD e herdado por todos os filhos.
static func build_theme() -> Theme:
	var theme := Theme.new()
	theme.default_font = BODY_FONT
	theme.default_font_size = 16

	theme.set_stylebox("panel", "Panel", flat(PANEL, EDGE))
	theme.set_stylebox("panel", "PanelContainer", flat(PANEL, EDGE))
	theme.set_stylebox("panel", "ScrollContainer", StyleBoxEmpty.new())

	theme.set_stylebox("normal", "Button", flat(PANEL_RAISED, EDGE, 6.0))
	theme.set_stylebox("hover", "Button", flat(PANEL_HOVER, STEEL, 6.0))
	theme.set_stylebox("pressed", "Button", flat(BG_DEEP, BRASS, 6.0))
	theme.set_stylebox("focus", "Button", outline(BRASS))
	theme.set_stylebox("disabled", "Button", flat(PANEL, EDGE_SOFT, 6.0))

	theme.set_color("font_color", "Button", TEXT)
	theme.set_color("font_hover_color", "Button", Color.WHITE)
	theme.set_color("font_pressed_color", "Button", BRASS)
	theme.set_color("font_focus_color", "Button", TEXT)
	theme.set_color("font_disabled_color", "Button", TEXT_MUTED)
	theme.set_font_size("font_size", "Button", 15)

	theme.set_color("font_color", "Label", TEXT)
	theme.set_color("font_separator_color", "HSeparator", EDGE)

	# Barras de rolagem e tooltips também pertencem à paleta.
	theme.set_stylebox("scroll", "VScrollBar", flat(BG_DEEP, EDGE_SOFT))
	theme.set_stylebox("grabber", "VScrollBar", flat(EDGE, EDGE))
	theme.set_stylebox("grabber_highlight", "VScrollBar", flat(STEEL, STEEL))
	theme.set_stylebox("grabber_pressed", "VScrollBar", flat(BRASS, BRASS))
	theme.set_stylebox("panel", "TooltipPanel", flat(PANEL_RAISED, EDGE, 8.0))
	theme.set_color("font_color", "TooltipLabel", TEXT)

	return theme


## Caixa chapada com borda de 1 px.
static func flat(fill: Color, border: Color, padding: float = 0.0) -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = fill
	style.border_color = border
	style.set_border_width_all(1)
	style.set_corner_radius_all(2)
	if padding > 0.0:
		style.content_margin_left = padding
		style.content_margin_right = padding
		style.content_margin_top = padding * 0.5
		style.content_margin_bottom = padding * 0.5
	return style


## Contorno de foco sem preenchimento.
static func outline(color: Color) -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0, 0, 0, 0)
	style.border_color = color
	style.set_border_width_all(1)
	style.set_corner_radius_all(2)
	return style


static func label(text: String, size: int = 15, color: Color = TEXT) -> Label:
	var node := Label.new()
	node.text = text
	node.add_theme_font_size_override("font_size", size)
	node.add_theme_color_override("font_color", color)
	return node


static func title(text: String, size: int = 22) -> Label:
	var node := label(text, size, TEXT)
	node.add_theme_font_override("font", DISPLAY_FONT)
	return node


## Rótulo pequeno em caixa alta, usado como cabeçalho de coluna.
static func caption(text: String, color: Color = TEXT_MUTED) -> Label:
	var node := label(text.to_upper(), 11, color)
	return node


## Linha horizontal de 1 px.
static func rule(color: Color = EDGE_SOFT) -> ColorRect:
	var node := ColorRect.new()
	node.color = color
	node.custom_minimum_size.y = 1
	node.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return node


## Par rótulo/valor usado na barra superior e nos resumos.
## @returns [container, value_label]
static func metric(caption_text: String, value_text: String, color: Color = TEXT) -> Array:
	var box := HBoxContainer.new()
	box.add_theme_constant_override("separation", 6)

	box.add_child(caption(caption_text))

	var value := label(value_text, 15, color)
	value.name = "Value"
	box.add_child(value)

	return [box, value]


## Barra fina de proporção (0..1) sobre trilho escuro.
static func meter(ratio: float, color: Color, height: int = 6) -> Control:
	var root := Control.new()
	root.custom_minimum_size.y = height
	root.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	root.mouse_filter = Control.MOUSE_FILTER_IGNORE

	var track := ColorRect.new()
	track.color = EDGE_SOFT
	track.mouse_filter = Control.MOUSE_FILTER_IGNORE
	track.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	root.add_child(track)

	var fill := ColorRect.new()
	fill.color = color
	fill.mouse_filter = Control.MOUSE_FILTER_IGNORE
	fill.anchor_right = clampf(ratio, 0.0, 1.0)
	fill.anchor_bottom = 1.0
	fill.offset_right = 0.0
	fill.offset_bottom = 0.0
	root.add_child(fill)

	return root


## Cabeçalho de tabela: mesmas larguras relativas das linhas.
static func table_header(headings: Array, weights: Array, aligns: Array = []) -> HBoxContainer:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 12)

	for index in headings.size():
		var cell := caption(str(headings[index]))
		cell.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		cell.size_flags_stretch_ratio = _weight(weights, index)
		cell.horizontal_alignment = _align(aligns, index)
		cell.clip_text = true
		row.add_child(cell)

	return row


## Linha de dados com as mesmas larguras do cabeçalho.
static func table_row(values: Array, weights: Array, colors: Array = [],
		aligns: Array = []) -> HBoxContainer:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 12)

	for index in values.size():
		var cell := label(str(values[index]), 14, _color(colors, index))
		cell.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		cell.size_flags_stretch_ratio = _weight(weights, index)
		cell.horizontal_alignment = _align(aligns, index)
		cell.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
		cell.clip_text = true
		row.add_child(cell)

	return row


## Linha clicável: painel que destaca no hover e emite `pressed` no clique.
class RowButton extends PanelContainer:
	signal pressed

	var _idle: StyleBoxFlat
	var _hover: StyleBoxFlat
	var _active: StyleBoxFlat
	var selected := false

	func _init(idle: StyleBoxFlat, hover: StyleBoxFlat, active: StyleBoxFlat) -> void:
		_idle = idle
		_hover = hover
		_active = active
		add_theme_stylebox_override("panel", _idle)
		mouse_filter = Control.MOUSE_FILTER_STOP
		mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND

		mouse_entered.connect(_on_mouse_entered)
		mouse_exited.connect(_on_mouse_exited)
		gui_input.connect(_on_gui_input)

	func set_selected(value: bool) -> void:
		selected = value
		add_theme_stylebox_override("panel", _active if selected else _idle)

	func _on_mouse_entered() -> void:
		if not selected:
			add_theme_stylebox_override("panel", _hover)

	func _on_mouse_exited() -> void:
		add_theme_stylebox_override("panel", _active if selected else _idle)

	func _on_gui_input(event: InputEvent) -> void:
		if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT \
				and event.pressed:
			pressed.emit()


static func row_button() -> RowButton:
	return RowButton.new(flat(PANEL, Color(0, 0, 0, 0), 8.0),
			flat(PANEL_HOVER, EDGE_SOFT, 8.0), flat(PANEL_RAISED, BRASS, 8.0))


## Barra de abas de uma janela.
static func tab_bar() -> HBoxContainer:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)
	return row


static func tab_button(text: String) -> Button:
	var button := Button.new()
	button.text = text
	button.focus_mode = Control.FOCUS_ALL
	button.custom_minimum_size.y = 30
	button.add_theme_font_size_override("font_size", 13)
	set_tab_active(button, false)
	return button


static func set_tab_active(button: Button, active: bool) -> void:
	var fill := BG_DEEP if active else PANEL
	var border := BRASS if active else EDGE_SOFT
	button.add_theme_stylebox_override("normal", flat(fill, border, 12.0))
	button.add_theme_stylebox_override("hover", flat(PANEL_HOVER, STEEL, 12.0))
	button.add_theme_stylebox_override("pressed", flat(BG_DEEP, BRASS, 12.0))
	button.add_theme_color_override("font_color", BRASS if active else TEXT)
	button.add_theme_color_override("font_hover_color", TEXT)


## Remove e libera os filhos de um contêiner imediatamente.
static func clear(node: Node) -> void:
	for child in node.get_children():
		node.remove_child(child)
		child.queue_free()


## Espaçador que empurra o conteúdo para os lados.
static func spacer() -> Control:
	var node := Control.new()
	node.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	node.size_flags_vertical = Control.SIZE_EXPAND_FILL
	node.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return node


static func nation_color(tag: String) -> Color:
	return NATION_COLORS.get(tag, STEEL)


# Idioma usado na formatação de números; o HUD sincroniza com o I18n.
static var _locale := "pt_BR"


static func set_locale(code: String) -> void:
	_locale = code


## Número com separador de milhar do idioma ativo.
static func number(value: float, digits: int = 0) -> String:
	var text := String.num(value, digits)
	var negative := text.begins_with("-")
	if negative:
		text = text.substr(1)

	var parts := text.split(".")
	var whole: String = parts[0]
	var grouped := ""
	while whole.length() > 3:
		grouped = _group_separator() + whole.substr(whole.length() - 3) + grouped
		whole = whole.substr(0, whole.length() - 3)

	var result := whole + grouped
	if parts.size() > 1:
		result += _decimal_separator() + parts[1]
	return ("-" if negative else "") + result


static func _group_separator() -> String:
	return "," if _locale == "en" else "."


static func _decimal_separator() -> String:
	return "." if _locale == "en" else ","


## Varia de verde a vermelho conforme o estoque cobre mais ou menos dias.
static func coverage_color(coverage_days: float) -> Color:
	if coverage_days >= 7.0:
		return POSITIVE
	if coverage_days >= 2.0:
		return WARNING
	return NEGATIVE


static func _weight(weights: Array, index: int) -> float:
	return float(weights[index]) if index < weights.size() else 1.0


static func _align(aligns: Array, index: int) -> HorizontalAlignment:
	if index < aligns.size() and bool(aligns[index]):
		return HORIZONTAL_ALIGNMENT_RIGHT
	return HORIZONTAL_ALIGNMENT_LEFT


static func _color(colors: Array, index: int) -> Color:
	return colors[index] if index < colors.size() else TEXT
