extends SceneTree

## Verifica em runtime que a ficha agrícola monta a linha "Clima · Solo · Relevo"
## (composta de i18n amigável) sem erro, para um índice de trigo real.
##
##   godot --headless --path . --script res://tools/wheat_card_test.gd

const MAIN_SCENE := "res://game/main_game.tscn"


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var game: Node3D = (load(MAIN_SCENE) as PackedScene).instantiate()
	root.add_child(game)
	for _i in 8:
		await process_frame

	var layer = game.city_world._mode_layer
	var hud = game.hud
	hud.set_map_mode("resources")
	for _i in 2:
		await process_frame

	var wheat_index: int = layer._mineral_points.size()
	var info: Dictionary = layer.deposit_info(wheat_index)
	print("info:", info.get("territory"), "climate=", info.get("climate"),
			"soil=", info.get("soil_type"), "relief=", info.get("relief"))

	hud.show_deposit_info(info)
	await process_frame

	var labels: PackedStringArray = []
	for node in _find_labels(hud):
		labels.append(String(node.text))
	var terrain := ""
	for text in labels:
		if text.contains("CLIMA") or text.contains("Clima"):
			terrain = text
			break
	print("linha de terreno:", terrain)
	var upper := terrain.to_upper()
	var ok := upper.contains("CLIMA") and upper.contains("SOLO") and upper.contains("RELEVO")
	if not ok:
		print("FAIL: linha de terreno ausente ou incompleta")
	print("labels:", labels)

	game.queue_free()
	await process_frame
	quit(0 if ok else 1)


func _find_labels(node: Node) -> Array:
	var out: Array = []
	for child in node.get_children():
		if child is Label:
			out.append(child)
		out.append_array(_find_labels(child))
	return out
