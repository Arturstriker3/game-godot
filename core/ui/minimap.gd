extends Control
class_name Minimap

## Minimapa "Papelaria de guerra" no canto inferior direito.
##
## Folha de papel com contorno TINTA de 1 px + sombra suave, cabeçalho "MUNDO"
## compacto, mapa político global em tom sépia (2:1, sem rótulos de coordenada),
## retângulo de área vista em `CARIMBO` e abas verticais de tipo de mapa à
## esquerda da folha (VBox com separação fixa). Usa a imagem gerada pelo mesmo
## pipeline do mapa (tools/build_atlas_layers.py -> minimap.png, projeção
## equiretangular −90°..90° lat, −180°..180° lon), sem segunda câmera nem
## re-render do mundo. Ao clicar, pede para a câmera ir até o ponto; trabalha
## em UV do mapa (0..1) e a conversão para o mundo fica com main_game.gd.

signal point_requested(map_uv: Vector2)
signal mode_requested(mode: String)

const MAP_TEXTURE: Texture2D = preload("res://map/map_data/atlas/minimap.png")

## Tamanho com que a textura do mapa é desenhada, na proporção 2:1 da textura
## (512×256). A moldura externa é determinística (226×118): 1 px de TINTA + 4 px
## de papel em volta da textura, centralizada. Sem os rótulos de coordenada, a
## textura cresceu para ocupar a faixa de papel que antes ficava reservada a eles.
const MAP_SIZE := Vector2(216.0, 108.0)

# --- Tokens de layout (px) ----------------------------------------------------
const TAB_W := 34.0
const TAB_H := 30.0
const TAB_GAP := 4.0        # separação fixa entre abas (VBox)
const PAPER_MARGIN := 14.0  # margem interna da folha
const FRAME_BORDER := 1.0   # moldura fina TINTA do mapa
const FRAME_PAD := 4.0      # papel entre a moldura e a textura (3-4 px)
const TITLE_HEIGHT := 14.0  # cabeçalho compacto (era 22)
const HEADER_GAP := 2.0     # respiro vertical reduzido
const RULE_HEIGHT := 1.0    # filete TINTA de 1 px (era 2)
const HEADER_HEIGHT := TITLE_HEIGHT + HEADER_GAP + RULE_HEIGHT + HEADER_GAP
const TITLE_SIZE := 10      # ~70% de 14

## Tom sépia quente aplicado à textura (modulate), para casar com o papel.
const SEPIA := Color(1.0, 0.92, 0.78)

var _image: TextureRect
var _view: Panel
var _view_uv := Rect2(0.45, 0.2, 0.05, 0.05)
var _map_origin := Vector2.ZERO
var _title: Label
var _tab_box: VBoxContainer
var _frame_y := 0.0
var _sheet_h := 0.0
var _mode_buttons := {}
var _mode_icons := {}
var _active_mode := ""


## Tamanho total do controle (abas + folha). Determinístico, independente de
## `_ready`, para o HUD posicionar o nó antes de adicioná-lo à árvore.
static func total_size() -> Vector2:
	var frame := MAP_SIZE + Vector2(FRAME_BORDER + FRAME_PAD, FRAME_BORDER + FRAME_PAD) * 2.0
	return Vector2(TAB_W + PAPER_MARGIN * 2.0 + frame.x,
			PAPER_MARGIN * 2.0 + HEADER_HEIGHT + frame.y)


