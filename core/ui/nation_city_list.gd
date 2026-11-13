extends PanelContainer
class_name NationCityList

## Ficha manila recolhível com as cidades da nação do jogador, colada à borda
## direita abaixo da topbar (acima do minimapa).
##
## Ordem: população decrescente (quem chama já ordena e formata). Um clique
## centraliza a câmera (mesmo pedido do minimapa, via `city_focused`) e o duplo
## clique abre a ficha T3 (`city_activated`). O ícone de alerta só aparece
## quando o dia publicado tem déficit; nenhum estado é inventado.
##
## Como o minimapa, a ficha some enquanto uma sobreposição do HUD está aberta
## (regra em `hud.gd::_sync_map_overlays`).

signal city_clicked(city_id: String)
signal city_activated(city_id: String)

## Largura da ficha: a mesma folha do minimapa (288 px) para alinhar as bordas.
const WIDTH := 288.0
const HEADER_HEIGHT := 34.0
const ROW_HEIGHT := 30.0
const PAD_X := 8.0

## Respingo de tinta do alerta (déficit/fome) e do cabeçalho recolhível.
const ALERT_COLOR := UiKit.CARIMBO

var _header: Button
var _body: VBoxContainer
var _rows: Array[Control] = []
var _title := ""
var _collapsed := false


func _init() -> void:
	mouse_filter = Control.MOUSE_FILTER_STOP
	custom_minimum_size = Vector2(WIDTH, 0.0)
	add_theme_stylebox_override("panel", _sheet_style())

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 0)
	box.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(box)

	_header = Button.new()
	_header.custom_minimum_size = Vector2(WIDTH, HEADER_HEIGHT)
	_header.alignment = HORIZONTAL_ALIGNMENT_LEFT
	_header.focus_mode = Control.FOCUS_ALL
	_header.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
	_header.add_theme_font_override("font", UiKit.PAPER_DISPLAY_FONT)
	_header.add_theme_font_size_override("font_size", 14)
	_header.add_theme_color_override("font_color", UiKit.TINTA)
	_header.add_theme_stylebox_override("normal", _header_style(false))
	_header.add_theme_stylebox_override("hover", _header_style(true))
	_header.add_theme_stylebox_override("pressed", _header_style(true))
	_header.add_theme_stylebox_override("focus", UiKit.outline(UiKit.CARIMBO))
	_header.pressed.connect(func() -> void: set_collapsed(not _collapsed))
	box.add_child(_header)

	_body = VBoxContainer.new()
	_body.add_theme_constant_override("separation", 0)
	_body.mouse_filter = Control.MOUSE_FILTER_IGNORE
	box.add_child(_body)


## Ficha manila: papel pardo, contorno de tinta e sombra suave (a mesma do
## minimapa, para as duas fichas lerem como o mesmo objeto).
static func _sheet_style() -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = UiKit.MANILA
	style.border_color = UiKit.TINTA
	style.set_border_width_all(1)
	style.set_corner_radius_all(0)
	style.shadow_color = Color(0, 0, 0, 0.35)
	style.shadow_size = 5
	style.shadow_offset = Vector2(2.0, 2.0)
	return style


static func _header_style(hover: bool) -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = UiKit.MANILA_LIGHT if hover else UiKit.MANILA
	style.border_color = UiKit.TINTA
	style.border_width_bottom = 1
	style.content_margin_left = PAD_X
	style.content_margin_right = PAD_X
	return style


## Substitui a lista. `rows` traz itens já formatados:
## `{"id", "name", "population", "alert"}`.
func set_rows(title: String, rows: Array) -> void:
	_title = title
	_sync_header()
	_header.tooltip_text = I18n.t("hud.city_list.expand" if _collapsed \
			else "hud.city_list.collapse")

	for child in _body.get_children():
		_body.remove_child(child)
		child.queue_free()
	_rows.clear()

	if rows.is_empty():
		var empty := Label.new()
		empty.text = I18n.t("hud.city_list.empty")
		empty.add_theme_font_override("font", UiKit.PAPER_BODY_FONT)
		empty.add_theme_font_size_override("font_size", 13)
		empty.add_theme_color_override("font_color", UiKit.TINTA_SUAVE)
		empty.custom_minimum_size = Vector2(WIDTH, ROW_HEIGHT)
		empty.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		empty.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
		empty.mouse_filter = Control.MOUSE_FILTER_IGNORE
		_body.add_child(empty)
	else:
		for row in rows:
			_body.add_child(_build_row(row))

	_body.visible = not _collapsed
	_sync_size()


func set_collapsed(collapsed: bool) -> void:
	if collapsed == _collapsed:
		return
	_collapsed = collapsed
	_sync_header()
	_body.visible = not _collapsed
	_sync_size()


func is_collapsed() -> bool:
	return _collapsed


func row_count() -> int:
	return _rows.size()


func row_at(index: int) -> Control:
	return _rows[index] if index >= 0 and index < _rows.size() else null


