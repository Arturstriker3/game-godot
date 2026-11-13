extends Control

## Estatísticas no template T5 (Quadro comparativo).
##
## Ranking entre nações por população, PIB (PPC), comida (autossuficiência de
## trigo) e reservas minerais. Os grupos são Mundo ou a região do cenário
## (`nations.csv`); G7/G20 não existem como lista nos dados, então não aparecem.
## A posição da nação do jogador é destacada no ranking e no cartão "Sua
## posição".
##
## Sem histórico de 12 meses a partida não tem série: o cartão de tendência
## mostra o estado vazio em vez de uma linha inventada.

signal close_requested

const SPEC_ID := "quadro"
const PLAYER_NATION := "GBR"
const TOP_ROWS := 7

const INDICATORS := [
	{"key": "population", "label": "@tpl.quadro.ind.population"},
	{"key": "gdp", "label": "@tpl.quadro.ind.gdp"},
	{"key": "food", "label": "@tpl.quadro.ind.food"},
	{"key": "minerals", "label": "@tpl.quadro.ind.minerals"},
]

var _summary: Dictionary = {}
var _indicator := 0
var _group := 0
var _groups: Array = []  # [{"key": "world"/region, "label": chave i18n}]
var _history: RefCounted = null

## Indicador do ranking → campo da série mensal por nação.
const TREND_FIELDS := {
	"population": "population",
	"gdp": "gdp",
	"food": "food",
	"minerals": "reserves",
}

var _root: UiTemplateBuilder.TemplateRoot
var _slots := {}
var _structural := ""
var _rows_box: VBoxContainer


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build_groups()


func uses_window_shell() -> bool:
	return true


# --- API ---------------------------------------------------------------------

func refresh(summary: Dictionary) -> void:
	_summary = summary
	if _root == null or _structural != I18n.locale:
		_rebuild()
		return
	_refresh()


## Serviço de histórico de 12 meses, fornecido pelo HUD. Sem ele (ou com menos
## de 2 meses amostrados), a tendência mostra o estado vazio honesto.
func set_history_service(service: RefCounted) -> void:
	_history = service


func set_indicator(index: int) -> void:
	_indicator = clampi(index, 0, INDICATORS.size() - 1)
	_refresh()


func set_group(index: int) -> void:
	_group = clampi(index, 0, maxi(_groups.size() - 1, 0))
	_refresh()


func current_indicator() -> String:
	return String(INDICATORS[_indicator]["key"])


# --- Montagem ----------------------------------------------------------------

func _build_groups() -> void:
	_groups = [{"key": "world", "label": "tpl.quadro.group.world"}]
	for region in WorldFacts.REGIONS:
		_groups.append({"key": region, "label": "tpl.region.%s" % region})


func _rebuild() -> void:
	_structural = I18n.locale
	if _root != null and is_instance_valid(_root):
		remove_child(_root)
		_root.queue_free()
	_slots.clear()
	_rows_box = null

	_root = UiTemplateBuilder.build(_patch_spec(
			UiTemplateBuilder.load_spec(SPEC_ID)))
	_root.name = "StatisticsBoard"
	_root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_root.close_requested.connect(func() -> void: close_requested.emit())
	add_child(_root)
	_index_slots()
	_refresh()


## Troca o bloco de filtros e o miolo do T5 pelos blocos ligados ao estado da
## tela (indicador, grupo, ranking e posição). O envelope da spec é preservado.
func _patch_spec(spec: Dictionary) -> Dictionary:
	var blocks: Array = spec.get("blocks", [])
	if blocks.is_empty():
		return spec
	var frame: Dictionary = blocks[0]
	var body: Array = frame.get("body", [])
	if body.is_empty():
		return spec
	var column: Dictionary = body[0]
	column["blocks"] = [_indicator_block(), _content_block()]
	return spec


