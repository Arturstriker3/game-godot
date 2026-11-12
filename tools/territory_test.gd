extends SceneTree

## Território dinâmico: valida a atribuição província->cidade, o override de
## debug e a textura de dono.
##   godot --headless --path . --script res://tools/territory_test.gd

const MAIN_SCENE := "res://game/main_game.tscn"
const OWNER_COLUMNS := 64
const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
## Nação jogável sem nenhuma província admin-1 no atlas. Hoje a lista é vazia:
## os microestados (VAT/SMR/MCO/LIE) foram fundidos nos vizinhos pela curadoria
## (tools/world_nation_curation.py, MICROSTATES) e saíram do roster, então não
## existe exceção legítima — qualquer nação jogável sem província é falha.
const NO_PROVINCE_NATIONS: Array[String] = []


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

	# (a) cada nação jogável tem províncias. A lista de exceções é vazia desde a
	# curadoria r2 (VAT anexado à ITA); a checagem vem do nations.json (177),
	# não de tags fixas.
	var empty_nations: Array[String] = []
	for tag: String in GeographyServiceScript.playable_tags():
		var atlas_tag := GeographyServiceScript.province_tag(tag)
		if _nation_province_count(tm, atlas_tag) <= 0:
			empty_nations.append(tag)
	empty_nations.sort()
	var expected_empty := NO_PROVINCE_NATIONS.duplicate()
	expected_empty.sort()
	if empty_nations != expected_empty:
		failures.append("nações sem província: %s (esperado %s)" % [
			", ".join(empty_nations), ", ".join(expected_empty)])
	else:
		print("[territory] (a) %d nações jogáveis com província; sem província: %s" % [
			GeographyServiceScript.playable_tags().size() - empty_nations.size(),
			", ".join(empty_nations)])

	var owners: Dictionary = tm.owners()

	# (b) toda província de nação jogável tem dono da MESMA nação (sem captura).
	for tag: String in GeographyServiceScript.playable_tags():
		var atlas_tag := GeographyServiceScript.province_tag(tag)
		for pid in _nation_province_ids(tm, atlas_tag):
			if String(owners.get(pid, "")) != tag:
				failures.append("província %s %d com dono %s (esperado %s)" % [
					atlas_tag, pid, owners.get(pid, ""), tag])
	print("[territory] (b) dono inicial == nação em todas as províncias jogáveis")

	# (c) override BEL-BRU -> FRA. A Bélgica tem três cidades (Bruxelas,
	# Antuérpia, Liège): só as províncias atribuídas a Bruxelas mudam de dono.
	var bel_ids := _nation_province_ids(tm, "BEL")

	# Índice do dono FRA, lido da textura (antes do override).
	var fra_idx := _owner_index_of(tm, "FRA")

	var brussels_ids: Array[int] = []
	var rest_ids: Array[int] = []
	for pid in bel_ids:
		if tm.city_for_province(pid) == "BEL-BRU":
			brussels_ids.append(pid)
		else:
			rest_ids.append(pid)
	city_world.debug_set_city_nation("BEL-BRU", "FRA")
	owners = tm.owners()
	for pid in brussels_ids:
		if String(owners.get(pid, "")) != "FRA":
			failures.append("após override, província %d (Bruxelas) deveria ser FRA (era %s)"
					% [pid, owners.get(pid, "")])
	for pid in rest_ids:
		if String(owners.get(pid, "")) != "BEL":
			failures.append("após override, província %d (Bruxelas é só BEL-BRU) deveria seguir BEL (era %s)"
					% [pid, owners.get(pid, "")])
	if tm.nation_anchor("BEL").is_empty():
		failures.append("nation_anchor(BEL) deveria existir após override (outras cidades seguem BEL)")

	if not brussels_ids.is_empty():
		var pid: int = brussels_ids[0]
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
