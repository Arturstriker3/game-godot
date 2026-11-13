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

# --- Tokens: paleta C — "Papelaria de guerra" --------------------------------
## Paleta da variante C do HUD (fichas datilografadas, papel, carimbo). Usada
## apenas na barra superior; os demais componentes seguem o tema Industrial.
const PAPEL := Color("#e6dbbd")            # papel (fundo de ficha)
const PAPEL_CLARO := Color("#efe6cc")      # papel claro (aba ativa)
const ABA_INATIVA := Color("#d5c7a1")      # aba inativa
const TINTA := Color("#2b2418")            # tinta (texto principal)
const TINTA_SUAVE := Color("#5a4a2c")      # tinta suave (rótulos)
const CARIMBO := Color("#a3241a")          # carimbo (déficit, alerta)
const CARIMBO_BARRA := Color("#e0605a")     # carimbo sobre a barra escura (déficit/badge)

# --- Tokens: botão fechar "Lacre de cera" ------------------------------------
## Círculo de cera CARIMBO com borda escura e anel interno; hover/pressed
## clareiam e escurecem o lacre; o X é papel claro.
const SEAL_BORDER := Color("#7a1a12")       # borda 2 px do lacre
const SEAL_HOVER := Color("#c43c30")        # preenchimento no hover
const SEAL_PRESSED := Color("#8e1f17")      # preenchimento pressionado
const SEAL_SIZE_BAR := 38                   # sobre a barra escura (window_frame)
const SEAL_SIZE_PAPER := 52                 # sobre papel (telegrama, menu, T3)
const POSITIVO := Color("#3c5a22")         # positivo (verde-papel)
const ALERTA := Color("#7a5410")           # alerta (âmbar-papel)
const BARRA_FUNDO := Color("#2d2a24")      # fundo da barra superior
const BARRA_FILETE := Color("#b9a77a")     # filete/borda da barra superior
const BARRA_TEXTO := Color("#efe4c6")      # valor na barra superior
const BARRA_ROTULO := Color("#b3a47e")     # rótulo na barra superior

# --- Tokens: gerador de telas da paleta C ----------------------------------
## Véu atrás das janelas/cartões de papel (não cobre a barra superior).
const PAPER_VEU := Color(0.02, 0.03, 0.04, 0.55)
## Escala tipográfica das fichas (Black Ops One para títulos, Special Elite
## para rótulos, Barlow Condensed para dados).
const FS_PAPER_HERO := 30
const FS_PAPER_TITLE := 24
const FS_PAPER_HEADING := 20
const FS_PAPER_VALUE := 17
const FS_PAPER_LABEL := 12

# --- Tokens: "Etiqueta de inventário" (legenda do mapa de recursos) -----------
const MANILA := Color("#c9a96a")            # papel pardo da etiqueta
const MANILA_LIGHT := Color("#d8bd84")      # manila claro (botão "Todos")
const MANILA_DARK := Color("#8a6a34")       # manila escuro (anel do furo)
const MANILA_VIGNETTE := Color("#7a5a2a")   # vinheta interna da etiqueta
const HOLE := Color("#5f625e")              # furo da etiqueta (vaza para o mapa)
const STAGE_START := Color("#c43c30")       # estágio inicial do depósito (vermelho)
const STAGE_PRODUCING := Color("#5f9a3f")   # estágio produtor (verde)
## Escala de situação do depósito (1..5): residual → intacta. Reaproveita os
## extremos da escala de estágio (mesmos matizes), com os meios-tons próprios.
const SITUATION_1 := STAGE_START            # residual (vermelho)
const SITUATION_2 := Color("#c8642c")       # escassa
const SITUATION_3 := Color("#d29a2a")       # moderada
const SITUATION_4 := Color("#9aa83a")       # abundante
const SITUATION_5 := STAGE_PRODUCING        # intacta (verde)

