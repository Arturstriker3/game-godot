extends RefCounted
class_name UiKit

## Design System do WarEconomy — "Industrial Strategy, Premium Dark".
##
## Tokens centralizados (cores, tipografia, espaçamento, controles, estados) e
## construtores de componentes nativos do Godot 4 (Control/Container/Theme/
## StyleBox). Nenhum componente contém lógica econômica: eles recebem texto e
## valores já formatados pelo chamador.

# --- Tokens: superfícies e traços -------------------------------------------
const BG_DEEP := Color("#0A1112")
const PANEL := Color("#101A18")
const PANEL_RAISED := Color("#16221F")
const PANEL_HOVER := Color("#1E2E29")
const EDGE := Color("#2C3B35")
const EDGE_SOFT := Color("#22302B")
const OVERLAY := Color(0.02, 0.03, 0.04, 0.62)

# --- Tokens: texto ----------------------------------------------------------
const TEXT := Color("#E2DFD2")
const TEXT_DIM := Color("#9AA69B")
const TEXT_MUTED := Color("#7F8C82")
const TEXT_INVERSE := Color("#0E1715")

# --- Tokens: acentos e estados ---------------------------------------------
const BRASS := Color("#C9A24A")
const BRASS_DEEP := Color("#8E7134")
const COPPER := Color("#B0764A")
const STEEL := Color("#6F8F8C")
const POSITIVE := Color("#7FA86B")
const NEGATIVE := Color("#D26A5F")
const WARNING := Color("#D08A3E")

# --- Tokens: tipografia -----------------------------------------------------
const BODY_FONT: Font = preload("res://assets/fonts/BarlowCondensed-Regular.ttf")
const CAPTION_FONT: Font = preload("res://assets/fonts/BarlowCondensed-SemiBold.ttf")
const DISPLAY_BASE: Font = preload("res://assets/fonts/Cinzel-Variable.ttf")

const FS_DISPLAY := 26
const FS_TITLE := 19
const FS_SUBTITLE := 15
const FS_BODY := 15
const FS_SMALL := 13
const FS_CAPTION := 11

# --- Tokens: espaçamento, controles e movimento -----------------------------
const SPACE_XS := 4
const SPACE_SM := 8
const SPACE_MD := 12
const SPACE_LG := 16
const SPACE_XL := 24

const CONTROL_HEIGHT := 30
const DOCK_WIDTH := 108.0
const DOCK_BUTTON_HEIGHT := 52.0
const RADIUS := 2

