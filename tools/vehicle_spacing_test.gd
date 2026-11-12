extends Node3D

## Teste headless da separação de veículos (`map/convoy_spacing.gd`).
##
## Cobre o que a captura não consegue garantir com pedidos reais:
##   - caminhão 33% menor que a escala anterior (0,460625 u de comprimento);
##   - dois caminhões no mesmo ponto se separam por faixa (lateral) e, na mesma
##     faixa, pela fila ao longo da rota — sem sair do terreno;
##   - dois aviões no mesmo ponto ganham camadas de altitude e offset lateral;
##   - navios preservam o empurrão radial curto.
##
## Uso:
##   godot --headless --path . res://tools/vehicle_spacing_test.tscn

const ConvoysScript: Script = preload("res://map/convoy_spacing.gd")
const TRUCK_SCENE: PackedScene = preload("res://assets/models/truck.glb")

var _convoy: Node3D
var _fail: Array[String] = []


func _ready() -> void:
	_convoy = ConvoysScript.new()
	add_child(_convoy)
	var land: Vector3 = _find_point("truck")
	var water: Vector3 = _find_point("ship")
	if land == Vector3(INF, INF, INF) or water == Vector3(INF, INF, INF):
		print("[spacing] FALHA: não encontrei terra/água na máscara")
		get_tree().quit(1)
		return

	_check_scale()
	_check_trucks_lanes(land)
	_check_trucks_queue(land)
	_check_planes(land)
	_check_ships(water)
	_check_cost(land)

	if _fail.is_empty():
		print("[spacing] OK: escala, faixas de caminhão, fila, aviões e navios.")
		get_tree().quit(0)
	else:
		print("[spacing] FALHAS (%d):" % _fail.size())
		for failure in _fail:
			print(" - " + failure)
		get_tree().quit(1)


func _check_scale() -> void:
	var scale_value: float = _convoy._scale_for("truck", TRUCK_SCENE)
	var length := scale_value * 1.02
	var expected := 0.6875 * 0.67
	if absf(length - expected) > 0.002:
		_fail.append("caminhão mede %.5f u (esperado %.5f u, 67%% do antigo)" % [
			length, expected])
	print("[spacing] caminhão: %.5f u de comprimento (base 0,6875 × 0,67)" % length)


func _check_trucks_lanes(land: Vector3) -> void:
	var first := _make("truck", 0, 0.12)
	var second := _make("truck", 2, 0.12)
	_place(first, land)
	_place(second, land)
	_step(first, land)
	_step(second, land, 20)
	var gap := _planar_distance(first.position, second.position)
	if gap < 0.25:
		_fail.append("caminhões em faixas vizinhas a %.3f u (esperado ≥ 0,25)" % gap)
	if not _walkable(first.position, "truck") or not _walkable(second.position, "truck"):
		_fail.append("faixa de caminhão saiu do terreno")
	print("[spacing] caminhões em faixas 0/2: distância %.3f u" % gap)
	_free(first)
	_free(second)


func _check_trucks_queue(land: Vector3) -> void:
	# Mesma faixa: o desempate por id manda um para cada lado da fila.
	var first := _make("truck", 0, 0.12)
	var second := _make("truck", 3, 0.12)
	_place(first, land)
	_place(second, land)
	_step(first, land)
	_step(second, land, 20)
	var gap := _planar_distance(first.position, second.position)
	if gap < 0.4:
		_fail.append("caminhões na mesma faixa a %.3f u (esperado ≥ 0,40)" % gap)
	if not _walkable(first.position, "truck") or not _walkable(second.position, "truck"):
		_fail.append("fila de caminhão saiu do terreno")
	print("[spacing] caminhões na mesma faixa: distância %.3f u" % gap)
	_free(first)
	_free(second)


func _check_planes(land: Vector3) -> void:
	var first := _make("plane", 0, 0.16)
	var second := _make("plane", 2, 0.16)
	_place(first, land)
	_place(second, land)
	_step(first, land)
	_step(second, land, 20)
	var gap := _planar_distance(first.position, second.position)
	var altitude := absf(first.position.y - second.position.y)
	if gap < 0.4:
		_fail.append("aviões a %.3f u na horizontal (esperado ≥ 0,40)" % gap)
	if altitude < 0.1:
		_fail.append("aviões com %.3f u de altitude entre camadas (esperado ≥ 0,10)" % altitude)
	print("[spacing] aviões: distância %.3f u, altitude %.3f u" % [gap, altitude])
	_free(first)
	_free(second)