# --- Tokens: tipografia -----------------------------------------------------
const BODY_FONT: Font = preload("res://assets/fonts/BarlowCondensed-Regular.ttf")
const CAPTION_FONT: Font = preload("res://assets/fonts/BarlowCondensed-SemiBold.ttf")
const DISPLAY_BASE: Font = preload("res://assets/fonts/Cinzel-Variable.ttf")
## Fontes da "Papelaria de guerra": títulos em Black Ops One (OFL) e corpo/dados
## em Special Elite (Apache 2.0), simulando ficha datilografada.
const PAPER_DISPLAY_FONT: Font = preload("res://assets/fonts/BlackOpsOne-Regular.ttf")
const PAPER_BODY_FONT: Font = preload("res://assets/fonts/SpecialElite-Regular.ttf")

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
## Barra lateral esquerda de painéis (ver `hud.gd::RAIL_WIDTH`).
const DOCK_WIDTH := 208.0
const DOCK_BUTTON_HEIGHT := 48.0
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
	# Tooltip em papel (papel claro + borda de tinta), tipografia datilografada.
	theme.set_stylebox("panel", "TooltipPanel", paper_tooltip_style())
	theme.set_font("font", "TooltipLabel", PAPER_BODY_FONT)
	theme.set_color("font_color", "TooltipLabel", TINTA)
	theme.set_font_size("font_size", "TooltipLabel", FS_CAPTION + 1)
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


## Estilo de tooltip em papel: papel claro com borda de tinta 1 px e sombra sutil.
static func paper_tooltip_style() -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = PAPEL_CLARO
	style.border_color = TINTA
	style.set_border_width_all(1)
	style.set_corner_radius_all(RADIUS)
	style.content_margin_left = 8
	style.content_margin_right = 8
	style.content_margin_top = 5
	style.content_margin_bottom = 5
	style.shadow_color = Color(0, 0, 0, 0.30)
	style.shadow_size = 3
	style.shadow_offset = Vector2(1, 2)
	return style


## Textura de papel procedural (512×512) gerada por tools/generate_paper_texture.gd.
const PAPER_TEXTURE: Texture2D = preload("res://assets/ui/paper.png")


## Fundo de ficha em papel, com 9-patch para a vinheta das bordas acompanhar
## qualquer tamanho de painel.
static func paper_stylebox() -> StyleBoxTexture:
	var style := StyleBoxTexture.new()
	style.texture = PAPER_TEXTURE
	var edge := 48
	style.texture_margin_left = edge
	style.texture_margin_right = edge
	style.texture_margin_top = edge
	style.texture_margin_bottom = edge
	var padding := 18
	style.content_margin_left = padding
	style.content_margin_right = padding
	style.content_margin_top = padding
	style.content_margin_bottom = padding
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
	button.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
	_bind_sfx(button)
	return button


## Cursor de mão em TODO controle clicável, por padrão: botões (menos os
## desabilitados), linhas clicáveis (`RowButton`, `PaperRow`) e qualquer painel
## que emita `pressed`. Chamado no fim de `UiTemplateBuilder.build` e nas
## camadas do HUD, então o que o jogo cria hoje e o que vier depois herda a
## regra sem ajuste item a item.
##
## O pass SÓ pinta `mouse_default_cursor_shape`: nada de foco, `mouse_filter`
## ou hierarquia. O setter, porém, pede ao viewport para atualizar o cursor
## quando o controle está sob o mouse, e essa atualização injeta um
## `InputEventMouseMotion` sintético: ele inicia o timer de tooltip do controle
## e o motor passa a consumir o próximo `ui_cancel` para fechar o tooltip (a
## ficha deixava de fechar com Esc). Com o GUI desligado durante o passe, o
## refresh não roda e o efeito colateral desaparece.
static func pointer_cursor(root: Node) -> void:
	if root == null:
		return
	var viewport := root.get_viewport() if root.is_inside_tree() else null
	var gui_was_disabled := viewport != null and viewport.gui_disable_input
	if viewport != null:
		viewport.gui_disable_input = true
	_paint_pointer_cursor(root)
	if viewport != null:
		viewport.gui_disable_input = gui_was_disabled


static func _paint_pointer_cursor(root: Node) -> void:
	for child in root.get_children():
		if child is Control:
			var clickable := child.has_signal("pressed")
			if child is BaseButton:
				clickable = not (child as BaseButton).disabled
			if clickable:
				(child as Control).mouse_default_cursor_shape = \
						Control.CURSOR_POINTING_HAND
		_paint_pointer_cursor(child)


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