const DURATION_FAST := 0.12
const DURATION := 0.20

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
	theme.default_font_size = FS_BODY

	theme.set_stylebox("panel", "Panel", flat(PANEL, EDGE))
	theme.set_stylebox("panel", "PanelContainer", flat(PANEL, EDGE))
	theme.set_stylebox("panel", "ScrollContainer", StyleBoxEmpty.new())

	theme.set_stylebox("normal", "Button", flat(PANEL_RAISED, EDGE, 12.0))
	theme.set_stylebox("hover", "Button", flat(PANEL_HOVER, STEEL, 12.0))
	theme.set_stylebox("pressed", "Button", flat(BG_DEEP, BRASS, 12.0))
	theme.set_stylebox("focus", "Button", outline(BRASS))
	theme.set_stylebox("disabled", "Button", flat(PANEL, EDGE_SOFT, 12.0))

	theme.set_color("font_color", "Button", TEXT)
	theme.set_color("font_hover_color", "Button", Color.WHITE)
	theme.set_color("font_pressed_color", "Button", BRASS)
	theme.set_color("font_focus_color", "Button", TEXT)
	theme.set_color("font_disabled_color", "Button", TEXT_MUTED)
	theme.set_font_size("font_size", "Button", 14)

	theme.set_stylebox("normal", "LineEdit", flat(BG_DEEP, EDGE, 10.0))
	theme.set_stylebox("focus", "LineEdit", outline(BRASS))
	theme.set_stylebox("read_only", "LineEdit", flat(PANEL, EDGE_SOFT, 10.0))
	theme.set_color("font_color", "LineEdit", TEXT)
	theme.set_color("font_placeholder_color", "LineEdit", TEXT_MUTED)
	theme.set_color("caret_color", "LineEdit", BRASS)
	theme.set_color("selection_color", "LineEdit", Color(BRASS, 0.28))
	theme.set_font_size("font_size", "LineEdit", FS_SMALL)

	theme.set_stylebox("normal", "OptionButton", flat(PANEL_RAISED, EDGE, 12.0))
	theme.set_stylebox("hover", "OptionButton", flat(PANEL_HOVER, STEEL, 12.0))
	theme.set_stylebox("pressed", "OptionButton", flat(BG_DEEP, BRASS, 12.0))
	theme.set_stylebox("focus", "OptionButton", outline(BRASS))
	theme.set_color("font_color", "OptionButton", TEXT)
	theme.set_font_size("font_size", "OptionButton", FS_SMALL)

	theme.set_stylebox("panel", "PopupMenu", flat(PANEL_RAISED, EDGE, 8.0))
	theme.set_stylebox("hover", "PopupMenu", flat(PANEL_HOVER, Color.TRANSPARENT, 8.0))
	theme.set_color("font_color", "PopupMenu", TEXT)
	theme.set_color("font_hover_color", "PopupMenu", Color.WHITE)
	theme.set_color("font_disabled_color", "PopupMenu", TEXT_MUTED)

	theme.set_stylebox("background", "ProgressBar", flat(BG_DEEP, EDGE_SOFT))
	theme.set_stylebox("fill", "ProgressBar", flat(BRASS, BRASS))
	theme.set_color("font_color", "ProgressBar", TEXT)
	theme.set_font_size("font_size", "ProgressBar", FS_CAPTION)

	theme.set_stylebox("slider", "HSlider", flat(EDGE_SOFT, EDGE_SOFT))
	theme.set_stylebox("grabber_area", "HSlider", flat(BRASS_DEEP, BRASS_DEEP))
	theme.set_stylebox("grabber_area_highlight", "HSlider", flat(BRASS, BRASS))

	theme.set_color("font_color", "Label", TEXT)
	theme.set_color("font_color", "CheckBox", TEXT)
	theme.set_color("font_hover_color", "CheckBox", Color.WHITE)

	theme.set_constant("separation", "HBoxContainer", SPACE_SM)
	theme.set_constant("separation", "VBoxContainer", SPACE_SM)
	theme.set_constant("h_separation", "GridContainer", SPACE_SM)
	theme.set_constant("v_separation", "GridContainer", SPACE_SM)

	# Barras de rolagem e tooltips também pertencem à paleta.
	theme.set_stylebox("scroll", "VScrollBar", flat(BG_DEEP, EDGE_SOFT))
	theme.set_stylebox("grabber", "VScrollBar", flat(EDGE, EDGE))
	theme.set_stylebox("grabber_highlight", "VScrollBar", flat(STEEL, STEEL))
	theme.set_stylebox("grabber_pressed", "VScrollBar", flat(BRASS, BRASS))
	# Tooltip com borda latão (estilo HOI3): destaca do fundo do mapa.
	theme.set_stylebox("panel", "TooltipPanel", tooltip_style())
	theme.set_color("font_color", "TooltipLabel", TEXT)
	theme.set_font_size("font_size", "TooltipLabel", FS_SMALL)
	theme.set_constant("separation", "HSeparator", 1)
	theme.set_constant("separation", "VSeparator", 1)

	return theme


## Caixa chapada com borda de 1 px.
static func flat(fill: Color, border: Color, padding: float = 0.0) -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = fill
	style.border_color = border
	style.set_border_width_all(1)
	style.set_corner_radius_all(RADIUS)
	if padding > 0.0:
		style.content_margin_left = padding
		style.content_margin_right = padding
		style.content_margin_top = padding * 0.5
		style.content_margin_bottom = padding * 0.5
	return style


## Painel sem borda, usado como superfície de fundo.
static func surface(fill: Color, padding: float = 0.0) -> StyleBoxFlat:
	return flat(fill, Color.TRANSPARENT, padding)


## Contorno de foco sem preenchimento.
static func outline(color: Color) -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0, 0, 0, 0)
	style.border_color = color
	style.set_border_width_all(1)
	style.set_corner_radius_all(RADIUS)
	return style


## Estilo de tooltip HOI3: fundo escuro com borda latão e sombra sutil.
static func tooltip_style() -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = PANEL
	style.border_color = BRASS
	style.set_border_width_all(1)
	style.set_corner_radius_all(RADIUS)
	style.content_margin_left = 10
	style.content_margin_right = 10
	style.content_margin_top = 6
	style.content_margin_bottom = 6
	# Sombra sutil (estilo HOI3)
	style.shadow_color = Color(0, 0, 0, 0.4)
	style.shadow_size = 4
	style.shadow_offset = Vector2(2, 2)
	return style