## Altura acompanha o conteúdo: ancorada pelo topo no HUD, a ficha cresce para
## baixo quando expande e encolhe quando recolhe, sem subir sobre a topbar.
func _sync_size() -> void:
	# A altura mínima própria precisa ser zerada antes de medir: se ficasse no
	# valor expandido, o `max(custom, conteúdo)` nunca deixaria a ficha recolher.
	custom_minimum_size.y = 0.0
	var min_h := get_combined_minimum_size().y
	custom_minimum_size.y = min_h
	offset_bottom = offset_top + min_h


func _sync_header() -> void:
	_header.text = ("▸  " if _collapsed else "▾  ") + _title
	_header.tooltip_text = I18n.t("hud.city_list.expand" if _collapsed \
			else "hud.city_list.collapse")


func _build_row(data: Dictionary) -> Control:
	var row := CityRow.new(String(data.get("id", "")))
	row.pressed.connect(func(id: String) -> void: city_clicked.emit(id))
	row.activated.connect(func(id: String) -> void: city_activated.emit(id))

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", int(PAD_X))
	margin.add_theme_constant_override("margin_right", int(PAD_X))
	margin.mouse_filter = Control.MOUSE_FILTER_IGNORE
	row.add_child(margin)

	var line := HBoxContainer.new()
	line.add_theme_constant_override("separation", 6)
	line.mouse_filter = Control.MOUSE_FILTER_IGNORE
	margin.add_child(line)

	var name_label := _label(String(data.get("name", "")), 13, UiKit.TINTA)
	name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	line.add_child(name_label)

	var population := _label(String(data.get("population", "")), 13,
			UiKit.TINTA_SUAVE)
	population.custom_minimum_size.x = 62.0
	population.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	line.add_child(population)

	var alert := Label.new()
	alert.text = "!" if bool(data.get("alert", false)) else ""
	alert.custom_minimum_size.x = 12.0
	alert.add_theme_font_override("font", UiKit.PAPER_DISPLAY_FONT)
	alert.add_theme_font_size_override("font_size", 14)
	alert.add_theme_color_override("font_color", ALERT_COLOR)
	alert.tooltip_text = I18n.t("hud.city_list.alert")
	alert.mouse_filter = Control.MOUSE_FILTER_IGNORE
	line.add_child(alert)

	_rows.append(row)
	return row


static func _label(text: String, size: int, color: Color) -> Label:
	var node := Label.new()
	node.text = text
	node.add_theme_font_override("font", UiKit.PAPER_BODY_FONT)
	node.add_theme_font_size_override("font_size", size)
	node.add_theme_color_override("font_color", color)
	node.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return node


## Linha clicável (um clique centraliza, duplo abre a ficha). Altura mínima de
## 30 px e foco visível, como o resto do design system.
class CityRow extends PanelContainer:
	signal pressed(city_id: String)
	signal activated(city_id: String)

	var city_id := ""

	func _init(id: String) -> void:
		city_id = id
		custom_minimum_size = Vector2(0.0, ROW_HEIGHT)
		mouse_filter = Control.MOUSE_FILTER_STOP
		mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
		focus_mode = Control.FOCUS_ALL
		add_theme_stylebox_override("panel", _row_style(false, false))
		mouse_entered.connect(func() -> void: _paint(true))
		mouse_exited.connect(func() -> void: _paint(false))
		focus_entered.connect(func() -> void:
			add_theme_stylebox_override("panel", _row_style(true, true)))
		focus_exited.connect(func() -> void: _paint(false))

	func _paint(hover: bool) -> void:
		add_theme_stylebox_override("panel",
				_row_style(hover, has_focus()))

	## Linha sem moldura no repouso; papel claro no hover; borda de carimbo no
	## foco (anel de 1 px, como o resto do design system).
	func _row_style(hover: bool, focused: bool) -> StyleBoxFlat:
		var style := StyleBoxFlat.new()
		style.bg_color = Color(UiKit.PAPEL_CLARO, 0.85) if hover \
				else Color(0, 0, 0, 0)
		style.border_color = UiKit.CARIMBO if focused else Color(0, 0, 0, 0)
		style.set_border_width_all(1 if focused else 0)
		style.set_corner_radius_all(0)
		return style

	func _gui_input(event: InputEvent) -> void:
		if event is InputEventMouseButton \
				and (event as InputEventMouseButton).button_index == MOUSE_BUTTON_LEFT \
				and (event as InputEventMouseButton).pressed:
			if (event as InputEventMouseButton).double_click:
				activated.emit(city_id)
			else:
				pressed.emit(city_id)
			accept_event()
		elif event is InputEventKey and (event as InputEventKey).pressed \
				and not (event as InputEventKey).echo \
				and (event as InputEventKey).keycode in [KEY_ENTER, KEY_SPACE]:
			pressed.emit(city_id)
			accept_event()
