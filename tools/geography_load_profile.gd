extends SceneTree

## Perfil do custo de CARGA da geografia (fora do tick): carregar os JSON,
## particionar as províncias entre as cidades (assign_provinces) e atribuir as
## jazidas (assign_deposits). Serve para medir o efeito da densidade de
## províncias (Fase B) no boot do jogo.
##
##   godot --headless --path . --script res://tools/geography_load_profile.gd

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"


func _init() -> void:
	var cities: Array = []
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(CITIES_PATH))
	if typeof(parsed) == TYPE_ARRAY:
		for city in parsed:
			cities.append({
				"id": String(city.get("id", "")),
				"nation": String(city.get("nation", "")),
				"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
				"landmass": String(city.get("landmass", "")),
			})
	var geo = GeographyServiceScript.new()
	var t0 := Time.get_ticks_usec()
	geo.load_data()
	var t1 := Time.get_ticks_usec()
	var catchment: Dictionary = geo.assign_provinces(cities)
	var t2 := Time.get_ticks_usec()
	geo.assign_deposits(cities)
	var t3 := Time.get_ticks_usec()
	var provinces := 0
	for city_id in catchment:
		provinces += (catchment[city_id] as Array).size()
	print("[geo_profile] load_data=%.1f ms  assign_provinces=%.1f ms  assign_deposits=%.1f ms  (%d cidades, %d províncias na bacia)" % [
		float(t1 - t0) / 1000.0, float(t2 - t1) / 1000.0, float(t3 - t2) / 1000.0,
		cities.size(), provinces])
	quit(0)