# --- Tipografia -------------------------------------------------------------

static func label(text: String, size: int = FS_BODY, color: Color = TEXT) -> Label:
	var node := Label.new()
	node.text = text
	node.add_theme_font_size_override("font_size", size)
	node.add_theme_color_override("font_color", color)
	return node


static func title(text: String, size: int = 22) -> Label:
	var node := label(text, size, TEXT)
	node.add_theme_font_override("font", display_font())
	return node


## Rótulo pequeno em caixa alta, usado como cabeçalho de coluna.
static func caption(text: String, color: Color = TEXT_MUTED) -> Label:
	var node := label(text.to_upper(), FS_CAPTION, color)
	node.add_theme_font_override("font", CAPTION_FONT)
	return node


## Rótulo de apoio com quebra de linha e largura controlada.
static func paragraph(text: String, size: int = FS_SMALL, color: Color = TEXT_DIM,
		width: float = 0.0) -> Label:
	var node := label(text, size, color)
	node.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	if width > 0.0:
		node.custom_minimum_size.x = width
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

	var value := label(value_text, FS_SUBTITLE, color)
	value.name = "Value"
	box.add_child(value)

	return [box, value]


# --- Controles --------------------------------------------------------------

static func _button_base(text: String) -> Button:
	var button := Button.new()
	button.text = text
	button.custom_minimum_size.y = CONTROL_HEIGHT
	button.add_theme_font_size_override("font_size", FS_SMALL)
	button.focus_mode = Control.FOCUS_ALL
	_bind_sfx(button)
	return button


## Associa um efeito sonoro de interface ao clique do botão.
static func _bind_sfx(button: BaseButton, sound: int = UiSfx.Sound.CLICK) -> void:
	button.pressed.connect(func() -> void: UiSfx.play(sound))


## Intervalo mínimo entre sons de rolagem e de arraste (ms).
const SCROLL_SFX_MS := 90
const SLIDER_SFX_MS := 55


## Som de rolagem: segue as barras do ScrollContainer, sem soar quando o mouse
## está fora do painel (o recálculo de layout muda o valor sem o jogador rolar).
static func bind_scroll_sfx(scroll: ScrollContainer) -> void:
	if scroll == null or scroll.has_meta("sfx_scroll_bound"):
		return
	scroll.set_meta("sfx_scroll_bound", true)

	var key := "scroll:%d" % scroll.get_instance_id()
	var on_change := func(_value: float) -> void:
		if not is_instance_valid(scroll):
			return
		if scroll.get_global_rect().has_point(scroll.get_global_mouse_position()):
			UiSfx.play_throttled(UiSfx.Sound.SCROLL, key, SCROLL_SFX_MS)

	scroll.get_v_scroll_bar().value_changed.connect(on_change)
	scroll.get_h_scroll_bar().value_changed.connect(on_change)


## Som de arraste: tique curto a cada passo do slider, só enquanto o jogador
## segura o controle (mudanças programáticas de valor continuam silenciosas).
static func bind_slider_sfx(slider: Slider) -> void:
	if slider == null or slider.has_meta("sfx_slider_bound"):
		return
	slider.set_meta("sfx_slider_bound", true)

	var dragging := [false]
	slider.drag_started.connect(func(_value: float) -> void: dragging[0] = true)
	slider.drag_ended.connect(
			func(_changed: bool) -> void: dragging[0] = false)
	slider.value_changed.connect(_on_slider_tick.bind(slider, dragging))


static func _on_slider_tick(_value: float, slider: Slider, dragging: Array) -> void:
	if not dragging[0] or not is_instance_valid(slider):
		return
	UiSfx.play_throttled(UiSfx.Sound.TICK,
			"slider:%d" % slider.get_instance_id(), SLIDER_SFX_MS)


## Ação principal — latão sólido, texto escuro. Usar no máximo uma por tela.
static func primary_button(text: String) -> Button:
	var button := _button_base(text)
	button.add_theme_stylebox_override("normal", flat(BRASS, BRASS, 14.0))
	button.add_theme_stylebox_override("hover", flat(Color("#D8B35A"), BRASS, 14.0))
	button.add_theme_stylebox_override("pressed", flat(BRASS_DEEP, BRASS, 14.0))
	button.add_theme_stylebox_override("focus", outline(BG_DEEP))
	button.add_theme_color_override("font_color", TEXT_INVERSE)
	button.add_theme_color_override("font_hover_color", TEXT_INVERSE)
	button.add_theme_color_override("font_pressed_color", TEXT_INVERSE)
	button.add_theme_color_override("font_focus_color", TEXT_INVERSE)
	return button


