extends SceneTree

## Território dinâmico: valida a atribuição província->cidade, o override de
## debug e a textura de dono.
##   godot --headless --path . --script res://tools/territory_test.gd

const MAIN_SCENE := "res://game/main_game.tscn"
const OWNER_COLUMNS := 64


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var game = (load(MAIN_SCENE) as PackedScene).instantiate()
	root.add_child(game)
	for _i in 3:
		await process_frame

	var city_world = game.get("city_world")
	var failures: Array[String] = []
	var tm = city_world.territory()
	if tm == null:
		push_error("[territory] TerritoryMap nulo após build")
		game.queue_free()
		await process_frame
		quit(1)
		return

	# (a) cada nação jogável possui > 0 províncias.
	for tag in ["GBR", "FRA", "DEU", "NED", "BEL"]:
		if _nation_province_count(tm, tag) <= 0:
			failures.append("nação %s sem províncias" % tag)

	var owners: Dictionary = tm.owners()

	# (b) toda província de nation BEL tem dono BEL.
	var bel_ids := _nation_province_ids(tm, "BEL")
	for pid in bel_ids:
		if String(owners.get(pid, "")) != "BEL":
			failures.append("província BEL %d com dono %s" % [pid, owners.get(pid, "")])

	# Índice do dono FRA, lido da textura (antes do override).
	var fra_idx := _owner_index_of(tm, "FRA")

	# (c) override BEL-BRU -> FRA.
	city_world.debug_set_city_nation("BEL-BRU", "FRA")
	owners = tm.owners()
	for pid in bel_ids:
		if String(owners.get(pid, "")) != "FRA":
			failures.append("após override, província %d deveria ser FRA (era %s)"
					% [pid, owners.get(pid, "")])
	if not tm.nation_anchor("BEL").is_empty():
		failures.append("nation_anchor(BEL) deveria ser vazio após override")

	if not bel_ids.is_empty():
		var pid: int = bel_ids[0]
		var pixel: Color = tm.owner_image().get_pixel(
				pid % OWNER_COLUMNS, int(pid / OWNER_COLUMNS))
		var alpha_idx := int(round(pixel.a * 255.0))
		if alpha_idx != fra_idx:
			failures.append("pixel de owner a=%.3f (índice %d), esperado %d (FRA)"
					% [pixel.a, alpha_idx, fra_idx])

	# (d) clear volta tudo.
	city_world.debug_clear_territory_overrides()
	owners = tm.owners()
	for pid in bel_ids:
		if String(owners.get(pid, "")) != "BEL":
			failures.append("após clear, província %d deveria ser BEL (era %s)"
					% [pid, owners.get(pid, "")])

	game.queue_free()
	await process_frame
	if failures.is_empty():
		print("[territory] OK: atribuição, override e textura de dono validados.")
		quit(0)
	else:
		for failure in failures:
			push_error("[territory] " + failure)
		quit(1)


func _nation_province_ids(tm, tag: String) -> Array[int]:
	var ids: Array[int] = []
	for p in tm.provinces():
		if String(p["nation"]) == tag:
			ids.append(int(p["id"]))
	return ids


func _nation_province_count(tm, tag: String) -> int:
	return _nation_province_ids(tm, tag).size()


func _owner_index_of(tm, tag: String) -> int:
	var owners: Dictionary = tm.owners()
	for p in tm.provinces():
		if String(owners.get(int(p["id"]), "")) == tag:
			var pixel: Color = tm.owner_image().get_pixel(
					int(p["id"]) % OWNER_COLUMNS, int(int(p["id"]) / OWNER_COLUMNS))
			return int(round(pixel.a * 255.0))
	return -1