func _indicator_block() -> Dictionary:
	var chips: Array = []
	for index in INDICATORS.size():
		chips.append({
			"type": "chip_filter", "text": String(INDICATORS[index]["label"]),
			"active": index == _indicator, "slot": "ind.%d" % index,
		})
	var group_chips: Array = []
	for index in _groups.size():
		group_chips.append({
			"type": "chip_filter",
			"text": String(_groups[index]["label"]),
			"active": index == _group, "slot": "group.%d" % index,
		})
	return {
		"type": "column", "gap": 8,
		"blocks": [
			{"type": "row", "gap": 8, "blocks": [
				{"type": "label", "text": "@tpl.quadro.indicator",
						"font": "typed", "size": 13, "tone": "suave"},
			] + _chips_with_slots(chips, "ind")},
			{"type": "row", "gap": 8, "blocks": [
				{"type": "label", "text": "@tpl.quadro.group",
						"font": "typed", "size": 13, "tone": "suave"},
			] + _chips_with_slots(group_chips, "group")},
		],
	}


func _chips_with_slots(chips: Array, prefix: String) -> Array:
	var out: Array = []
	for index in chips.size():
		var chip: Dictionary = chips[index]
		chip["slot"] = "%s.%d" % [prefix, index]
		out.append(chip)
	# O último chip absorve a sobra da linha.
	if not out.is_empty():
		out.append({"type": "spacer", "w": 1})
	return out


func _content_block() -> Dictionary:
	return {
		"type": "row", "gap": 16, "w": 1, "grow": true,
		"blocks": [
			{
				"type": "paper_card", "pad": 20, "gap": 10, "w": 1.4,
				"grow": true,
				"blocks": [
					{"type": "ruled_heading", "text": "@tpl.quadro.ranking",
							"size": 20, "slot": "ranking.title"},
					{"type": "column", "slot": "ranking.rows", "gap": 2,
							"blocks": []},
					{"type": "spacer", "grow": true},
					{"type": "label", "text": "@tpl.quadro.source",
							"font": "typed", "size": 12, "tone": "suave",
							"wrap": true, "wrap_width": 500,
							"slot": "ranking.source"},
				],
			},
			{
				"type": "column", "gap": 16, "w": 1, "grow": true,
				"blocks": [
					{
						"type": "paper_card", "pad": 20, "gap": 10,
						"blocks": [
							{"type": "ruled_heading",
									"text": "@tpl.quadro.position", "size": 20},
							{"type": "row", "gap": 12, "blocks": [
								{"type": "row", "gap": 2, "blocks": [
									{"type": "label", "text": "—",
											"font": "display", "size": 56,
											"tone": "carimbo",
											"slot": "position.rank"},
									{"type": "label",
											"text": "@tpl.quadro.rank_suffix",
											"font": "display", "size": 30,
											"tone": "carimbo"},
								]},
								{"type": "label",
										"text": "@tpl.quadro.position.note",
										"font": "typed", "size": 15,
										"tone": "suave", "wrap": true,
										"wrap_width": 430,
										"slot": "position.note"},
							]},
							{"type": "kv_row", "key": "@tpl.quadro.above",
									"value": "—", "value_slot": "position.above"},
							{"type": "kv_row", "key": "@tpl.quadro.below",
									"value": "—", "value_slot": "position.below"},
						],
					},
					{
						"type": "paper_card", "pad": 20, "gap": 10,
						"grow": true,
						"blocks": [
							{"type": "ruled_heading",
									"text": "@tpl.quadro.trend", "size": 20},
							{
								"type": "column", "slot": "trend.series",
								"gap": 6, "hidden": true,
								"blocks": [
									{"type": "line_chart", "height": 128,
											"slot": "trend.chart",
											"tone": "tinta",
											"autoscale": true},
									{"type": "label", "text": "",
											"font": "typed", "size": 12,
											"tone": "suave",
											"slot": "trend.range"},
								],
							},
							{
								"type": "paper_card", "slot": "trend.empty",
								"border": "dashed", "fill": "papel",
								"pad": 14, "gap": 8,
								"blocks": [
									{"type": "label",
											"text": "@tpl.quadro.trend.empty.title",
											"font": "display", "size": 20,
											"slot": "trend.empty.title"},
									{"type": "label",
											"text": "@tpl.quadro.trend.empty.body",
											"font": "typed", "size": 13,
											"tone": "suave", "wrap": true,
											"wrap_width": 400,
											"slot": "trend.empty.body"},
								],
							},
							{"type": "label", "text": "@tpl.quadro.trend.note",
									"font": "typed", "size": 12,
									"tone": "suave", "wrap": true,
									"wrap_width": 400, "slot": "trend.note"},
							{"type": "spacer", "grow": true},
						],
					},
				],
			},
		],
	}