## Ação padrão em painéis.
static func secondary_button(text: String) -> Button:
	var button := _button_base(text)
	button.add_theme_stylebox_override("normal", flat(PANEL_RAISED, EDGE, 14.0))
	button.add_theme_stylebox_override("hover", flat(PANEL_HOVER, STEEL, 14.0))
	button.add_theme_stylebox_override("pressed", flat(BG_DEEP, BRASS, 14.0))
	return button


## Ação discreta: sem fundo até o hover.
static func ghost_button(text: String) -> Button:
	var button := _button_base(text)
	button.add_theme_stylebox_override("normal",
			flat(Color(0, 0, 0, 0), Color(0, 0, 0, 0), 12.0))
	button.add_theme_stylebox_override("hover", flat(PANEL_HOVER, EDGE_SOFT, 12.0))
	button.add_theme_stylebox_override("pressed", flat(BG_DEEP, BRASS, 12.0))
	button.add_theme_color_override("font_color", TEXT_DIM)
	button.add_theme_color_override("font_hover_color", TEXT)
	return button


## Botão compacto só com glifo (×, ‹, ≡). Sempre acompanhado de tooltip.
static func icon_button(glyph: String, tooltip_text: String = "") -> Button:
	var button := _button_base(glyph)
	button.custom_minimum_size = Vector2(30.0, 30.0)
	button.add_theme_font_size_override("font_size", 16)
	button.add_theme_stylebox_override("normal",
			flat(Color(0, 0, 0, 0), Color(0, 0, 0, 0), 6.0))
	button.add_theme_stylebox_override("hover", flat(PANEL_HOVER, EDGE_SOFT, 6.0))
	button.add_theme_stylebox_override("pressed", flat(BG_DEEP, BRASS, 6.0))
	button.add_theme_color_override("font_color", TEXT_DIM)
	button.add_theme_color_override("font_hover_color", TEXT)
	button.tooltip_text = tooltip_text
	return button


## Barra de proporção (0..1) sobre trilho escuro.
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


## Paleta de uma etiqueta de estado: [fundo, borda, texto].
static func pill_palette(tone: String) -> Array:
	var palette := {
		"neutral": [PANEL_RAISED, EDGE_SOFT, TEXT_DIM],
		"accent": [Color(BRASS, 0.14), Color(BRASS, 0.40), BRASS],
		"positive": [Color(POSITIVE, 0.12), Color(POSITIVE, 0.38), POSITIVE],
		"negative": [Color(NEGATIVE, 0.12), Color(NEGATIVE, 0.38), NEGATIVE],
		"warning": [Color(WARNING, 0.12), Color(WARNING, 0.38), WARNING],
		"steel": [Color(STEEL, 0.14), Color(STEEL, 0.40), STEEL],
	}
	return palette.get(tone, palette["neutral"])


## Atualiza uma etiqueta existente sem recriar o nó (texto + cor semântica).
static func apply_pill(panel: PanelContainer, text: String, tone: String) -> void:
	var colors: Array = pill_palette(tone)
	panel.add_theme_stylebox_override("panel", flat(colors[0], colors[1], 9.0))
	var label := panel.get_child(0) as Label
	if label == null:
		return
	label.text = text
	label.add_theme_color_override("font_color", colors[2])


## Etiqueta de estado (déficit, paz, em trânsito) com cor semântica.
static func status_pill(text: String, tone: String = "neutral") -> PanelContainer:
	var colors: Array = pill_palette(tone)

	var panel := PanelContainer.new()
	panel.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	panel.add_theme_stylebox_override("panel", flat(colors[0], colors[1], 9.0))

	var node := label(text, FS_CAPTION + 1, colors[2])
	node.add_theme_font_override("font", CAPTION_FONT)
	node.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	panel.add_child(node)
	return panel


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


