extends SceneTree

## E6 — mapa de POTENCIAL renovável (solar / eólica / hídrica).
##
## O mapa não é o parque existente: cada marcador é uma REGIÃO (cidade) com
## potencial para instalar, na mesma infraestrutura do mapa de recursos.
## Verifica, sem câmera e sem autoloads:
##   1. marcadores = cidades com dado por fonte (solar, eólica e hídrica);
##   2. limiares p25/p50/p75 declarados conferem com a distribuição dos dados;
##   3. classes corretas em 3 casos conhecidos (Saara solar, Mar do Norte/
##      Patagônia eólico, Brasil/Noruega hídrico);
##   4. filtro por fonte esconde só a fonte desligada;
##   5. ficha traz valor, classe, construído do parque e disponível (hídrico);
##   6. zoom afastado agrupa por nação×fonte no melhor sítio, sem perder classe;
##   7. recursos minerais continuam funcionando (regressão do modo vizinho);
##   8. camada opcional de calor só desenha com toggle ligado.
##
##   godot --headless --path . --script res://tools/e6_energy_map_test.gd

const ModeLayerScript: Script = preload("res://map/map_mode_layer.gd")
const EnergyGridLayerScript: Script = preload("res://map/energy_grid_layer.gd")
const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const RENEWABLE_PATH := "res://data/definitions/energy/renewable_potential.json"
const HYDRO_PATH := "res://data/definitions/energy/hydro_potential.json"
const PLANTS_PATH := "res://data/definitions/energy/power_plants_2026_scaled.json"
const SOURCES := ["solar", "wind", "hydro"]
const QUANTILES := [0.25, 0.5, 0.75]

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
	var cities: Array = JSON.parse_string(
			FileAccess.get_file_as_string(CITIES_PATH))
	var renewable: Dictionary = JSON.parse_string(
			FileAccess.get_file_as_string(RENEWABLE_PATH))
	var hydro: Dictionary = JSON.parse_string(
			FileAccess.get_file_as_string(HYDRO_PATH))
	_check(cities.size() > 0, "cenário com cidades (%d)" % cities.size())
	_check(typeof(renewable) == TYPE_DICTIONARY, "renewable_potential carregado")
	_check(typeof(hydro) == TYPE_DICTIONARY, "hydro_potential carregado")

	var expected := _expected_values(renewable, hydro)
	for source in SOURCES:
		_check((expected[source] as Dictionary).size() > 0,
				"%s: %d cidades com dado" % [source,
						(expected[source] as Dictionary).size()])

	var started := Time.get_ticks_msec()
	var layer: Node3D = ModeLayerScript.new()
	root.add_child(layer)
	layer.setup(cities)
	layer.set_mode("energy")
	layer.set_energy_zoom_band(2)
	var build_ms := Time.get_ticks_msec() - started
	print("  build do mapa de energia: %d ms" % build_ms)

	# 1. Marcadores = cidades com dado (>0) por fonte.
	var counts: Dictionary = layer.energy_marker_counts()
	var total_expected := 0
	for source in SOURCES:
		var want: int = (expected[source] as Dictionary).size()
		total_expected += want
		_check(int(counts.get(source, 0)) == want,
				"zoom perto: %s com %d marcadores (esperado %d)"
				% [source, int(counts.get(source, 0)), want])
	_check(layer.energy_marker_count() == total_expected,
			"total de marcadores = cidades com dado (%d)" % total_expected)
	_check(layer.energy_marker_count() < cities.size() * SOURCES.size(),
			"hídrico sem marcador para cidade sem potencial (vazio honesto)")

	# 2. Limiares declarados = quartis da distribuição dos próprios dados.
	var thresholds: Dictionary = layer.energy_thresholds()
	for source in SOURCES:
		var computed := _quantiles(expected[source])
		var declared: Array = thresholds.get(source, [])
		var ok := declared.size() == computed.size()
		if ok:
			for index in computed.size():
				if absf(float(declared[index]) - float(computed[index])) > 0.15:
					ok = false
		_check(ok, "limiares %s = p25/p50/p75 dos dados (%s)" % [source,
				str(computed)])

	# 3. Casos conhecidos. Saara solar, Mar do Norte/Patagônia eólico,
	#    Brasil/Noruega hídrico.
	var cases := [
		{"source": "solar", "city": "DZA-TAM", "class": "excellent"},
		{"source": "wind", "city": "GBR-NCL", "class": "excellent"},
		{"source": "wind", "city": "ARG-RIO", "class": "excellent"},
		{"source": "hydro", "city": "NOR-OSL", "class": "excellent"},
		{"source": "hydro", "city": "BRA-BELE", "class": "excellent"},
	]
	for case: Dictionary in cases:
		var info := _marker_info(layer, String(case["source"]),
				String(case["city"]))
		_check(not info.is_empty(), "marcador %s|%s existe"
				% [case["source"], case["city"]])
		if info.is_empty():
			continue
		_check(String(info.get("class", "")) == String(case["class"]),
				"%s|%s = %s (classe %s)"
				% [case["source"], case["city"], info.get("class"),
						case["class"]])
		_check(String(info.get("unit", "")) != "",
				"%s|%s traz unidade declarada (%s)"
				% [case["source"], case["city"], info.get("unit")])

	# 4. Filtro por fonte.
	var solar_count := int(counts.get("solar", 0))
	layer.set_energy_filter({"solar": false})
	var filtered: Dictionary = layer.energy_marker_counts()
	_check(not filtered.has("solar"), "filtro solar esconde a fonte")
	_check(int(filtered.get("wind", 0)) == int(counts.get("wind", 0))
			and int(filtered.get("hydro", 0)) == int(counts.get("hydro", 0)),
			"filtro solar mantém eólica e hídrica")
	_check(layer.energy_marker_count() == total_expected - solar_count,
			"filtro solar: %d marcadores (esperado %d)"
			% [layer.energy_marker_count(), total_expected - solar_count])
	layer.set_energy_filter({})
	_check(layer.energy_marker_count() == total_expected,
			"filtro vazio restaura todas as fontes")

	# 5. Construído vs disponível no hídrico.
	var oslo := _marker_info(layer, "hydro", "NOR-OSL")
	var hydro_rows: Dictionary = (hydro as Dictionary).get("by_city", {})
	var oslo_potential := _json_number(
			(hydro_rows.get("NOR-OSL", {}) as Dictionary).get("potential_mw"))
	var built_expected := _built_mw("hydro", "NOR-OSL")
	_check(absf(float(oslo.get("built_mw", -1.0)) - built_expected) <= 0.5,
			"NOR-OSL construído = %.1f MW do parque atual" % built_expected)
	_check(absf(float(oslo.get("potential_mw", -1.0)) - oslo_potential) <= 0.5,
			"NOR-OSL potencial técnico = %.1f MW" % oslo_potential)
	_check(absf(float(oslo.get("available_mw", -1.0))
			- maxf(oslo_potential - built_expected, 0.0)) <= 0.5,
			"NOR-OSL disponível = potencial - construído")
	var sahara := _marker_info(layer, "solar", "DZA-TAM")
	_check(float(sahara.get("available_mw", 0.0)) < 0.0,
			"solar não inventa teto de MW (disponível = recurso)")

	# 6. Zoom afastado: nação×fonte, no melhor sítio da fonte.
	layer.set_energy_zoom_band(0)
	var far_count: int = layer.energy_marker_count()
	_check(far_count < total_expected,
			"zoom afastado agrupa por nação×fonte (%d marcadores)" % far_count)
	var nor_index: int = layer.energy_marker_index("hydro|NOR")
	_check(nor_index >= 0, "agregado hydro|NOR existe")
	if nor_index >= 0:
		var nor: Dictionary = layer.energy_marker_info(nor_index)
		_check(bool(nor.get("aggregate", false)), "hydro|NOR é agregado")
		_check(String(nor.get("best_city_id", "")) == "NOR-OSL",
				"hydro|NOR aponta o melhor sítio (NOR-OSL)")
		_check(String(nor.get("class", "")) == "excellent",
				"hydro|NOR mantém a classe do melhor sítio")
		_check(int(nor.get("count", 0)) > 1,
				"hydro|NOR soma %d cidades com dado" % int(nor.get("count", 0)))
	layer.set_energy_zoom_band(2)

	# 7. Regressão: o mapa de recursos continua o mesmo sistema.
	layer.set_mode("resources")
	_check(layer.mineral_points().size() > 0,
			"mapa de recursos: %d jazidas" % layer.mineral_points().size())
	var deposit: Dictionary = layer.deposit_info(0)
	_check(String(deposit.get("kind", "")) == "mineral",
			"mapa de recursos: ficha do depósito intacta")
	layer.set_mode("energy")
	layer.set_energy_zoom_band(2)

	# 8. Camada opcional de calor/redes.
	var grid: Node3D = EnergyGridLayerScript.new()
	root.add_child(grid)
	grid.setup(cities)
	var ids := _two_nations(cities)
	var status := {
		"cities": {
			String(ids["city_a"]): {"has_data": true, "connection": "connected"},
			String(ids["city_b"]): {"has_data": true, "connection": "isolated"},
		},
		"interconnects": [{
			"a": String(ids["nation_a"]), "b": String(ids["nation_b"]),
			"capacity_mw": 1200.0, "flow_mwh": 600.0, "contract_flow_mwh": 600.0,
		}],
	}
	grid.refresh(status)
	grid.set_active(true)
	grid.set_overlays({"grid": true})
	var dot_mesh: MultiMeshInstance3D = grid.get("_dot_mesh")
	var line_mesh: MultiMeshInstance3D = grid.get("_line_mesh")
	_check(dot_mesh != null and dot_mesh.multimesh.visible_instance_count == 2,
			"redes: 2 cidades com estado conectada/isolada")
	_check(line_mesh != null and line_mesh.multimesh.visible_instance_count == 1,
			"redes: 1 interconexão E5 desenhada")
	grid.set_overlays({"grid": false, "solar": true, "wind": true, "hydro": true})
	_check(not dot_mesh.visible and not line_mesh.visible,
			"redes somem com o toggle desligado")
	var potential: Dictionary = grid.get("_potential_meshes")
	_check(potential.size() == 3, "calor: 3 camadas (solar/eólica/hídrica)")
	var visible := 0
	for kind: Variant in potential:
		if (potential[kind] as MeshInstance3D).visible:
			visible += 1
	_check(visible == 3, "calor visível nos toggles ligados")

	# 9. Legibilidade: cor por classe, piso de px e calor automático de fonte
	#    única (revisão do chefe das capturas e6_capture_*).
	var colors := {}
	for class_key in ["excellent", "good", "medium", "weak"]:
		var info := _class_visual(layer, class_key)
		_check(not info.is_empty(), "classe %s tem marcador para medir" % class_key)
		if info.is_empty():
			continue
		var color: Color = info.get("color", Color.WHITE)
		_check(not colors.has(color), "cor da classe %s é distinta" % class_key)
		colors[color] = class_key
		_check(float(info.get("min_px", 0.0)) >= 22.0,
				"classe %s: piso >= 22 px (%.0f)"
				% [class_key, float(info.get("min_px", 0.0))])
	var excellent := _class_visual(layer, "excellent")
	var weak := _class_visual(layer, "weak")
	_check(float(excellent.get("min_px", 0.0)) > float(weak.get("min_px", 0.0)),
			"excelente lê maior que fraco (%.0f > %.0f)"
			% [float(excellent.get("min_px", 0.0)),
					float(weak.get("min_px", 0.0))])
	layer.set_energy_zoom_band(0)
	var aggregate_min := 0.0
	for index in layer.energy_marker_count():
		var visual: Dictionary = layer.energy_marker_visual(index)
		if bool(visual.get("aggregate", false)):
			aggregate_min = float(visual.get("min_px", 0.0))
			break
	_check(aggregate_min > float(weak.get("min_px", 0.0)),
			"agregado de nação >= sítio fraco (%.0f > %.0f)"
			% [aggregate_min, float(weak.get("min_px", 0.0))])
	layer.set_energy_zoom_band(2)

	grid.set_overlays({"solar": true, "auto": "solar"})
	_check(grid.auto_heat() == "solar", "calor automático registra a fonte")
	var solar_mesh: MeshInstance3D = potential.get("solar")
	var solar_material := solar_mesh.material_override as StandardMaterial3D
	_check(solar_material != null
			and solar_material.albedo_color.a < 0.99,
			"calor automático entra em opacidade baixa (%.2f)"
			% (solar_material.albedo_color.a if solar_material != null else 1.0))
	grid.set_overlays({"solar": true})
	_check(grid.auto_heat().is_empty() and solar_material != null
			and solar_material.albedo_color.a > 0.99,
			"calor manual volta à opacidade cheia")

	print("")
	if _failures.is_empty():
		print("E6 OK: todos os checks passaram")
		quit(0)
	else:
		print("E6 FALHOU: %d check(s)" % _failures.size())
		for failure in _failures:
			print("  - " + failure)
		quit(1)


