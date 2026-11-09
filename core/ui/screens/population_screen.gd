extends VBoxContainer

## Janela de População: totais nacionais, grupos por nação e por cidade, com a
## relação entre renda, consumo e atendimento da demanda.
##
## Nada aqui é inventado: os números vêm do resumo da simulação. O que o modelo
## ainda não cobre (desemprego, salários nominais, poupança, migração) aparece
## explicitamente como não modelado.

signal city_requested(city_id: String)

const NATION_WEIGHTS := [1.6, 1.0, 0.8, 1.2, 1.0, 1.0]
const NATION_ALIGN := [false, true, true, true, true, true]
const CITY_WEIGHTS := [1.4, 1.0, 1.0, 0.8, 0.9, 1.0, 1.0]
const CITY_ALIGN := [false, false, true, true, true, true, true]

var _summary: Dictionary = {}
var _tab := 0
var _tab_buttons: Array[Button] = []
var _body: VBoxContainer


func _ready() -> void:
	add_theme_constant_override("separation", UiKit.SPACE_MD)
	size_flags_vertical = Control.SIZE_EXPAND_FILL

	add_child(_build_tabs())
	add_child(UiKit.rule())

	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED

	var card := UiKit.card(UiKit.SPACE_MD)
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(card)

	_body = VBoxContainer.new()
	_body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_body.add_theme_constant_override("separation", UiKit.SPACE_SM)
	card.add_child(_body)

	add_child(scroll)
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


func refresh(summary: Dictionary) -> void:
	_summary = summary
	_retranslate()
	_render()


func _retranslate() -> void:
	if _tab_buttons.size() < 2:
		return
	_tab_buttons[0].text = I18n.t("econ.tab.nations")
	_tab_buttons[1].text = I18n.t("econ.tab.cities")


func _set_tab(index: int) -> void:
	_tab = index
	_render()


func _render() -> void:
	if _body == null:
		return

	for i in _tab_buttons.size():
		UiKit.set_tab_active(_tab_buttons[i], i == _tab)

	UiKit.clear(_body)
	_body.add_child(_glance())
	_body.add_child(_note())
	_body.add_child(UiKit.rule())

	if _tab == 0:
		_render_nations()
	else:
		_render_cities()


## Faixa de indicadores agregados, todos derivados do resumo da simulação.
func _glance() -> Control:
	var totals := _totals()
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", UiKit.SPACE_XL)

	row.add_child(_pair(I18n.t("pop.total"),
			UiKit.number(float(totals["population"]) / 1_000_000.0, 1) + " M"))
	row.add_child(_pair(I18n.t("pop.jobs"), UiKit.number(float(totals["jobs"]))))
	row.add_child(_pair(I18n.t("pop.income"),
			UiKit.number(float(totals["income"]) * 100.0) + "%"))
	row.add_child(_pair(I18n.t("pop.consumption"),
			UiKit.number(float(totals["consumed"])) + " t"))
	row.add_child(_pair(I18n.t("pop.attendance"),
			UiKit.number(float(totals["attendance"]) * 100.0) + "%",
			_attendance_color(float(totals["attendance"]))))
	return row


func _note() -> Control:
	var note := UiKit.paragraph(I18n.t("pop.note"), UiKit.FS_CAPTION + 1,
			UiKit.TEXT_MUTED, 780.0)
	return note


func _render_nations() -> void:
	var nations := _nation_rows()
	if nations.is_empty():
		_body.add_child(UiKit.empty_state(I18n.t("pop.empty"), I18n.t("pop.empty.body")))
		return

	_body.add_child(UiKit.table_header([
		I18n.t("econ.col.nation"),
		I18n.t("econ.col.population"),
		I18n.t("econ.col.cities"),
		I18n.t("pop.jobs"),
		I18n.t("pop.col.income"),
		I18n.t("pop.attendance"),
	], NATION_WEIGHTS, NATION_ALIGN))

	for nation in nations:
		_body.add_child(UiKit.table_row([
			I18n.nation_name(nation["tag"]),
			UiKit.number(float(nation["population"]) / 1_000_000.0, 1) + " M",
			UiKit.number(float(nation["cities"])),
			UiKit.number(float(nation["jobs"])),
			UiKit.number(float(nation["income"]) * 100.0) + "%",
			UiKit.number(float(nation["attendance"]) * 100.0) + "%",
		], NATION_WEIGHTS, [
			UiKit.nation_color(nation["tag"]), UiKit.TEXT, UiKit.TEXT_DIM,
			UiKit.TEXT_DIM, UiKit.STEEL,
			_attendance_color(float(nation["attendance"])),
		], NATION_ALIGN))