## Cabeçalho de tabela ordenável: cada coluna é um botão que pede a ordenação.
## A coluna ativa recebe a cor de destaque e a seta do sentido atual.
static func sort_header(headings: Array, weights: Array, aligns: Array, active: int,
		descending: bool, on_sort: Callable) -> HBoxContainer:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 12)

	for index in headings.size():
		var text := str(headings[index])
		if index == active:
			text += " ▾" if descending else " ▴"

		var cell := Button.new()
		cell.text = text
		cell.flat = true
		cell.clip_text = true
		cell.focus_mode = Control.FOCUS_ALL
		cell.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
		cell.alignment = _align(aligns, index)
		cell.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		cell.size_flags_stretch_ratio = _weight(weights, index)
		cell.add_theme_font_override("font", CAPTION_FONT)
		cell.add_theme_font_size_override("font_size", FS_CAPTION)
		cell.add_theme_color_override("font_color",
				BRASS if index == active else TEXT_MUTED)
		cell.add_theme_color_override("font_hover_color", BRASS)
		cell.pressed.connect(func() -> void: on_sort.call(index))
		_bind_sfx(cell)
		row.add_child(cell)

	return row


## Linha de dados com as mesmas larguras do cabeçalho.
static func table_row(values: Array, weights: Array, colors: Array = [],
		aligns: Array = []) -> HBoxContainer:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 12)

	for index in values.size():
		var cell := label(str(values[index]), FS_SMALL, _color(colors, index))
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
		focus_mode = Control.FOCUS_ALL

		mouse_entered.connect(_on_mouse_entered)
		mouse_exited.connect(_on_mouse_exited)
		gui_input.connect(_on_gui_input)
		focus_entered.connect(_on_mouse_entered)
		focus_exited.connect(_on_mouse_exited)

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
			UiSfx.play(UiSfx.Sound.SELECT)
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
	button.custom_minimum_size.y = CONTROL_HEIGHT
	button.add_theme_font_size_override("font_size", FS_SMALL)
	set_tab_active(button, false)
	_bind_sfx(button, UiSfx.Sound.SWITCH)
	return button


static func set_tab_active(button: Button, active: bool) -> void:
	var fill := BG_DEEP if active else Color(0, 0, 0, 0)
	var border := BRASS if active else Color(0, 0, 0, 0)
	button.add_theme_stylebox_override("normal", flat(fill, border, 12.0))
	button.add_theme_stylebox_override("hover", flat(PANEL_HOVER, EDGE_SOFT, 12.0))
	button.add_theme_stylebox_override("pressed", flat(BG_DEEP, BRASS, 12.0))
	button.add_theme_stylebox_override("focus", outline(BRASS_DEEP))
	button.add_theme_color_override("font_color", BRASS if active else TEXT_DIM)
	button.add_theme_color_override("font_hover_color", TEXT)


## Barra de ferramentas (busca, filtros, ações) com espaçamento consistente.
static func toolbar() -> HBoxContainer:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", SPACE_SM)
	row.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	return row


## Campo de busca com botão de limpar nativo.
static func search_field(placeholder: String) -> LineEdit:
	var field := LineEdit.new()
	field.placeholder_text = placeholder
	field.clear_button_enabled = true
	field.custom_minimum_size = Vector2(220.0, CONTROL_HEIGHT)
	field.add_theme_font_size_override("font_size", FS_SMALL)
	return field


## Filtro alternável compacto.
static func filter_chip(text: String) -> Button:
	var button := Button.new()
	button.text = text
	button.toggle_mode = true
	button.custom_minimum_size.y = 26.0
	button.add_theme_font_size_override("font_size", FS_CAPTION + 1)
	button.add_theme_stylebox_override("normal", flat(Color(0, 0, 0, 0), EDGE_SOFT, 10.0))
	button.add_theme_stylebox_override("hover", flat(PANEL_HOVER, STEEL, 10.0))
	button.add_theme_stylebox_override("pressed", flat(PANEL_HOVER, BRASS, 10.0))
	button.add_theme_color_override("font_color", TEXT_DIM)
	button.add_theme_color_override("font_hover_color", TEXT)
	button.add_theme_color_override("font_pressed_color", BRASS)
	_bind_sfx(button, UiSfx.Sound.TOGGLE)
	return button


## Mantém o contorno de chip nos dois estados, trocando só o realce.
static func set_chip_active(button: Button, active: bool) -> void:
	button.set_pressed_no_signal(active)
	button.add_theme_stylebox_override("normal",
			flat(PANEL_HOVER if active else Color(0, 0, 0, 0),
					BRASS if active else EDGE_SOFT, 10.0))
	button.add_theme_color_override("font_color", BRASS if active else TEXT_DIM)