# --- Botão fechar: "Lacre de cera" ------------------------------------------
## Papel semântico do controle no HUD (meta `UI_ROLE`). `ROLE_CLOSE` marca o
## botão de fechar/lacre: `focus_first` nunca começa por ele.
const UI_ROLE := "ui_role"
const ROLE_CLOSE := "close"
## Meta para o `focus_first` preferir um controle (busca, aba ativa, 1ª linha).
const UI_FOCUS_PREFERRED := "ui_focus_preferred"

## Estado do lacre: o anel interno acende no hover e, no foco, SÓ quando a
## navegação vem do teclado (`keyboard_nav`).
static var _seal_lights: Array = []

## Fechar de TODAS as telas: círculo de cera CARIMBO (#a3241a), borda 2 px
## #7a1a12, anel interno de 3 px #c43c30 e X em papel (#efe6cc) DESENHADO em
## vetor (não um glifo de fonte, que aparecia como letra "x" em algumas telas).
## Hover: fundo #c43c30 e anel #e0605a; pressed: #8e1f17.
## Foco: SEM anel em volta — o anel latão do tema virava uma moldura dourada no
## X ao abrir a tela. O realce de foco é o próprio anel interno do lacre
## (#e0605a) e só aparece na navegação por teclado.
## `size` = 38 sobre a barra escura, 52 sobre papel.
static func close_seal_button(size: int = SEAL_SIZE_BAR) -> Button:
	var button := Button.new()
	button.custom_minimum_size = Vector2(size, size)
	button.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	button.focus_mode = Control.FOCUS_ALL
	button.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
	button.tooltip_text = I18n.t("tpl.common.close")
	button.accessibility_name = I18n.t("tpl.common.close")
	button.set_meta(UI_ROLE, ROLE_CLOSE)

	var radius := int(round(size * 0.5))
	button.add_theme_stylebox_override("normal", _seal_style(CARIMBO, radius))
	button.add_theme_stylebox_override("hover", _seal_style(SEAL_HOVER, radius))
	button.add_theme_stylebox_override("pressed", _seal_style(SEAL_PRESSED, radius))
	button.add_theme_stylebox_override("focus", StyleBoxEmpty.new())
	_bind_sfx(button, UiSfx.Sound.CLOSE)

	# Anel interno de 3 px: painel redondo vazado, recuado 4 px da borda.
	var ring := Panel.new()
	ring.mouse_filter = Control.MOUSE_FILTER_IGNORE
	ring.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	ring.offset_left = 4.0
	ring.offset_top = 4.0
	ring.offset_right = -4.0
	ring.offset_bottom = -4.0
	var inner_radius := maxi(radius - 4, 2)
	ring.add_theme_stylebox_override("panel",
			_seal_ring_style(STAGE_START, inner_radius))
	button.add_child(ring)

	var light := {
		"button": button,
		"ring": ring,
		"radius": inner_radius,
		"hover": false,
	}
	_seal_lights.append(light)
	button.tree_exiting.connect(func() -> void: _seal_lights.erase(light))
	button.tree_entered.connect(func() -> void: ensure_nav_guard(button))
	button.mouse_entered.connect(func() -> void:
		light["hover"] = true
		_paint_seal_ring(light))
	button.mouse_exited.connect(func() -> void:
		light["hover"] = false
		_paint_seal_ring(light))
	button.focus_entered.connect(func() -> void: _paint_seal_ring(light))
	button.focus_exited.connect(func() -> void: _paint_seal_ring(light))

	# X vetorial por cima do anel: idêntico em 38 e 52 px, em qualquer fonte.
	var cross := SealCross.new()
	cross.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	button.add_child(cross)
	return button