## Primeiro marcador de uma CLASSE na faixa atual (cor/piso do desenho). O
## agregado de nação fica de fora: ele tem piso próprio (+4 px).
func _class_visual(layer: Node3D, class_key: String) -> Dictionary:
	for index in layer.energy_marker_count():
		var visual: Dictionary = layer.energy_marker_visual(index)
		if String(visual.get("class", "")) == class_key \
				and not bool(visual.get("aggregate", false)):
			return visual
	return {}


## Valores esperados direto dos arquivos, com a MESMA regra do mapa: cidade com
## valor > 0. Os limiares de classe saem DESTA distribuição (sem os zeros).
func _expected_values(renewable: Dictionary,
		hydro: Dictionary) -> Dictionary:
	var solar := {}
	var wind := {}
	var hydro_positive := {}
	var rows: Dictionary = renewable.get("by_city", {})
	for city_id: Variant in rows:
		var row: Dictionary = rows[city_id]
		var solar_value := _json_number((row.get("solar", {}) as Dictionary)
				.get("pvout_kwh_kwp_year"))
		var wind_value := _json_number((row.get("wind", {}) as Dictionary)
				.get("power_density_w_m2"))
		if solar_value > 0.0:
			solar[city_id] = solar_value
		if wind_value > 0.0:
			wind[city_id] = wind_value
	var hydro_rows: Dictionary = hydro.get("by_city", {})
	for city_id: Variant in hydro_rows:
		var row: Dictionary = hydro_rows[city_id]
		var value := _json_number(row.get("potential_mw"))
		if value > 0.0:
			hydro_positive[city_id] = value
	return {
		"solar": solar,
		"wind": wind,
		"hydro": hydro_positive,
	}