func _index_slots() -> void:
	var pending: Array[Node] = [_root]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node is Control:
			var slot := String((node as Control).get_meta("tpl_slot", ""))
			if not slot.is_empty():
				_slots[slot] = node
		for child in node.get_children():
			pending.append(child)

	_rows_box = _slots.get("ranking.rows") as VBoxContainer
	for index in INDICATORS.size():
		var chip := _slots.get("ind.%d" % index) as Button
		if chip != null:
			chip.pressed.connect(set_indicator.bind(index))
	for index in _groups.size():
		var chip := _slots.get("group.%d" % index) as Button
		if chip != null:
			chip.pressed.connect(set_group.bind(index))


# --- Desenho -----------------------------------------------------------------

func _refresh() -> void:
	if _root == null:
		return
	_refresh_chips()
	_refresh_ranking()
	_refresh_position()
	_refresh_trend()


func _refresh_chips() -> void:
	for index in INDICATORS.size():
		var chip := _slots.get("ind.%d" % index) as Button
		if chip != null:
			UiTemplateBuilder.set_chip_active(chip, index == _indicator)
	var counts := _group_counts()
	for index in _groups.size():
		var chip := _slots.get("group.%d" % index) as Button
		if chip == null:
			continue
		var label := I18n.t(String(_groups[index]["label"]))
		var total: int = int(counts.get(String(_groups[index]["key"]), 0))
		chip.text = "%s · %s" % [label, UiKit.number(total)]
		UiTemplateBuilder.set_chip_active(chip, index == _group)


func _refresh_ranking() -> void:
	if _rows_box == null:
		return
	UiKit.clear(_rows_box)
	var all := _ranked_rows()
	var group := _group_rows(all)
	var top := group.slice(0, mini(TOP_ROWS, group.size()))
	var player_group_index := _index_of_tag(group, PLAYER_NATION)
	if player_group_index >= TOP_ROWS and player_group_index >= 0:
		top.append(group[player_group_index])
	var peak := float(top[0]["value"]) if not top.is_empty() else 0.0

	for row in top:
		var value := float(row["value"])
		var block := {
			"type": "ranking_row",
			"rank": str(_index_of_tag(group, String(row["tag"])) + 1),
			"name": I18n.nation_name(String(row["tag"])),
			"ratio": value / peak if peak > 0.0 else 0.0,
			"value": String(row["text"]),
			"highlight": String(row["tag"]) == PLAYER_NATION,
		}
		_rows_box.add_child(UiTemplateBuilder.build_block(block))

	_set_text("ranking.title", I18n.t("tpl.quadro.ranking.title", {
		"indicator": I18n.t(String(INDICATORS[_indicator]["label"])),
	}))
	_set_text("ranking.source", I18n.t("tpl.quadro.source"))


func _refresh_position() -> void:
	var all := _ranked_rows()
	var group := _group_rows(all)
	var group_index := _index_of_tag(group, PLAYER_NATION)
	var world_index := _index_of_tag(all, PLAYER_NATION)
	if group_index < 0 or world_index < 0:
		_set_text("position.rank", "—")
		_set_text("position.note", I18n.t("tpl.quadro.position.none"))
		_set_text("position.above", "—")
		_set_text("position.below", "—")
		return

	_set_text("position.rank", str(group_index + 1))
	_set_text("position.note", I18n.t("tpl.quadro.position.note", {
		"group": UiKit.number(group.size()),
		"world": UiKit.number(world_index + 1),
		"world_total": UiKit.number(all.size()),
	}))
	_set_text("position.above", _neighbour_text(group, group_index - 1))
	_set_text("position.below", _neighbour_text(group, group_index + 1))


