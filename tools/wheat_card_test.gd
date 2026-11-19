extends SceneTree

## Verifica em runtime que a ficha agrícola monta a linha "Clima · Solo · Relevo"
## (composta de i18n amigável) sem erro, para um índice de trigo real.
##
## A ficha é conferida em pt-BR: a preferência salva em `user://settings.cfg`
## pode estar em en/es de uma sessão anterior (era a causa da falha do M2, com a
## linha saindo em inglês). Os rótulos esperados vêm do próprio I18n, então o
## teste não depende de texto colado no arquivo.
##
##   godot --headless --path . --script res://tools/wheat_card_test.gd

const MAIN_SCENE := "res://game/main_game.tscn"
const LOCALE := "pt_BR"


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var i18n: Node = root.get_node_or_null("I18n")
	if i18n == null:
		print("FAIL: autoload I18n ausente")
		quit(1)
		return
	i18n.call("set_locale", LOCALE)

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
		if text.contains(String(i18n.call("t", "map.legend.climate"))):
			terrain = text
			break
	print("linha de terreno:", terrain)
	var upper := terrain.to_upper()
	var ok := upper.contains(String(i18n.call("t", "map.legend.climate")).to_upper()) \
			and upper.contains(String(i18n.call("t", "map.legend.soil")).to_upper()) \
			and upper.contains(String(i18n.call("t", "map.legend.relief")).to_upper())
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
