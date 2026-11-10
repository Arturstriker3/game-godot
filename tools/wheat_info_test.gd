extends SceneTree

## Verifica a ficha do recurso agrícola: para um índice de trigo, deposit_info
## deve retornar kind "wheat" + aptidão/fertilidade/cultivável do território, e
## a seleção/hover/pick deve funcionar para trigo como para mineral.
##
##   godot --headless --path . --script res://tools/wheat_info_test.gd

const MAIN_SCENE := "res://game/main_game.tscn"

var _failures: Array[String] = []


func _init() -> void:
	_run.call_deferred()


func _check(ok: bool, message: String) -> void:
	if ok:
		print("PASS: " + message)
	else:
		_failures.append(message)
		print("FAIL: " + message)


func _run() -> void:
	var game: Node3D = (load(MAIN_SCENE) as PackedScene).instantiate()
	root.add_child(game)
	for _i in 8:
		await process_frame

	var layer = game.city_world._mode_layer
	for _i in 2:
		await process_frame

	var wheat_count: int = (layer._crop_points["wheat"] as Array).size()
	var rice_count: int = (layer._crop_points["rice"] as Array).size()
	var maize_count: int = (layer._crop_points["maize"] as Array).size()
	var mineral_count: int = layer._mineral_points.size()
	_check(wheat_count > 0, "há pontos de trigo (%d)" % wheat_count)
	_check(layer._total_pickable() == mineral_count + wheat_count + rice_count + maize_count,
			"total pickable = minerais + culturas (%d)" % layer._total_pickable())

	if wheat_count > 0:
		var wheat_index: int = mineral_count
		var info: Dictionary = layer.deposit_info(wheat_index)
		_check(String(info.get("kind", "")) == "wheat", "kind = wheat")
		_check(String(info.get("resource", "")) == "wheat", "resource = wheat")
		var suitability := float(info.get("suitability", -1.0))
		var fertility := float(info.get("fertility", -1.0))
		var cultivable := float(info.get("cultivable", -1.0))
		_check(suitability >= 0.0 and suitability <= 1.0,
				"aptidão em 0..1 (%.3f)" % suitability)
		_check(fertility >= 0.0 and fertility <= 1.0,
				"fertilidade em 0..1 (%.3f)" % fertility)
		_check(cultivable >= 0.0 and cultivable <= 1.0,
				"cultivável em 0..1 (%.3f)" % cultivable)
		_check(not String(info.get("territory", "")).is_empty(),
				"território identificado (%s)" % String(info.get("territory", "")))

		# O pick deve resolver o centro projetado de um ponto de trigo para o
		# índice codificado (minerais 0..M-1, trigo M..M+W-1).
		var camera: Camera3D = game.get_node("Player").camera
		var point: Array = layer._crop_points["wheat"][0]
		var world: Vector3 = layer.to_global(layer._marker_world(point))
		if not camera.is_position_behind(world):
			var center: Vector2 = camera.unproject_position(world)
			_check(layer.pick_deposit(center, camera) == mineral_count,
					"pick no centro do trigo devolve o índice codificado")

	# Um mineral continua sendo mineral (não regrediu).
	if mineral_count > 0:
		var mineral_info: Dictionary = layer.deposit_info(0)
		_check(String(mineral_info.get("kind", "")) == "mineral", "kind do índice 0 = mineral")

	game.queue_free()
	await process_frame
	if _failures.is_empty():
		print("OK: ficha agrícola e pick de trigo consistentes")
	else:
		print("FAILURES: %d" % _failures.size())
	quit(0 if _failures.is_empty() else 1)