## Cruz do lacre desenhada com `draw_line`: duas linhas de 3 px, pontas
## arredondadas (círculos do raio do traço) e cor de papel. O tamanho acompanha
## o botão, então o X é o mesmo desenho em 38 e em 52 px.
class SealCross extends Control:
	const STROKE_PX := 3.0
	## Meia-diagonal da cruz como fração do diâmetro do lacre.
	const ARM_RATIO := 0.145

	func _init() -> void:
		mouse_filter = Control.MOUSE_FILTER_IGNORE

	func _notification(what: int) -> void:
		if what == NOTIFICATION_RESIZED:
			queue_redraw()

	func _draw() -> void:
		var arm := minf(size.x, size.y) * ARM_RATIO
		var center := size * 0.5
		for direction in [Vector2(-1.0, -1.0), Vector2(-1.0, 1.0)]:
			var a: Vector2 = center + direction * arm
			var b: Vector2 = center - direction * arm
			draw_line(a, b, PAPEL_CLARO, STROKE_PX, true)
			# O `draw_line` do Godot só tem ponta reta; os círculos arredondam.
			draw_circle(a, STROKE_PX * 0.5, PAPEL_CLARO)
			draw_circle(b, STROKE_PX * 0.5, PAPEL_CLARO)


## Disco de cera: fundo cheio, borda 2 px SEAL_BORDER e cantos redondos no raio.
static func _seal_style(fill: Color, radius: int) -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = fill
	style.border_color = SEAL_BORDER
	style.set_border_width_all(2)
	style.set_corner_radius_all(radius)
	return style


## Anel interno vazado de 3 px (sem preenchimento).
static func _seal_ring_style(color: Color, radius: int) -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0, 0, 0, 0)
	style.border_color = color
	style.set_border_width_all(3)
	style.set_corner_radius_all(radius)
	return style


## Realce do lacre: anel interno #e0605a no hover e no foco por teclado; fora
## disso, o anel de descanso (#c43c30). Sem moldura em volta do disco.
static func _paint_seal_ring(light: Dictionary) -> void:
	var button = light.get("button")
	var ring = light.get("ring")
	if not is_instance_valid(button) or not is_instance_valid(ring):
		return
	var hovered: bool = bool(light.get("hover", false))
	var focused: bool = bool(button.has_focus()) and keyboard_nav
	var lit: bool = hovered or focused
	ring.add_theme_stylebox_override("panel", _seal_ring_style(
			CARIMBO_BARRA if lit else STAGE_START, int(light.get("radius", 2))))


## Repinta todos os lacres vivos (usado quando o modo de navegação muda).
static func _refresh_seal_lights() -> void:
	for light in _seal_lights.duplicate():
		_paint_seal_ring(light)


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
	button.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
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
	button.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
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


# --- Foco: anel só na navegação por teclado ----------------------------------
## `true` quando a última entrada foi teclado/gamepad de NAVEGAÇÃO (Tab, setas,
## Enter, D-pad). Abrir uma tela com o mouse ou por atalho não acende anel
## nenhum; o primeiro Tab/seta devolve o anel a todos os controles.
static var keyboard_nav := false
## Controles com o anel de foco temporariamente escondido (abertura de tela).
static var _muted_focus: Array = []
static var _nav_guard: Node = null


## Observa a entrada global para saber se o jogador está no teclado. Vive no
## root da árvore, com `PROCESS_MODE_ALWAYS`, e não consome evento nenhum.
class NavGuard extends Node:
	func _ready() -> void:
		process_mode = Node.PROCESS_MODE_ALWAYS

	func _input(event: InputEvent) -> void:
		UiKit.note_input(event)


## Instala o observador uma única vez (`focus_first` e todo lacre chamam).
static func ensure_nav_guard(node: Node) -> void:
	if _nav_guard != null and is_instance_valid(_nav_guard):
		return
	if node == null or not node.is_inside_tree():
		return
	var guard := NavGuard.new()
	guard.name = "UiKitNavGuard"
	_nav_guard = guard
	node.get_tree().root.add_child.call_deferred(guard)