## Tendência de 12 meses do indicador atual, para a nação do jogador. Sem 2
## meses amostrados (ou com buraco na série), o cartão mantém o estado vazio;
## nada é interpolado.
func _refresh_trend() -> void:
	var field := String(TREND_FIELDS.get(String(INDICATORS[_indicator]["key"]),
			"population"))
	var values: PackedFloat32Array = PackedFloat32Array()
	var labels := PackedStringArray()
	if _history != null:
		values = _history.nation_monthly(PLAYER_NATION, field)
		labels = _history.month_labels()
	var valid := values.size() >= 2 and labels.size() == values.size() \
			and _all_finite(values)
	_set_hidden("trend.series", not valid)
	_set_hidden("trend.empty", valid)
	if not valid:
		return
	var chart := _slots.get("trend.chart") as UiTemplateBuilder.LineChart
	if chart != null:
		var series: Array[float] = []
		for value in values:
			series.append(value)
		chart.values.assign(series)
		# População/PIB/reservas têm escala absoluta; comida é razão.
		chart.autoscale = field != "food"
		chart.queue_redraw()
	_set_text("trend.range", "%s → %s · %s" % [
		_month_text(labels[0]), _month_text(labels[labels.size() - 1]),
		I18n.t("stats.history.months", {"n": UiKit.number(values.size())}),
	])


func _set_hidden(slot: String, hidden: bool) -> void:
	var node := _slots.get(slot) as Control
	if node != null:
		node.visible = not hidden


static func _all_finite(values: PackedFloat32Array) -> bool:
	for value in values:
		if not is_finite(value):
			return false
	return true


## "2026-03" → "março 2026" no idioma ativo (sem inventar nome se faltar a chave).
func _month_text(key: String) -> String:
	var parts := key.split("-")
	if parts.size() < 2:
		return key
	var number := int(parts[1])
	var translated := I18n.t("month.%d" % number)
	var name := "%02d" % number if translated == "month.%d" % number else translated
	return "%s %s" % [name, parts[0]]


func _neighbour_text(group: Array, index: int) -> String:
	if index < 0 or index >= group.size():
		return "—"
	var row: Dictionary = group[index]
	var mine: Dictionary = group[_index_of_tag(group, PLAYER_NATION)]
	var diff := float(mine["value"]) - float(row["value"])
	var sign_text := "+" if diff > 0.0 else "−"
	return "%s · %s%s" % [I18n.nation_name(String(row["tag"])), sign_text,
			_delta_text(absf(diff))]


# --- Dados -------------------------------------------------------------------

func _ranked_rows() -> Array:
	var rows: Array = []
	for entry in _nation_entries():
		var tag := String(entry["tag"])
		var value := _value_for(tag)
		rows.append({
			"tag": tag,
			"value": value,
			"text": _value_text(value),
			"region": String(WorldFacts.nation(tag).get("region", "")),
		})
	rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["value"]) > float(b["value"]))
	return rows


func _nation_entries() -> Array:
	var by_tag := {}
	for city in _summary.get("cities", []):
		var tag := String(city.get("nation", ""))
		if tag.is_empty():
			continue
		var entry: Dictionary = by_tag.get(tag, {"tag": tag, "population": 0.0,
				"production": 0.0, "need": 0.0})
		entry["population"] = float(entry["population"]) \
				+ float(city.get("population", 0.0))
		entry["production"] = float(entry["production"]) \
				+ float(city.get("production", 0.0))
		entry["need"] = float(entry["need"]) + float(city.get("need", 0.0))
		by_tag[tag] = entry
	if by_tag.is_empty():
		for tag in WorldFacts.nations():
			by_tag[tag] = {"tag": tag, "population": 0.0, "production": 0.0,
					"need": 0.0}
	return by_tag.values()


