extends VBoxContainer
class_name CultureMixView

## Lista de composição cultural em papel: nome, barra fina na cor da cultura e
## participação. Marcador opcional à esquerda (★ principal, ✓ aceita) para a
## composição nacional; a mistura de cidade não tem papéis e não desenha a
## coluna do marcador.
##
## O componente só desenha o que `CultureDb` devolve — nada é somado,
## reescalado ou inventado. Sem dado, mostra o estado vazio honesto.

const MARKER_WIDTH := 14.0
const PERCENT_WIDTH := 44.0
const NAME_SIZE := 15
const PERCENT_SIZE := 14
const BAR_HEIGHT := 6.0
const ROW_GAP := 2


func _ready() -> void:
	add_theme_constant_override("separation", ROW_GAP)


## `mix` no formato de `CultureDb.nation_mix`/`city_mix`.
func show_mix(mix: Array, empty_text: String, show_marks: bool = true) -> void:
	_clear()
	if mix.is_empty():
		add_child(_empty(empty_text))
		return
	for item: Variant in mix:
		if typeof(item) != TYPE_DICTIONARY:
			continue
		add_child(_row(item as Dictionary, show_marks))


func _clear() -> void:
	for child in get_children():
		remove_child(child)
		child.queue_free()


## Linha de fonte da composição. Fonte-marcador (`national_fallback`,
## `estimate_post2022`) nunca aparece crua: vira a palavra "estimativa".
static func source_line(source: Dictionary) -> String:
	if bool(source.get("estimate", false)):
		return I18n.t("pop.culture.estimate")
	var text := String(source.get("text", "")).strip_edges()
	if text.is_empty():
		return ""
	return I18n.t("pop.culture.source", {"source": text})


func _row(item: Dictionary, show_marks: bool) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	row.mouse_filter = Control.MOUSE_FILTER_IGNORE

	var role := String(item.get("role", ""))
	if show_marks:
		var mark := _label(_mark(role), 12, UiKit.TINTA)
		mark.custom_minimum_size.x = MARKER_WIDTH
		mark.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		row.add_child(mark)

	var culture_name := String(item.get("name", ""))
	var name_label := _label(culture_name, NAME_SIZE, UiKit.TINTA)
	name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	name_label.size_flags_stretch_ratio = 1.15
	name_label.tooltip_text = culture_name
	row.add_child(name_label)

	var share := clampf(float(item.get("share", 0.0)), 0.0, 1.0)
	var color: Variant = item.get("color_value", UiKit.TINTA)
	var bar := UiTemplateBuilder.MeterBar.new(share,
			color if color is Color else UiKit.TINTA, BAR_HEIGHT)
	bar.size_flags_stretch_ratio = 1.0
	row.add_child(bar)

	var percent := _label(_percent_text(float(item.get("percent", 0.0))),
			PERCENT_SIZE, UiKit.TINTA_SUAVE)
	percent.custom_minimum_size.x = PERCENT_WIDTH
	percent.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	percent.tooltip_text = percent.text
	row.add_child(percent)
	return row


func _empty(text: String) -> Control:
	var label := _label(text, 13, UiKit.TINTA_SUAVE)
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.custom_minimum_size.x = 220.0
	return label


## "★" principal, "✓" aceita; o dado sem papel (ou "outras") fica sem marca.
static func _mark(role: String) -> String:
	if role == "primary":
		return "★"
	if role == "accepted":
		return "✓"
	return " "


static func _percent_text(percent: float) -> String:
	return "%s%%" % UiKit.number(percent, 0)


## Barlow Condensed é a fonte de dado da Papelaria; o texto é dado, não ornamento.
static func _label(text: String, size: int, color: Color) -> Label:
	var label := UiKit.label(text, size, color)
	label.add_theme_font_override("font", UiKit.BODY_FONT)
	label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	label.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return label