## Atualiza `keyboard_nav` a partir do evento. Tecla/controle de navegação liga
## o modo teclado (e devolve os anéis escondidos); clique de mouse desliga.
static func note_input(event: InputEvent) -> void:
	if event == null:
		return
	if event is InputEventKey:
		var key := event as InputEventKey
		if key.pressed and not key.echo and _is_nav_key(key):
			_enter_keyboard_nav()
	elif event is InputEventJoypadButton:
		if (event as InputEventJoypadButton).pressed:
			_enter_keyboard_nav()
	elif event is InputEventJoypadMotion:
		if absf((event as InputEventJoypadMotion).axis_value) > 0.5:
			_enter_keyboard_nav()
	elif event is InputEventMouseButton and event.pressed:
		if keyboard_nav:
			keyboard_nav = false
			_refresh_seal_lights()


static func _enter_keyboard_nav() -> void:
	if keyboard_nav and _muted_focus.is_empty():
		return
	keyboard_nav = true
	_restore_focus_rings()
	_refresh_seal_lights()


static func _is_nav_key(event: InputEventKey) -> bool:
	if event.physical_keycode == KEY_TAB or event.keycode == KEY_TAB:
		return true
	return event.keycode in [KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER,
			KEY_KP_ENTER, KEY_SPACE, KEY_PAGEUP, KEY_PAGEDOWN, KEY_HOME, KEY_END]


## Esconde o anel de foco de um controle até a primeira tecla de navegação.
## Usado ao abrir telas: o foco existe (teclado funciona), mas não acende nada.
static func mute_focus_ring(control: Control) -> void:
	if control == null or not is_instance_valid(control):
		return
	var id := control.get_instance_id()
	for entry in _muted_focus:
		if int(entry["id"]) == id:
			return
	# Telas fechadas com o mouse nunca recebem tecla: descarta as entradas mortas.
	_muted_focus = _muted_focus.filter(func(entry: Dictionary) -> bool:
		return is_instance_valid(entry["node"]))
	_muted_focus.append({
		"id": id,
		"node": control,
		"had_override": control.has_theme_stylebox_override("focus"),
		"style": control.get_theme_stylebox("focus"),
	})
	control.add_theme_stylebox_override("focus", StyleBoxEmpty.new())


## Devolve o `focus` original aos controles silenciados e limpa a lista.
static func _restore_focus_rings() -> void:
	for entry in _muted_focus:
		var control = entry["node"]
		if not is_instance_valid(control):
			continue
		if bool(entry["had_override"]) and entry["style"] != null:
			control.add_theme_stylebox_override("focus", entry["style"])
		else:
			control.remove_theme_stylebox_override("focus")
	_muted_focus.clear()


## Foca um controle sem acender anel: o realce só nasce na navegação por
## teclado. Fecha o ciclo com `mute_focus_ring`.
static func focus_silent(control: Control) -> void:
	if control == null or not is_instance_valid(control):
		return
	control.grab_focus()
	if keyboard_nav:
		return
	ensure_nav_guard(control)
	mute_focus_ring(control)


## Foca o primeiro controle ÚTIL da tela. Nunca escolhe o botão de fechar
## (`ROLE_CLOSE`): abrir uma tela não pode começar pelo lacre. Prefere busca,
## aba ativa e primeira linha da tabela; empate decidido pela ordem de leitura
## do contêiner. Sem candidato, o foco fica livre (em vez de cair no fechar).
static func focus_first(root: Node) -> void:
	if root == null:
		return
	var preferred: Control = null
	var fallback: Control = null
	var stack: Array[Node] = [root]
	while not stack.is_empty():
		var node: Node = stack.pop_front()
		if node is Control:
			var control := node as Control
			if _focus_candidate(control):
				if _is_focus_preferred(control):
					preferred = control
					break
				if fallback == null:
					fallback = control
		stack.append_array(node.get_children())

	var target: Control = preferred if preferred != null else fallback
	if target == null:
		if root.is_inside_tree():
			root.get_viewport().gui_release_focus()
		return
	focus_silent(target)


## Controle que pode receber o foco inicial: visível, focável, não desabilitado
## e não marcado como fechar.
static func _focus_candidate(control: Control) -> bool:
	if not control.is_visible_in_tree():
		return false
	if control.focus_mode != Control.FOCUS_ALL:
		return false
	if control.custom_minimum_size == Vector2.ZERO \
			and control.get_combined_minimum_size() == Vector2.ZERO:
		# Sem medida nenhuma: é respiro/placeholder, não um alvo de foco.
		return false
	if is_close(control):
		return false
	if control is BaseButton and (control as BaseButton).disabled:
		return false
	return true