## Cartão de conteúdo com padding uniforme.
static func card(padding: float = SPACE_LG) -> PanelContainer:
	var panel := PanelContainer.new()
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var style := flat(PANEL, EDGE, padding)
	style.content_margin_top = padding
	style.content_margin_bottom = padding
	panel.add_theme_stylebox_override("panel", style)
	return panel


## Estado vazio honesto: título, explicação e ação opcional.
static func empty_state(heading: String, body: String, action_text: String = "",
		action: Callable = Callable()) -> Control:
	return _state_block(heading, body, TEXT_DIM, "neutral", action_text, action)


## Estado de erro recuperável.
static func error_state(heading: String, body: String, action_text: String = "",
		action: Callable = Callable()) -> Control:
	return _state_block(heading, body, NEGATIVE, "negative", action_text, action)


## Estado de carregamento (sem animação: o chamador troca quando os dados chegam).
static func loading_state(text: String = "Carregando…") -> Control:
	var center := CenterContainer.new()
	center.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	center.size_flags_vertical = Control.SIZE_EXPAND_FILL
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", SPACE_SM)
	row.add_child(label("⋯", 18, BRASS))
	row.add_child(label(text, FS_SMALL, TEXT_DIM))
	center.add_child(row)
	return center


static func _state_block(heading: String, body: String, color: Color, tone: String,
		action_text: String, action: Callable) -> Control:
	var center := CenterContainer.new()
	center.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	center.size_flags_vertical = Control.SIZE_EXPAND_FILL

	var panel := PanelContainer.new()
	panel.custom_minimum_size.x = 520.0
	panel.add_theme_stylebox_override("panel", flat(PANEL, EDGE_SOFT, 24.0))

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", SPACE_MD)
	panel.add_child(box)

	var head := HBoxContainer.new()
	head.add_theme_constant_override("separation", SPACE_SM)
	head.add_child(status_pill("•", tone))
	head.add_child(title(heading, FS_TITLE))
	box.add_child(head)

	box.add_child(paragraph(body, FS_SMALL, TEXT_MUTED, 440.0))

	if not action_text.is_empty() and action.is_valid():
		var row := HBoxContainer.new()
		row.alignment = BoxContainer.ALIGNMENT_END
		row.add_child(secondary_button(action_text))
		var button := row.get_child(0) as Button
		button.pressed.connect(action)
		box.add_child(row)

	center.add_child(panel)
	return center


## Aviso efêmero no rodapé da tela; some sozinho.
static func toast(parent: Control, text: String, tone: String = "neutral",
		seconds: float = 2.4) -> void:
	match tone:
		"negative":
			UiSfx.play(UiSfx.Sound.ERROR)
		"warning":
			UiSfx.play(UiSfx.Sound.QUESTION)
		_:
			pass

	var holder := Control.new()
	holder.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	holder.mouse_filter = Control.MOUSE_FILTER_IGNORE
	parent.add_child(holder)

	var column := VBoxContainer.new()
	column.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_WIDE)
	column.offset_top = -72.0
	column.offset_bottom = -SPACE_XL
	column.alignment = BoxContainer.ALIGNMENT_END
	column.mouse_filter = Control.MOUSE_FILTER_IGNORE
	holder.add_child(column)

	var row := HBoxContainer.new()
	row.alignment = BoxContainer.ALIGNMENT_CENTER
	row.mouse_filter = Control.MOUSE_FILTER_IGNORE
	column.add_child(row)

	var pill := status_pill(text, tone)
	row.add_child(pill)

	var tween := holder.create_tween()
	holder.modulate.a = 0.0
	tween.tween_property(holder, "modulate:a", 1.0, DURATION_FAST)
	tween.tween_interval(seconds)
	tween.tween_property(holder, "modulate:a", 0.0, DURATION)
	tween.tween_callback(holder.queue_free)


## Foca o primeiro controle focável e visível da árvore.
static func focus_first(root: Node) -> void:
	var stack: Array[Node] = [root]
	while not stack.is_empty():
		var node: Node = stack.pop_front()
		if node is Control:
			var control := node as Control
			if control.is_visible_in_tree() and control.focus_mode == Control.FOCUS_ALL \
					and control.custom_minimum_size != Vector2.ZERO:
				control.grab_focus()
				return
		stack.append_array(node.get_children())


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