func _render_cities() -> void:
	var cities := _city_rows()
	if cities.is_empty():
		_body.add_child(UiKit.empty_state(I18n.t("pop.empty"), I18n.t("pop.empty.body")))
		return

	_body.add_child(UiKit.table_header([
		I18n.t("econ.col.city"),
		I18n.t("econ.col.nation"),
		I18n.t("econ.col.population"),
		I18n.t("pop.jobs"),
		I18n.t("pop.col.income"),
		I18n.t("pop.consumption"),
		I18n.t("pop.attendance"),
	], CITY_WEIGHTS, CITY_ALIGN))

	for city in cities:
		var row := UiKit.row_button()
		row.add_child(UiKit.table_row([
			I18n.city_name(city["id"], city["name"]),
			I18n.nation_name(city["nation"]),
			UiKit.number(float(city["population"]) / 1_000_000.0, 1) + " M",
			UiKit.number(float(city["jobs"])),
			UiKit.number(float(city["income"]) * 100.0) + "%",
			UiKit.number(float(city["consumed"])) + " t",
			UiKit.number(float(_attendance(city)) * 100.0) + "%",
		], CITY_WEIGHTS, [
			UiKit.TEXT, UiKit.nation_color(city["nation"]), UiKit.TEXT,
			UiKit.TEXT_DIM, UiKit.STEEL, UiKit.TEXT_DIM,
			_attendance_color(_attendance(city)),
		], CITY_ALIGN))

		var city_id := String(city["id"])
		row.pressed.connect(func() -> void: city_requested.emit(city_id))
		_body.add_child(row)


## Proporção da demanda de trigo efetivamente consumida.
func _attendance(city: Dictionary) -> float:
	var need := float(city.get("need", 0.0))
	if need <= 0.0:
		return 1.0
	return clampf(float(city.get("consumed", 0.0)) / need, 0.0, 1.0)


func _attendance_color(ratio: float) -> Color:
	if ratio >= 0.95:
		return UiKit.POSITIVE
	if ratio >= 0.80:
		return UiKit.WARNING
	return UiKit.NEGATIVE


func _totals() -> Dictionary:
	var population := 0.0
	var jobs := 0.0
	var consumed := 0.0
	var need := 0.0
	var weighted_income := 0.0

	for city in _summary.get("cities", []):
		var city_population := float(city.get("population", 0.0))
		population += city_population
		jobs += float(city.get("jobs", 0.0))
		consumed += float(city.get("consumed", 0.0))
		need += float(city.get("need", 0.0))
		weighted_income += city_population * float(city.get("income", 0.0))

	return {
		"population": population,
		"jobs": jobs,
		"consumed": consumed,
		"need": need,
		"income": weighted_income / population if population > 0.0 else 0.0,
		"attendance": consumed / need if need > 0.0 else 1.0,
	}


## Agrega as cidades por nação: população, empregos, renda e atendimento.
func _nation_rows() -> Array:
	var buckets := {}

	for city in _summary.get("cities", []):
		var tag := String(city["nation"])
		if not buckets.has(tag):
			buckets[tag] = {
				"tag": tag, "population": 0.0, "cities": 0.0, "jobs": 0.0,
				"consumed": 0.0, "need": 0.0, "weighted_income": 0.0,
			}
		var bucket: Dictionary = buckets[tag]
		var city_population := float(city.get("population", 0.0))
		bucket["population"] = float(bucket["population"]) + city_population
		bucket["cities"] = float(bucket["cities"]) + 1.0
		bucket["jobs"] = float(bucket["jobs"]) + float(city.get("jobs", 0.0))
		bucket["consumed"] = float(bucket["consumed"]) + float(city.get("consumed", 0.0))
		bucket["need"] = float(bucket["need"]) + float(city.get("need", 0.0))
		bucket["weighted_income"] = float(bucket["weighted_income"]) \
				+ city_population * float(city.get("income", 0.0))

	var rows: Array = []
	for tag in buckets:
		var bucket: Dictionary = buckets[tag]
		var population := float(bucket["population"])
		var need := float(bucket["need"])
		rows.append({
			"tag": tag,
			"population": population,
			"cities": bucket["cities"],
			"jobs": bucket["jobs"],
			"income": float(bucket["weighted_income"]) / population \
					if population > 0.0 else 0.0,
			"attendance": float(bucket["consumed"]) / need if need > 0.0 else 1.0,
		})

	rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["population"]) > float(b["population"]))
	return rows


func _city_rows() -> Array:
	var cities: Array = _summary.get("cities", []).duplicate()
	cities.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("population", 0.0)) > float(b.get("population", 0.0)))
	return cities


func _pair(caption_text: String, value_text: String,
		color: Color = UiKit.TEXT) -> VBoxContainer:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 0)
	box.add_child(UiKit.caption(caption_text))
	box.add_child(UiKit.label(value_text, UiKit.FS_SUBTITLE, color))
	return box