## Quartis com a mesma fórmula declarada no map_mode_layer.gd:
## `sorted[floor(q * n)]` (clampado ao último índice).
func _quantiles(values: Dictionary) -> Array:
	var sorted: Array = values.values()
	sorted.sort()
	var count := sorted.size()
	var out: Array = []
	for q: float in QUANTILES:
		var index := mini(count - 1, int(floor(q * float(count))))
		out.append(snappedf(float(sorted[index]), 0.1))
	return out


func _marker_info(layer: Node3D, source: String, city_id: String) -> Dictionary:
	for index in layer.energy_marker_count():
		var info: Dictionary = layer.energy_marker_info(index)
		if String(info.get("source", "")) == source \
				and String(info.get("city_id", "")) == city_id:
			return info
	return {}


## MW construídos de uma fonte numa cidade, direto do parque atual.
func _built_mw(source: String, city_id: String) -> float:
	var data: Dictionary = JSON.parse_string(
			FileAccess.get_file_as_string(PLANTS_PATH))
	var total := 0.0
	for plant: Variant in data.get("plants", []):
		if not (plant is Dictionary):
			continue
		var row: Dictionary = plant
		if String(row.get("fuel_group", "")) != source \
				or String(row.get("city_id", "")) != city_id:
			continue
		total += _json_number(row.get("capacity_mw"))
	return total


## JSON traz `null` onde o dado falta; `float(null)` estoura no GDScript.
static func _json_number(value: Variant) -> float:
	if typeof(value) == TYPE_FLOAT or typeof(value) == TYPE_INT:
		return float(value)
	return 0.0


## Duas cidades de nações diferentes para ancorar a interconexão do teste.
func _two_nations(cities: Array) -> Dictionary:
	var first := {}
	for entry: Variant in cities:
		if not (entry is Dictionary):
			continue
		var city: Dictionary = entry
		var nation := String(city.get("nation", ""))
		if nation.is_empty():
			continue
		if first.is_empty():
			first = city
			continue
		if nation != String(first.get("nation", "")):
			return {
				"city_a": String(first.get("id", "")),
				"city_b": String(city.get("id", "")),
				"nation_a": String(first.get("nation", "")),
				"nation_b": nation,
			}
	return {}
