extends Control

## Tela de Alertas no template T2 (Registro) em janela compacta.
##
## A lista é derivada do resumo real (`core/ui/alerts.gd`): déficit, estoque
## abaixo de 2 dias e cidade sem produção, priorizados. A moldura de papel vem
## da spec montada em `_spec()` — nada aqui inventa alerta, número ou cidade.
##
## Clicar (ou Enter) numa linha abre a ficha T3 da cidade na aba que explica o
## problema: mercado para falta/estoque, produção para cidade sem produção.

signal alert_requested(city_id: String, tab: String)
signal close_requested

const ALERT_MODEL: Script = preload("res://core/ui/alerts.gd")

const KIND_KEYS := {
	"deficit": "alerts.deficit",
	"low_stock": "alerts.low_stock",
	"no_production": "alerts.no_production",
}
## Aba da ficha T3 que mostra o dado do alerta.
const TAB_BY_KIND := {
	"deficit": "market",
	"low_stock": "market",
	"no_production": "production",
}
const PAGE_SIZE := 8

var _summary: Dictionary = {}
var _root: UiTemplateBuilder.TemplateRoot
var _slots := {}
var _table: UiTemplateBuilder.DataTableView
var _alerts_list: Array = []
var _structure_key := ""


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)


## A tela desenha a própria moldura de papel (template T2), sobre o mapa.
func uses_window_shell() -> bool:
	return true


# --- API ---------------------------------------------------------------------

func refresh(summary: Dictionary) -> void:
	_summary = summary
	if _root == null or _structure_key != I18n.locale:
		_rebuild()
		return
	_refresh()


# --- Montagem ----------------------------------------------------------------

func _rebuild() -> void:
	_structure_key = I18n.locale
	if _root != null and is_instance_valid(_root):
		remove_child(_root)
		_root.queue_free()
	_slots.clear()
	_table = null

	_root = UiTemplateBuilder.build(_spec())
	_root.name = "AlertsRegistry"
	_root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_root.close_requested.connect(func() -> void: close_requested.emit())
	add_child(_root)

	var pending: Array[Node] = [_root]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node is Control:
			var slot := String((node as Control).get_meta("tpl_slot", ""))
			if not slot.is_empty():
				_slots[slot] = node
		for child in node.get_children():
			pending.append(child)

	_table = _slots.get("alerts.table") as UiTemplateBuilder.DataTableView
	if _table != null:
		_table.row_activated.connect(_on_row_activated)
	_refresh()


## Spec do T2 compacto: só descreve estado; as linhas vêm do resumo publicado.
func _spec() -> Dictionary:
	return {
		"id": "alerta",
		"shell": {
			"kind": "window",
			"ratio": [0.5, 0.62],
			"min": [640, 460],
			"max": [880, 620],
			"pauses_game": false,
		},
		"data": {},
		"blocks": [
			{
				"type": "window_frame",
				"crumb": "@tpl.alerts.crumb",
				"title": "@hud.alerts",
				"title_slot": "nav.title",
				"footer": [
					{"text": "@tpl.alerts.footer"},
					{"spacer": true},
					{"seal": "ESC"},
					{"text": "@tpl.footer.close"},
				],
				"body": [
					{
						"type": "row",
						"gap": 12,
						"blocks": [
							{
								"type": "label",
								"text": "@alerts.explain",
								"font": "typed",
								"size": 13,
								"tone": "suave",
								"wrap": true,
								"wrap_width": 520,
							},
							{"type": "spacer", "w": 1},
							{
								"type": "label",
								"text": "@alerts.count",
								"params": {"count": 0},
								"font": "typed",
								"size": 13,
								"tone": "suave",
								"slot": "alerts.count",
							},
						],
					},
					{
						"type": "data_table",
						"slot": "alerts.table",
						"grow": true,
						"selected": 0,
						"page_size": PAGE_SIZE,
						"page": 1,
						"sort_col": -1,
						"footer_left": "@tpl.alerts.footer",
						"columns": [
							{"label": "@tpl.alerts.col.kind", "weight": 1.1, "align": "left"},
							{"label": "@econ.col.city", "weight": 1.9, "align": "left"},
							{"label": "@econ.col.nation", "weight": 1.3, "align": "left"},
							{"label": "@tpl.alerts.col.detail", "weight": 3, "align": "left"},
						],
						"rows": [],
					},
					{
						"type": "paper_card",
						"slot": "alerts.empty",
						"hidden": true,
						"fill": "claro",
						"border": "dashed",
						"pad": 18,
						"gap": 10,
						"blocks": [
							{
								"type": "label",
								"text": "@alerts.empty.title",
								"font": "display",
								"size": 22,
								"slot": "alerts.empty.title",
							},
							{
								"type": "label",
								"text": "@alerts.empty.body",
								"font": "body",
								"size": 15,
								"tone": "suave",
								"wrap": true,
								"wrap_width": 560,
								"slot": "alerts.empty.body",
							},
						],
					},
				],
			},
		],
	}


# --- Desenho -----------------------------------------------------------------

func _refresh() -> void:
	if _root == null:
		return
	_alerts_list = ALERT_MODEL.derive(_summary)

	var rows: Array = []
	for alert in _alerts_list:
		var city := _find_city(String(alert["city_id"]))
		var name := I18n.t("city.title") if city.is_empty() \
				else I18n.city_name(String(city["id"]), String(city["name"]))
		var nation := "" if city.is_empty() \
				else I18n.nation_name(String(city["nation"]))
		var negative := String(alert.get("tone", "warning")) == "negative"
		rows.append([
			{"text": I18n.t("alerts.tag." + String(alert["kind"])),
					"tone": "carimbo" if negative else "alerta"},
			{"text": name, "strong": true},
			{"text": nation, "tone": "suave"},
			{"text": _detail(alert), "tone": "suave"},
		])

	if _table != null:
		_table.update_rows(rows, 0 if not rows.is_empty() else -1)
	_set_hidden("alerts.table", rows.is_empty())
	_set_hidden("alerts.empty", not rows.is_empty())
	_set_text("alerts.count", I18n.t("alerts.count", {"count": rows.size()}))


## Texto do alerta com os mesmos campos do modelo (nada é recalculado aqui).
func _detail(alert: Dictionary) -> String:
	var key := String(KIND_KEYS.get(String(alert["kind"]), "alerts.no_production"))
	return I18n.t(key, {
		"amount": UiKit.number(float(alert.get("amount", 0.0))) + " t",
		"days": UiKit.number(float(alert.get("coverage", 0.0)), 1),
	})


func _on_row_activated(index: int) -> void:
	if index < 0 or index >= _alerts_list.size():
		return
	var alert: Dictionary = _alerts_list[index]
	var kind := String(alert["kind"])
	alert_requested.emit(String(alert["city_id"]),
			String(TAB_BY_KIND.get(kind, "market")))


# --- Utilidades ---------------------------------------------------------------

func _set_text(slot: String, text: String) -> void:
	var label := _slots.get(slot) as Label
	if label != null:
		label.text = text


func _set_hidden(slot: String, hidden: bool) -> void:
	var node := _slots.get(slot) as CanvasItem
	if node != null:
		node.visible = not hidden


func _find_city(city_id: String) -> Dictionary:
	for city in _summary.get("cities", []):
		if String(city["id"]) == city_id:
			return city
	return {}
