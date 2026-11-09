extends VBoxContainer

## Janela de Produção e Construção.
##
## A simulação ainda não tem custo de obra nem fila de produção, então esta
## tela mostra o estado vazio com honestidade, em vez de números inventados.

var _tab := 0
var _tab_buttons: Array[Button] = []
var _body: VBoxContainer


func _ready() -> void:
	add_theme_constant_override("separation", 12)
	size_flags_vertical = Control.SIZE_EXPAND_FILL

	add_child(_build_tabs())
	add_child(UiKit.rule())

	_body = VBoxContainer.new()
	_body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_body.alignment = BoxContainer.ALIGNMENT_CENTER
	add_child(_body)

	_render()


func _build_tabs() -> HBoxContainer:
	var bar := UiKit.tab_bar()
	for index in 2:
		var button := UiKit.tab_button("")
		button.pressed.connect(_set_tab.bind(index))
		bar.add_child(button)
		_tab_buttons.append(button)
	bar.add_child(UiKit.spacer())
	return bar


func refresh(_summary: Dictionary) -> void:
	_retranslate()
	_render()


func _retranslate() -> void:
	if _tab_buttons.size() < 2:
		return
	_tab_buttons[0].text = I18n.t("prod.tab.queue")
	_tab_buttons[1].text = I18n.t("prod.tab.facilities")


func _set_tab(index: int) -> void:
	_tab = index
	_render()


func _render() -> void:
	if not is_node_ready() or _body == null:
		return

	for i in _tab_buttons.size():
		UiKit.set_tab_active(_tab_buttons[i], i == _tab)

	UiKit.clear(_body)

	var heading := I18n.t("prod.empty.title") if _tab == 0 else I18n.t("prod.tab.facilities")
	var body := I18n.t("prod.empty.body") if _tab == 0 else I18n.t("prod.facilities.body")
	_body.add_child(_empty_state(heading, body))


func _empty_state(heading: String, body: String) -> Control:
	var center := CenterContainer.new()
	center.size_flags_horizontal = Control.SIZE_EXPAND_FILL

	var panel := PanelContainer.new()
	panel.custom_minimum_size = Vector2(520.0, 0.0)
	var style := UiKit.flat(UiKit.PANEL, UiKit.EDGE_SOFT, 24.0)
	style.content_margin_top = 22.0
	style.content_margin_bottom = 22.0
	panel.add_theme_stylebox_override("panel", style)
	center.add_child(panel)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 10)
	panel.add_child(box)

	box.add_child(UiKit.title(heading, 20))

	var text := UiKit.label(body, 13, UiKit.TEXT_DIM)
	text.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	text.custom_minimum_size.x = 460.0
	box.add_child(text)

	return center
