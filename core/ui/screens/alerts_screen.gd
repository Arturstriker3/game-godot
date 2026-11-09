extends VBoxContainer

## Lista de alertas econômicos priorizados. Cada linha abre a cidade
## responsável; nenhum alerta é criado fora dos dados da simulação.

signal alert_requested(city_id: String)

const ALERT_MODEL: Script = preload("res://core/ui/alerts.gd")

const KIND_KEYS := {
	"deficit": "alerts.deficit",
	"low_stock": "alerts.low_stock",
	"no_production": "alerts.no_production",
}

var _summary: Dictionary = {}
var _body: VBoxContainer
var _intro: Label
var _count: PanelContainer


func _ready() -> void:
	add_theme_constant_override("separation", UiKit.SPACE_MD)
	size_flags_vertical = Control.SIZE_EXPAND_FILL

	var head := HBoxContainer.new()
	head.add_theme_constant_override("separation", UiKit.SPACE_SM)
	_intro = UiKit.paragraph("", UiKit.FS_SMALL, UiKit.TEXT_DIM, 760.0)
	head.add_child(_intro)
	head.add_child(UiKit.spacer())
	_count = UiKit.status_pill("", "steel")
	head.add_child(_count)
	add_child(head)
	add_child(UiKit.rule())

	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	add_child(scroll)

	_body = VBoxContainer.new()
	_body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_body.add_theme_constant_override("separation", UiKit.SPACE_SM)
	scroll.add_child(_body)

	_render()


func refresh(summary: Dictionary) -> void:
	_summary = summary
	_render()


func _render() -> void:
	if _body == null:
		return
	_intro.text = I18n.t("alerts.explain")
	UiKit.clear(_body)

	var alerts: Array = ALERT_MODEL.derive(_summary)
	UiKit.apply_pill(_count, I18n.t("alerts.count", {"count": alerts.size()}),
			"negative" if not alerts.is_empty() else "positive")

	if alerts.is_empty():
		_body.add_child(UiKit.empty_state(
				I18n.t("alerts.empty.title"), I18n.t("alerts.empty.body")))
		return

	for alert in alerts:
		_body.add_child(_row(alert))


func _row(alert: Dictionary) -> Control:
	var city := _find_city(String(alert["city_id"]))
	var city_name := I18n.t("city.title") if city.is_empty() \
			else I18n.city_name(city["id"], city["name"])
	var nation := "" if city.is_empty() else I18n.nation_name(city["nation"])

	var row := UiKit.row_button()
	var box := HBoxContainer.new()
	box.add_theme_constant_override("separation", UiKit.SPACE_MD)
	row.add_child(box)

	var tone := String(alert.get("tone", "warning"))
	box.add_child(UiKit.status_pill(I18n.t("alerts.tag." + String(alert["kind"])), tone))

	var text := VBoxContainer.new()
	text.add_theme_constant_override("separation", 0)
	text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	text.add_child(UiKit.label(city_name, UiKit.FS_SUBTITLE, UiKit.TEXT))
	text.add_child(UiKit.paragraph(
			I18n.t(KIND_KEYS[String(alert["kind"])], {
				"amount": UiKit.number(float(alert.get("amount", 0.0))) + " t",
				"days": "%.1f" % float(alert.get("coverage", 0.0)),
			}), UiKit.FS_CAPTION + 1, UiKit.TEXT_DIM, 420.0))
	box.add_child(text)

	box.add_child(UiKit.spacer())
	if not nation.is_empty():
		box.add_child(UiKit.caption(nation, UiKit.nation_color(String(city["nation"]))))
	box.add_child(UiKit.caption(I18n.t("alerts.open"), UiKit.BRASS))

	var city_id := String(alert["city_id"])
	row.pressed.connect(func() -> void: alert_requested.emit(city_id))
	return row


func _find_city(city_id: String) -> Dictionary:
	for city in _summary.get("cities", []):
		if String(city["id"]) == city_id:
			return city
	return {}