func _ready() -> void:
	custom_minimum_size = total_size()
	mouse_filter = Control.MOUSE_FILTER_STOP
	clip_contents = false

	var frame_size := MAP_SIZE + Vector2(FRAME_BORDER + FRAME_PAD, FRAME_BORDER + FRAME_PAD) * 2.0
	var sheet_w := PAPER_MARGIN * 2.0 + frame_size.x
	var sheet_h := PAPER_MARGIN * 2.0 + HEADER_HEIGHT + frame_size.y
	var sheet_x := TAB_W
	var frame_x := sheet_x + PAPER_MARGIN
	var frame_y := PAPER_MARGIN + HEADER_HEIGHT
	_frame_y = frame_y
	_sheet_h = sheet_h
	_map_origin = Vector2(frame_x + FRAME_BORDER + FRAME_PAD, frame_y + FRAME_BORDER + FRAME_PAD)

	# Folha de papel com contorno TINTA de 1 px e sombra suave, via StyleBox.
	var sheet := Panel.new()
	sheet.position = Vector2(sheet_x, 0.0)
	sheet.size = Vector2(sheet_w, sheet_h)
	sheet.mouse_filter = Control.MOUSE_FILTER_IGNORE
	sheet.add_theme_stylebox_override("panel", _sheet_style())
	add_child(sheet)

	# Textura do papel por dentro do contorno (inset 1 px).
	var paper := Panel.new()
	paper.position = Vector2(1.0, 1.0)
	paper.size = Vector2(sheet_w - 2.0, sheet_h - 2.0)
	paper.mouse_filter = Control.MOUSE_FILTER_IGNORE
	paper.add_theme_stylebox_override("panel", UiKit.paper_stylebox())
	sheet.add_child(paper)

	# Cabeçalho compacto: título + filete de 1 px. Nada à direita do título.
	_title = Label.new()
	_title.position = Vector2(frame_x, PAPER_MARGIN)
	_title.size = Vector2(frame_size.x, TITLE_HEIGHT)
	_title.add_theme_font_override("font", UiKit.PAPER_DISPLAY_FONT)
	_title.add_theme_font_size_override("font_size", TITLE_SIZE)
	_title.add_theme_color_override("font_color", UiKit.TINTA)
	_title.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_title)

	var rule := ColorRect.new()
	rule.position = Vector2(frame_x, PAPER_MARGIN + TITLE_HEIGHT + HEADER_GAP)
	rule.size = Vector2(frame_size.x, RULE_HEIGHT)
	rule.color = UiKit.TINTA
	rule.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(rule)

	# Moldura fina TINTA em volta do mapa.
	var frame := Panel.new()
	frame.position = Vector2(frame_x, frame_y)
	frame.size = frame_size
	frame.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var frame_style := StyleBoxFlat.new()
	frame_style.bg_color = Color(0, 0, 0, 0)
	frame_style.border_color = UiKit.TINTA
	frame_style.set_border_width_all(FRAME_BORDER)
	frame_style.set_corner_radius_all(0)
	frame.add_theme_stylebox_override("panel", frame_style)
	add_child(frame)

	# Textura do mapa com tom sépia.
	_image = TextureRect.new()
	_image.texture = MAP_TEXTURE
	_image.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_image.stretch_mode = TextureRect.STRETCH_SCALE
	_image.position = _map_origin
	_image.size = MAP_SIZE
	_image.modulate = SEPIA
	_image.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_image)

	# Retângulo vazado da área vista: CARIMBO, 2 px.
	_view = Panel.new()
	_view.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var view_style := StyleBoxFlat.new()
	view_style.bg_color = Color(0, 0, 0, 0)
	view_style.border_color = UiKit.CARIMBO
	view_style.set_border_width_all(2)
	view_style.set_corner_radius_all(0)
	_view.add_theme_stylebox_override("panel", view_style)
	add_child(_view)
	_layout_view()

	# Pilha de abas de tipo de mapa à esquerda da folha. VBox alinhada ao topo da
	# área do mapa (primeira aba na borda superior da moldura); os modos são
	# construídos por `set_map_modes`, então adicionar um modo não mexe no layout.
	_tab_box = VBoxContainer.new()
	_tab_box.position = Vector2(0.0, frame_y)
	_tab_box.size = Vector2(TAB_W, 0.0)
	_tab_box.add_theme_constant_override("separation", TAB_GAP)
	_tab_box.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_tab_box)


## Contorno de 1 px em TINTA + sombra suave em volta da folha de papel, via
## StyleBox (reusa a sombra suave do design system; não desenha à mão).
func _sheet_style() -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = UiKit.PAPEL
	style.border_color = UiKit.TINTA
	style.set_border_width_all(1)
	style.set_corner_radius_all(0)
	style.shadow_color = Color(0, 0, 0, 0.35)
	style.shadow_size = 5
	style.shadow_offset = Vector2(2.0, 2.0)
	return style


## Título do cabeçalho (ex.: "MUNDO"), localizado pelo HUD.
func set_title(text: String) -> void:
	if _title != null:
		_title.text = text.to_upper()


## Constrói as abas verticais de tipo de mapa. `icons` mapeia modo → textura;
## `tooltips` mapeia modo → texto localizado. Reconstruído ao trocar idioma.
## As abas são filhos de um VBox com separação fixa; adicionar um modo só
## acrescenta um item, sem alterar o layout.
func set_map_modes(modes: Array, icons: Dictionary, tooltips: Dictionary, active: String) -> void:
	for button in _mode_buttons.values():
		if is_instance_valid(button):
			button.queue_free()
	_mode_buttons.clear()
	_mode_icons.clear()
	if _tab_box == null:
		return

	for mode in modes:
		var mode_name := String(mode)
		var button := Button.new()
		button.focus_mode = Control.FOCUS_NONE
		button.custom_minimum_size = Vector2(TAB_W, TAB_H)
		button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		button.tooltip_text = String(tooltips.get(mode_name, ""))
		button.pressed.connect(_on_tab_pressed.bind(mode_name))

		var icon := TextureRect.new()
		icon.texture = icons.get(mode_name)
		icon.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
		icon.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
		icon.mouse_filter = Control.MOUSE_FILTER_IGNORE
		icon.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
		icon.offset_left = 7.0
		icon.offset_top = 6.0
		icon.offset_right = -7.0
		icon.offset_bottom = -6.0
		button.add_child(icon)

		_tab_box.add_child(button)
		_mode_buttons[mode_name] = button
		_mode_icons[mode_name] = icon

	_layout_tabs()

	_active_mode = ""
	set_active_mode(active)