func _value_for(tag: String) -> float:
	var meta := WorldFacts.nation(tag)
	match String(INDICATORS[_indicator]["key"]):
		"population":
			var stats := _stats(tag)
			if float(stats["population"]) > 0.0:
				return float(stats["population"])
			return float(meta.get("population", 0.0))
		"gdp":
			return float(meta.get("gdp_ppp_bn", 0.0))
		"food":
			var stats := _stats(tag)
			var need := float(stats["need"])
			return float(stats["production"]) / need if need > 0.0 else -1.0
		"minerals":
			var deposits := WorldFacts.deposits(tag)
			var total := 0.0
			for resource in deposits:
				total += float(deposits[resource])
			return total
	return 0.0


func _stats(tag: String) -> Dictionary:
	var stats := {"population": 0.0, "production": 0.0, "need": 0.0}
	for city in _summary.get("cities", []):
		if String(city.get("nation", "")) != tag:
			continue
		for key in stats:
			stats[key] = float(stats[key]) + float(city.get(key, 0.0))
	return stats


func _group_rows(rows: Array) -> Array:
	if _group <= 0:
		return rows
	var region := String(_groups[_group]["key"])
	var out: Array = []
	for row in rows:
		if String(row.get("region", "")) == region:
			out.append(row)
	return out


func _group_counts() -> Dictionary:
	var counts := {"world": 0}
	for group in _groups:
		counts[String(group["key"])] = 0
	for entry in _nation_entries():
		var tag := String(entry["tag"])
		counts["world"] = int(counts["world"]) + 1
		var region := String(WorldFacts.nation(tag).get("region", ""))
		if counts.has(region):
			counts[region] = int(counts[region]) + 1
	return counts


func _index_of_tag(rows: Array, tag: String) -> int:
	for index in rows.size():
		if String(rows[index].get("tag", "")) == tag:
			return index
	return -1


# --- Formatação ---------------------------------------------------------------

func _value_text(value: float) -> String:
	match String(INDICATORS[_indicator]["key"]):
		"population":
			return _population_text(value)
		"gdp":
			return _money_text(value)
		"food":
			return UiKit.number(value * 100.0) + "%"
		"minerals":
			return _reserve_text(value)
	return UiKit.number(value)


func _delta_text(value: float) -> String:
	match String(INDICATORS[_indicator]["key"]):
		"population":
			return _population_text(value)
		"gdp":
			return _money_text(value)
		"food":
			return UiKit.number(value * 100.0) + "%"
		"minerals":
			return _reserve_text(value)
	return UiKit.number(value)


func _population_text(value: float) -> String:
	if value >= 1_000_000_000.0:
		return UiKit.number(value / 1_000_000_000.0, 1) + " " \
				+ I18n.t("common.billion")
	if value >= 1_000_000.0:
		return UiKit.number(value / 1_000_000.0, 1) + " " \
				+ I18n.t("common.million")
	if value >= 1_000.0:
		return UiKit.number(value / 1_000.0, 1) + " " \
				+ I18n.t("common.thousand")
	return UiKit.number(value)


func _money_text(bn: float) -> String:
	if bn >= 1000.0:
		return "$ " + UiKit.number(bn / 1000.0, 1) + " " \
				+ I18n.t("common.trillion")
	return "$ " + UiKit.number(bn, 1) + " " + I18n.t("common.billion")


## Reserva em kt → Mt/Gt (1 Gt = 1.000.000 kt).
func _reserve_text(kt: float) -> String:
	if kt >= 1_000_000.0:
		return UiKit.number(kt / 1_000_000.0, 1) + " Gt"
	if kt >= 1_000.0:
		return UiKit.number(kt / 1_000.0, 1) + " Mt"
	return UiKit.number(kt) + " kt"


# --- Utilidades ---------------------------------------------------------------

func _set_text(slot: String, text: String) -> void:
	var node := _slots.get(slot) as Label
	if node != null:
		node.text = text