static func _is_focus_preferred(control: Control) -> bool:
	if control.has_meta(UI_FOCUS_PREFERRED):
		return bool(control.get_meta(UI_FOCUS_PREFERRED))
	return control is LineEdit or control is TextEdit or control is TabBar


## `true` se o controle é o botão de fechar/lacre de uma tela.
static func is_close(control: Control) -> bool:
	return control != null and control.has_meta(UI_ROLE) \
			and String(control.get_meta(UI_ROLE)) == ROLE_CLOSE


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


# --- Bandeiras ---------------------------------------------------------------
## Bandeiras de UI: 640 px de largura, mesma arte e mesma licença dos PNGs de
## 40 px do mapa (`assets/flags/`). Ver `assets/flags/CREDITS.txt`.
const FLAGS_UI_DIR := "res://assets/flags_hi/"
## Bandeiras pequenas do atlas do mapa (w40); fallback quando falta a alta.
const FLAGS_DIR := "res://assets/flags/"

## Bandeira para UI: prefere a versão de 640 px e só cai para a de 40 px quando
## não existe alta. `null` = nação sem bandeira (nunca se desenha placeholder).
static func flag_texture(code: String) -> Texture2D:
	var tag := code.strip_edges().to_lower()
	if tag.is_empty():
		return null
	var high := FLAGS_UI_DIR + tag + ".png"
	if ResourceLoader.exists(high):
		return load(high) as Texture2D
	var small := FLAGS_DIR + tag + ".png"
	if ResourceLoader.exists(small):
		return load(small) as Texture2D
	return null

## Bandeira de uma nação do cenário pelo tag (`GBR`): o arquivo é o campo
## `flag` de `nations.json`, nunca o próprio tag.
static func nation_flag(tag: String) -> Texture2D:
	return flag_texture(String(WorldFacts.nation(tag).get("flag", "")))

## Tamanho da bandeira dentro de um envelope, na proporção nativa do país
## (Reino Unido 2:1, Suíça 1:1, Nepal ~1:1,22, Catar ~1:2,55). Nunca estica:
## o lado que não cabe é reduzido junto.
static func flag_size(texture: Texture2D, box: Vector2) -> Vector2:
	if texture == null or box.x <= 0.0 or box.y <= 0.0:
		return box
	var aspect := float(texture.get_width()) / maxf(float(texture.get_height()), 1.0)
	var width := box.y * aspect
	var height := box.y
	if width > box.x:
		width = box.x
		height = width / aspect
	return Vector2(roundf(maxf(width, 1.0)), roundf(maxf(height, 1.0)))

## Bandeira pronta para UI: moldura de tinta de 1 px, proporção do país
## preservada e filtro linear com mipmaps (a arte de 640 px encolhe limpa em
## vez de ser ampliada a partir do PNG de 40 px do mapa).
static func flag_view(texture: Texture2D, box: Vector2,
		framed: bool = true) -> Control:
	if texture == null:
		# Sem bandeira = sem desenho: nada de moldura vazia (estado vazio
		# honesto, nunca um placeholder).
		var empty := Control.new()
		empty.mouse_filter = Control.MOUSE_FILTER_IGNORE
		empty.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
		empty.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		return empty
	var holder := PanelContainer.new()
	holder.mouse_filter = Control.MOUSE_FILTER_IGNORE
	holder.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	holder.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	holder.custom_minimum_size = flag_size(texture, box)
	if framed:
		var border := StyleBoxFlat.new()
		border.bg_color = Color(0, 0, 0, 0)
		border.border_color = TINTA
		border.set_border_width_all(1)
		border.set_corner_radius_all(0)
		holder.add_theme_stylebox_override("panel", border)
	else:
		holder.add_theme_stylebox_override("panel", StyleBoxEmpty.new())
	var rect := TextureRect.new()
	rect.texture = texture
	rect.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	rect.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	rect.texture_filter = CanvasItem.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS
	rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	holder.add_child(rect)
	return holder


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
