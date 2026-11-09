extends SceneTree

## Valida o orçamento de renderização do mapa: milhares de cidades não podem
## criar milhares de nós. Mede tempo de montagem e número de nós.
##
## Uso: godot --headless --script res://tools/city_render_benchmark.gd

const CityWorldScript := preload("res://map/city_world.gd")
const CITY_COUNT := 5000
const NODE_BUDGET := 6000


func _init() -> void:
	var world := CityWorldScript.new()
	get_root().add_child(world)

	var cities: Array = []
	for index in CITY_COUNT:
		cities.append({
			"x": 100.0 + float(index % 4800),
			"y": 100.0 + float((index * 37) % 2300),
			"name": "C%04d" % index,
			"nation": "FRA" if index % 2 == 0 else "DEU",
			"population": 50000.0 + float(index * 17 % 150000),
		})

	var start := Time.get_ticks_usec()
	world.build(cities, [])
	var elapsed_ms := (Time.get_ticks_usec() - start) / 1000.0
	var nodes := _count_nodes(world)
	print("[render] cidades=%d nós=%d tempo=%.1f ms orçamento=%d" % [
		CITY_COUNT, nodes, elapsed_ms, NODE_BUDGET])
	if nodes > NODE_BUDGET:
		print("[render] FALHA: orçamento de nós excedido")
		quit(1)
	else:
		print("[render] OK: renderização limitada por orçamento")
		quit(0)


func _count_nodes(node: Node) -> int:
	var total := 1
	for child in node.get_children():
		total += _count_nodes(child)
	return total