## Posiciona a pilha de abas: alinhada ao topo da área do mapa; se a pilha
## crescer além da folha, sobe (cresce para cima) para continuar legível e
## dentro da tela.
func _layout_tabs() -> void:
	if _tab_box == null:
		return
	var count := _tab_box.get_child_count()
	var stack_h := count * TAB_H + maxf(0.0, count - 1) * TAB_GAP
	var y := _frame_y
	if y + stack_h > _sheet_h:
		y = _sheet_h - stack_h
	_tab_box.position = Vector2(0.0, y)
	_tab_box.size = Vector2(TAB_W, stack_h)


func _on_tab_pressed(mode: String) -> void:
	mode_requested.emit(mode)


## Restiliza as abas conforme o modo ativo.
func set_active_mode(mode: String) -> void:
	if mode == _active_mode and _active_mode != "":
		return
	_active_mode = mode
	for mode_name in _mode_buttons:
		var active: bool = mode_name == mode
		var button: Button = _mode_buttons[mode_name]
		var icon: TextureRect = _mode_icons[mode_name]
		button.add_theme_stylebox_override("normal", _tab_style(active, false))
		button.add_theme_stylebox_override("hover", _tab_style(active, true))
		button.add_theme_stylebox_override("pressed", _tab_style(true, false))
		icon.modulate = UiKit.BARRA_TEXTO if active else UiKit.TINTA_SUAVE


## Aba colada à folha: sem borda à direita, cantos arredondados só à esquerda,
## sombra leve. Ativa = tinta sólida; inativa = papel ABA_INATIVA.
func _tab_style(active: bool, hover: bool) -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	if active:
		style.bg_color = UiKit.TINTA
		style.border_color = UiKit.TINTA
	elif hover:
		style.bg_color = UiKit.PAPEL
		style.border_color = UiKit.TINTA
	else:
		style.bg_color = UiKit.ABA_INATIVA
		style.border_color = UiKit.TINTA
	style.border_width_left = 1
	style.border_width_top = 1
	style.border_width_bottom = 1
	style.border_width_right = 0
	style.corner_radius_top_left = 6
	style.corner_radius_bottom_left = 6
	style.corner_radius_top_right = 0
	style.corner_radius_bottom_right = 0
	style.shadow_color = Color(0, 0, 0, 0.35)
	style.shadow_size = 5
	style.shadow_offset = Vector2(-2.0, 2.0)
	return style


## Troca a textura do minimapa (território dinâmico). Preserva o retângulo de
## view e o clique/arraste; apenas substitui a imagem de fundo.
func set_territory_texture(texture: Texture2D) -> void:
	if _image != null and texture != null:
		_image.texture = texture


## Atualiza a área vista: centro e tamanho em UV do mapa.
func set_view(center_uv: Vector2, size_uv: Vector2) -> void:
	center_uv.x = fposmod(center_uv.x, 1.0)
	_view_uv = Rect2(center_uv - size_uv * 0.5, size_uv)
	_layout_view()


func _layout_view() -> void:
	if _view == null:
		return
	var min_size := Vector2(6.0, 4.0)
	var rect_size := (_view_uv.size * MAP_SIZE).max(min_size)
	var center := _map_origin + (_view_uv.position + _view_uv.size * 0.5) * MAP_SIZE
	var pos := center - rect_size * 0.5
	pos.x = clampf(pos.x, _map_origin.x, _map_origin.x + MAP_SIZE.x - rect_size.x)
	pos.y = clampf(pos.y, _map_origin.y, _map_origin.y + MAP_SIZE.y - rect_size.y)
	_view.position = pos
	_view.size = rect_size


func _gui_input(event: InputEvent) -> void:
	var pressed := event is InputEventMouseButton \
			and (event as InputEventMouseButton).button_index == MOUSE_BUTTON_LEFT \
			and (event as InputEventMouseButton).pressed
	var dragged := event is InputEventMouseMotion \
			and ((event as InputEventMouseMotion).button_mask & MOUSE_BUTTON_MASK_LEFT) != 0
	if not (pressed or dragged):
		return
	var local := get_local_mouse_position()
	if not Rect2(_map_origin, MAP_SIZE).has_point(local):
		return
	var map_uv := (local - _map_origin) / MAP_SIZE
	point_requested.emit(map_uv.clamp(Vector2.ZERO, Vector2.ONE))
	accept_event()
