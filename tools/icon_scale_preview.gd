extends Control

## Conferência de NITIDEZ dos ícones ilustrados, com os componentes reais do
## jogo (`ProductIcons.seal` e `ProductIcons.place`) em 16, 24 e 32 px.
##
## Uso (janela, não headless):
##   godot --path . res://tools/icon_scale_preview.tscn -- \
##         --out=res://docs/ui/icons_illustrated/icon_scale_16_24_32.png
##
## Não altera jogo nem dados: lê `goods.json` só para a lista de ids e salva um
## PNG de evidência. O `seal` de 16 px é o mesmo das tabelas; o de 32 px mostra
## a variante de 128 px do PNG.
##
## Layout: três faixas com os 113 bens (16/24/32 px), uma faixa com as
## construções em 32 px e uma tira sobre carvão para o caso de fundo escuro.

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const DEFAULT_OUT := "res://docs/ui/icons_illustrated/icon_scale_16_24_32.png"
const PAPER := Color("#efe6cc")
const INK := Color("#2b2418")
const DARK := Color("#1b1a17")
const COLUMNS := 24

var _out := DEFAULT_OUT


func _ready() -> void:
	_parse_args()
	get_window().size = Vector2i(1600, 1000)
	_build()
	for _frame in 4:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var path := ProjectSettings.globalize_path(_out)
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var image := get_viewport().get_texture().get_image()
	var error := image.save_png(path)
	print("[icon_scale_preview] ", path, " erro=", error)
	get_tree().quit(0 if error == OK else 1)


func _parse_args() -> void:
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with("--out="):
			_out = argument.trim_prefix("--out=")


func _build() -> void:
	var background := ColorRect.new()
	background.color = PAPER
	background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	background.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(background)

	var margin := MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	for side in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 24)
	add_child(margin)

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 10)
	margin.add_child(column)

	var header := Label.new()
	header.text = "Ícones ilustrados — nitidez em 16, 24 e 32 px (componente real)"
	header.add_theme_color_override("font_color", INK)
	header.add_theme_font_size_override("font_size", 22)
	column.add_child(header)

	var goods := _goods_ids()
	for size in [16, 24, 32]:
		column.add_child(_caption("Bens a %d px — %d ids" % [size, goods.size()]))
		column.add_child(_grid(goods, size, false))

	var places := _place_ids()
	if not places.is_empty():
		column.add_child(_caption("Construções, infraestrutura e finanças a 32 px — %d ids"
				% places.size()))
		column.add_child(_grid(places, 32, true))

	# Fundo escuro: o PNG colorido sobre carvão (com e sem a placa de papel).
	var dark := ColorRect.new()
	dark.color = DARK
	dark.custom_minimum_size = Vector2(0, 96)
	column.add_child(dark)
	var dark_row := HBoxContainer.new()
	dark_row.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dark_row.add_theme_constant_override("separation", 10)
	dark.add_child(dark_row)
	var inner := MarginContainer.new()
	for side in ["left", "right", "top", "bottom"]:
		inner.add_theme_constant_override("margin_" + side, 12)
	dark_row.add_child(inner)
	var strip := HBoxContainer.new()
	strip.add_theme_constant_override("separation", 8)
	inner.add_child(strip)
	for id in goods.slice(0, 14):
		strip.add_child(ProductIcons.seal(id, "", 24.0))


func _caption(text: String) -> Label:
	var label := Label.new()
	label.text = text
	label.add_theme_color_override("font_color", INK)
	label.add_theme_font_size_override("font_size", 14)
	return label


func _grid(ids: Array, size: int, places: bool) -> GridContainer:
	var grid := GridContainer.new()
	grid.columns = COLUMNS
	grid.add_theme_constant_override("h_separation", 6)
	grid.add_theme_constant_override("v_separation", 6)
	for id in ids:
		var node: Control = ProductIcons.place(String(id), float(size)) if places \
				else ProductIcons.seal(String(id), "", float(size))
		grid.add_child(node)
	return grid


func _goods_ids() -> Array:
	var file := FileAccess.open(GOODS_PATH, FileAccess.READ)
	if file == null:
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	if not (data is Array):
		return []
	var ids: Array = []
	for good in (data as Array):
		ids.append(String((good as Dictionary).get("id", "")))
	ids.sort()
	return ids


func _place_ids() -> Array:
	var ids: Array = []
	var directory := DirAccess.open(ProductIcons.PLACES_DIR)
	if directory == null:
		return ids
	for file in directory.get_files():
		if file.ends_with(".png"):
			ids.append(file.get_basename())
	ids.sort()
	return ids