func _check_ships(water: Vector3) -> void:
	var first := _make("ship", 0, 0.09)
	var second := _make("ship", 0, 0.09)
	_place(first, water)
	_place(second, water)
	_step(first, water)
	_step(second, water, 20)
	var gap := _planar_distance(first.position, second.position)
	if gap < 0.12:
		_fail.append("navios a %.3f u (esperado ≥ 0,12)" % gap)
	if not _walkable(first.position, "ship") or not _walkable(second.position, "ship"):
		_fail.append("navio saiu da água")
	print("[spacing] navios: distância %.3f u" % gap)
	_free(first)
	_free(second)


## Pior caso barato: 24 caminhões no mesmo ponto (teto de comboios visíveis).
## Mede o custo por frame do posicionamento + consulta das 9 células.
func _check_cost(land: Vector3) -> void:
	var nodes: Array[Node3D] = []
	for index in 24:
		var node := _make("truck", index % 3, 0.12)
		_place(node, land)
		nodes.append(node)
	var start := land + Vector3(-0.5, 0.0, 0.0)
	var end := land + Vector3(0.5, 0.0, 0.0)
	for node in nodes:
		_convoy._move_vehicle(0.5, node, start, end)
	var started := Time.get_ticks_usec()
	for _frame in 120:
		for node in nodes:
			_convoy._move_vehicle(0.5, node, start, end)
	var per_frame_ms := float(Time.get_ticks_usec() - started) / 120000.0
	print("[spacing] custo: %.3f ms/frame com 24 caminhões no mesmo ponto" % per_frame_ms)
	if per_frame_ms > 2.0:
		_fail.append("24 caminhões custam %.2f ms/frame (teto 2,0)" % per_frame_ms)
	for node in nodes:
		_free(node)


# --- Utilitários -----------------------------------------------------------

func _make(mode: String, lane: int, radius: float) -> Node3D:
	var node := Node3D.new()
	node.set_meta("mode", mode)
	node.set_meta("lane_slot", lane)
	node.set_meta("collision_radius", radius)
	_convoy.add_child(node)
	return node


func _place(node: Node3D, point: Vector3) -> void:
	node.position = point + Vector3(0.0, 0.18, 0.0)
	node.set_meta("separation", Vector3.ZERO)
	_convoy._space_update(node)


## Coloca o comboio em `point` via `_move_vehicle` (peso 0,5 entre extremos
## simétricos) e repete para a suavização convergir.
func _step(node: Node3D, point: Vector3, times := 20) -> void:
	var start := point + Vector3(-0.5, 0.0, 0.0)
	var end := point + Vector3(0.5, 0.0, 0.0)
	for _i in times:
		_convoy._move_vehicle(0.5, node, start, end)


func _free(node: Node3D) -> void:
	_convoy._space_remove(node)
	node.queue_free()


func _planar_distance(a: Vector3, b: Vector3) -> float:
	return Vector2(a.x - b.x, a.z - b.z).length()


func _walkable(position: Vector3, mode: String) -> bool:
	return _convoy._nav.walkable(
			_convoy._to_cell(_convoy._world_to_pixel(position)), mode)


## Primeiro ponto navegável (terra para caminhão, água para navio) na máscara,
## numa janela ao redor da Europa para o teste ser rápido e estável.
func _find_point(mode: String) -> Vector3:
	for y in range(700, 1700, 4):
		for x in range(2100, 3000, 4):
			var cell := Vector2i(x, y)
			if not _is_open(cell, mode):
				continue
			var pixel := Vector2(cell) * 2.0 + Vector2.ONE
			return _convoy._pixel_to_world(pixel) + Vector3(0.0, 0.18, 0.0)
	return Vector3(INF, INF, INF)


## Exige margem de células iguais em volta: a faixa de 0,16 u (~0,8 px) não
## pode cair no terreno oposto e mascarar o teste.
func _is_open(cell: Vector2i, mode: String) -> bool:
	for dy in range(-3, 4):
		for dx in range(-3, 4):
			if not _convoy._nav.walkable(cell + Vector2i(dx, dy), mode):
				return false
	return true
