extends SceneTree

## Limite elÃ¡stico da cÃ¢mera (game/main_game.gd): o centro nunca passa da folga
## alÃ©m da folha e a mola o devolve para dentro; destinos de move_to jÃ¡ saem
## presos Ã  folha.
##   godot --headless --path . --script res://tools/camera_limit_test.gd

const MAIN_SCENE := "res://game/main_game.tscn"


func _init() -> void:
	_run.call_deferred()


## Espera tempo real (a mola Ã© por tempo, e em headless os quadros sÃ£o curtos).
func _wait_seconds(seconds: float) -> void:
	var end := Time.get_ticks_msec() + int(seconds * 1000.0)
	while Time.get_ticks_msec() < end:
		await process_frame


func _run() -> void:
	var game: Node3D = (load(MAIN_SCENE) as PackedScene).instantiate()
	root.add_child(game)
	for _i in 3:
		await process_frame

	var player := game.get_node("Player") as Node3D
	# Em headless o mouse fica em (0,0), no canto: a rolagem pela borda empurraria
	# a cÃ¢mera o tempo todo e mascararia a mola.
	player.camera_can_automatic_pan = false
	var bounds: Vector2 = game._camera_z_bounds()
	var overshoot: float = game.CAMERA_OVERSHOOT
	var failures: Array[String] = []

	# 1) Teleporte muito ao norte: preso na folga e depois devolvido pela mola.
	player.global_position.z = bounds.x - 400.0
	await process_frame
	if player.global_position.z < bounds.x - overshoot - 0.01:
		failures.append("passou da folga norte: z=%.2f limite=%.2f"
				% [player.global_position.z, bounds.x - overshoot])
	await _wait_seconds(1.5)
	if absf(player.global_position.z - bounds.x) > 0.5:
		failures.append("mola nÃ£o devolveu ao norte: z=%.2f alvo=%.2f"
				% [player.global_position.z, bounds.x])

	# 2) Mesmo teste ao sul.
	player.global_position.z = bounds.y + 400.0
	await process_frame
	if player.global_position.z > bounds.y + overshoot + 0.01:
		failures.append("passou da folga sul: z=%.2f" % player.global_position.z)
	await _wait_seconds(1.5)
	if absf(player.global_position.z - bounds.y) > 0.5:
		failures.append("mola nÃ£o devolveu ao sul: z=%.2f alvo=%.2f"
				% [player.global_position.z, bounds.y])

	# 3) Destino de move_to fora da folha sai preso dentro dela.
	var target: Vector3 = game._clamp_camera_target(Vector3(0.0, 0.0, bounds.x - 300.0))
	if not is_equal_approx(target.z, bounds.x):
		failures.append("move_to nÃ£o foi preso: z=%.2f" % target.z)

	# 4) Dentro da folha nada muda.
	var inside := (bounds.x + bounds.y) * 0.5
	player.global_position.z = inside
	for _i in 10:
		await process_frame
	if absf(player.global_position.z - inside) > 0.01:
		failures.append("mexeu dentro da folha: z=%.2f" % player.global_position.z)

	game.queue_free()
	await process_frame
	if failures.is_empty():
		print("[camera_limit] OK: folga, mola, move_to e interior validados.")
		quit(0)
	else:
		for failure in failures:
			push_error("[camera_limit] " + failure)
		quit(1)
