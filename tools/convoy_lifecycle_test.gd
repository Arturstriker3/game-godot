extends Node3D

## Ciclo de vida dos comboios, headless: roda 20 dias da economia real e vigia,
## frame a frame, cada comboio ativo. Falha com != 0 quando:
##   - um comboio visível fica sem veículo visível (só medalhão);
##   - um veículo ativo fica invisível ou abaixo do plano do mapa;
##   - um nó volta ao pool visível/ com filho, ou há tween sem comboio;
##   - um comboio nasce sem nenhuma rota utilizável (aparece e some sem chegar);
##   - o pool devolve nó com pai (`already has a parent` no `add_child`);
##   - `stats.spawned != stats.finished + stats.cancelled` (viagem sem desfecho).
##
## Uso:
##   godot --headless --path . res://tools/convoy_lifecycle_test.tscn

const ConvoysScript: Script = preload("res://map/convoy_spacing.gd")

const DAYS := 20
## Acelera só o tween (o relógio da economia fica congelado): 1 dia de trecho
## vira ~0,25 s. A visibilidade é checada a cada frame, como no jogo.
const TWEEN_RATE := 240.0
const MAX_FRAMES_PER_DAY := 4000
const MAP_LAYER_Y := 0.13

var _convoys: Node3D
var _fail: Array[String] = []
var _trip: Dictionary = {}
var _prev_active: Dictionary = {}
var _trips := 0
var _finished_total := 0
var _ghosts := 0
var _medallion_frames := 0
var _invisible_frames := 0
var _below_map_frames := 0
var _stale_tween_frames := 0
var _pool_visible := 0
var _pool_with_children := 0
var _pool_vehicle_visible := 0
var _empty_routes := 0
var _ok_routes := 0
var _parent_errors := 0
var _tween_mismatch := 0


func _ready() -> void:
	Engine.max_fps = 240
	Economy.set_speed(0)
	var sim: Object = Economy.sim
	var ai: Object = Economy._ai
	if sim == null or ai == null:
		print("[convoy] autoload Economy sem sim/IA")
		get_tree().quit(1)
		return

	_convoys = ConvoysScript.new()
	add_child(_convoys)
	_convoys.set_time_scale(TWEEN_RATE, false)
	await get_tree().process_frame

	# O autoload agenda o 1º dia numa thread (tick_scheduler). Enquanto ela roda,
	# ninguém pode escrever no `sim`: esperar aqui evita dois ticks simultâneos.
	if Economy._ticker.is_running():
		Economy._ticker.wait()

	for day in range(1, DAYS + 1):
		var summary: Dictionary = sim.tick_day()
		ai.run(day)
		var orders: Array = summary.get("trade_orders", [])
		var trips_before := _trips
		var finished_before := _finished_total
		_convoys.spawn_orders(orders)
		var frames := 0
		while frames < MAX_FRAMES_PER_DAY:
			await get_tree().process_frame
			frames += 1
			_check_frame(day)
			if _idle():
				break
		print("[convoy] dia %2d: %2d pedidos, %d novos, %d terminaram, %d ativos, %d frames" % [
			day, orders.size(), _trips - trips_before,
			_finished_total - finished_before, _convoys._convoys.size(), frames])

	_audit_routes()
	_audit_pool_parents()
	_final_checks()
	if _fail.is_empty():
		print("[convoy] OK: %d dias, %d viagens e %d chegadas; nenhum " \
				% [DAYS, _trips, _finished_total]
				+ "medalhão sem veículo, veículo invisível ou tween órfão.")
		get_tree().quit(0)
	else:
		print("[convoy] FALHAS (%d):" % _fail.size())
		for failure in _fail:
			print(" - " + failure)
		get_tree().quit(1)


func _idle() -> bool:
	return _convoys._queue.is_empty() and _convoys._prep.is_empty() \
			and not _convoys._nav.active and _convoys._convoys.is_empty()


func _check_frame(day: int) -> void:
	var now: Dictionary = {}
	for convoy in _convoys._convoys:
		if not is_instance_valid(convoy):
			_fail.append("dia %d: comboio inválido na lista de ativos" % day)
			continue
		var id: int = int(convoy.get_instance_id())
		now[id] = true
		# Nós são reciclados: uma "viagem" começa quando o nó entra em
		# `_convoys` e termina quando sai.
		if not _prev_active.has(id):
			_trips += 1
			_trip[id] = {"day": day, "had_vehicle": false, "frames": 0}
		var entry: Dictionary = _trip[id]
		entry["frames"] = int(entry["frames"]) + 1
		if not convoy.visible or not convoy.is_visible_in_tree():
			_fail.append("dia %d: comboio ativo invisível (modo %s)" % [
				day, String(convoy.get_meta("mode", "?"))])
		var flags := convoy.get_node_or_null("Flags") as Sprite3D
		var vehicle := _vehicle_of(convoy)
		if vehicle == null:
			# Sem veículo: só no primeiro frame do comboio. Depois disso é
			# medalhão solto no mapa.
			if int(entry["frames"]) > 1 and flags != null and flags.visible:
				_medallion_frames += 1
				if _medallion_frames <= 3:
					_fail.append("dia %d: comboio %s com medalhão e sem veículo" % [
						day, String(convoy.get_meta("mode", "?"))])
		else:
			entry["had_vehicle"] = true
			if not _mesh_visible(vehicle):
				_invisible_frames += 1
				if _invisible_frames <= 3:
					_fail.append("dia %d: veículo %s invisível com comboio ativo" % [
						day, String(vehicle.name)])
			var y := vehicle.get_global_transform().origin.y
			if y < MAP_LAYER_Y:
				_below_map_frames += 1
				if _below_map_frames <= 3:
					_fail.append("dia %d: veículo %s abaixo do mapa (y=%.3f)" % [
						day, String(vehicle.name), y])

	# Saídas: o comboio saiu de `_convoys` (chegou ao destino).
	for id in _prev_active.keys():
		if not now.has(id):
			_finished_total += 1
			if _trip.has(id) and not bool(_trip[id]["had_vehicle"]):
				_ghosts += 1
				_fail.append("dia %d: comboio nascido no dia %s terminou sem nunca mostrar veículo" % [
					day, str(_trip[id]["day"])])
			_trip.erase(id)
	_prev_active = now

	# Tweens: um por comboio ativo, nenhum sobrando.
	var live := 0
	for tween in _convoys._tweens:
		if tween != null and tween.is_valid():
			live += 1
	if live > _convoys._convoys.size():
		_stale_tween_frames += 1
		if _stale_tween_frames <= 3:
			_fail.append("dia %d: %d tweens vivos para %d comboios" % [
				day, live, _convoys._convoys.size()])
	# Um tween vivo por comboio ativo, e nenhum tween de geração antiga.
	if live != _convoys._convoys.size() \
			or _convoys._tween_by_convoy.size() != _convoys._convoys.size():
		_tween_mismatch += 1
		if _tween_mismatch <= 3:
			_fail.append("dia %d: %d tweens vivos / %d no mapa para %d comboios" % [
				day, live, _convoys._tween_by_convoy.size(), _convoys._convoys.size()])

	# Pool: nada visível e nenhum comboio com filho pendurado.
	for pooled in _convoys._convoy_pool:
		if is_instance_valid(pooled) and pooled.visible:
			_pool_visible += 1
			if _pool_visible <= 3:
				_fail.append("dia %d: comboio no pool ainda visível" % day)
		if is_instance_valid(pooled) and pooled.get_child_count() > 1:
			_pool_with_children += 1
	# Veículos estacionados no pool não podem estar visíveis.
	for mode in _convoys._vehicle_pool.keys():
		for model in _convoys._vehicle_pool[mode]:
			if is_instance_valid(model) and model.visible:
				_pool_vehicle_visible += 1
				if _pool_vehicle_visible <= 3:
					_fail.append("dia %d: veículo %s visível dentro do pool" % [
						day, String(model.name)])


func _vehicle_of(convoy: Node3D) -> Node3D:
	for child in convoy.get_children():
		if child is Node3D and String(child.name) != "Flags":
			return child as Node3D
	return null


func _mesh_visible(node: Node) -> bool:
	if not node.is_visible_in_tree():
		return false
	var stack: Array[Node] = [node]
	while not stack.is_empty():
		var current: Node = stack.pop_back()
		if current is MeshInstance3D and current.mesh != null:
			return true
		stack.append_array(current.get_children())
	return false


func _audit_routes() -> void:
	for key in _convoys._path_cache.keys():
		var path: PackedVector2Array = _convoys._path_cache[key]
		if path.size() < 2:
			_empty_routes += 1
		else:
			_ok_routes += 1


## Invariante do bug dos caminhões: o pool devolve o nó sem pai (senão o
## `add_child` no comboio falha e o comboio fica só com o medalhão) e todo nó
## parado está de fato pendurado no `VehiclePool`, invisível.
func _audit_pool_parents() -> void:
	for mode in _convoys._vehicle_pool.keys():
		var pool: Array = _convoys._vehicle_pool[mode]
		for model in pool:
			if not is_instance_valid(model):
				continue
			if model.get_parent() != _convoys._pool_root:
				_parent_errors += 1
				if _parent_errors <= 3:
					_fail.append("veículo %s do pool com pai %s" % [
						String(model.name), str(model.get_parent())])
			if model.visible:
				_parent_errors += 1
				if _parent_errors <= 3:
					_fail.append("veículo %s do pool ainda visível" % String(model.name))
			if model.get_meta("vehicle_mode", "") != mode:
				_parent_errors += 1
				if _parent_errors <= 3:
					_fail.append("veículo %s no pool de %s mas marcado %s" % [
						String(model.name), mode, str(model.get_meta("vehicle_mode", ""))])
	# Um nó vindo do pool precisa chegar sem pai para o comboio aceitá-lo.
	for mode in _convoys._vehicle_pool.keys():
		var pool: Array = _convoys._vehicle_pool[mode]
		if pool.is_empty():
			continue
		var peek: Node3D = _convoys._acquire_vehicle(mode)
		if peek.get_parent() != null:
			_parent_errors += 1
			_fail.append("_acquire_vehicle(%s) devolveu nó com pai %s" % [
				mode, str(peek.get_parent())])
		_convoys._park_vehicle(peek, mode)


func _final_checks() -> void:
	# O laço de cada dia só termina quando não há comboio ativo: se sobrou
	# algum, é falha (não deveria acontecer com MAX_FRAMES_PER_DAY).
	if _trips == 0:
		_fail.append("nenhum comboio nasceu em %d dias" % DAYS)
	if _trips != _finished_total:
		_fail.append("%d viagens começaram e %d terminaram" % [
			_trips, _finished_total])
	if _ghosts > 0:
		_fail.append("%d comboio(s) nasceram sem rota e nunca mostraram veículo" % _ghosts)
	if _medallion_frames > 0:
		_fail.append("%d frame(s) com medalhão sem veículo" % _medallion_frames)
	if _invisible_frames > 0:
		_fail.append("%d frame(s) com veículo invisível" % _invisible_frames)
	if _below_map_frames > 0:
		_fail.append("%d frame(s) com veículo abaixo do mapa" % _below_map_frames)
	if _stale_tween_frames > 0:
		_fail.append("%d frame(s) com tween órfão" % _stale_tween_frames)
	if _pool_visible > 0 or _pool_with_children > 0 or _pool_vehicle_visible > 0:
		_fail.append("pool sujo (comboio visível=%d, com filho=%d, veículo visível=%d)" % [
			_pool_visible, _pool_with_children, _pool_vehicle_visible])
	if _parent_errors > 0:
		_fail.append("%d violação(ões) do pool de veículos (pai/visibilidade/modo)" % _parent_errors)
	if _convoys._convoys.size() > 0:
		_fail.append("%d comboio(s) ainda ativos no fim" % _convoys._convoys.size())
	if _convoys._convoy_pool.size() > _convoys.MAX_CONVOYS:
		_fail.append("pool de comboios com %d nós (teto %d)" % [
			_convoys._convoy_pool.size(), _convoys.MAX_CONVOYS])
	# Contadores de vida: toda tentativa de spawn teve um desfecho registrado.
	var stats: Dictionary = _convoys.stats
	var spawned := int(stats["spawned"])
	var finished := int(stats["finished"])
	var cancelled := int(stats["cancelled"])
	var reasons := int(stats["no_legs"]) + int(stats["no_route"]) + int(stats["no_slot"])
	if spawned != finished + cancelled:
		_fail.append("stats: %d tentativas != %d chegadas + %d canceladas" % [
			spawned, finished, cancelled])
	if reasons != cancelled:
		_fail.append("stats: %d canceladas sem motivo (motivos somam %d)" % [
			cancelled, reasons])
	if finished != stats["placed"]:
		_fail.append("stats: %d chegadas != %d comboios que nasceram com veículo" % [
			finished, int(stats["placed"])])
	if int(stats["placed"]) != spawned - cancelled:
		_fail.append("stats: %d comboios com veículo != %d tentativas - %d canceladas" % [
			int(stats["placed"]), spawned, cancelled])
	if _tween_mismatch > 0:
		_fail.append("%d frame(s) com tween a mais/menos que comboio" % _tween_mismatch)
	if not _convoys._tweens.is_empty() or not _convoys._tween_by_convoy.is_empty():
		_fail.append("sobrou tween no fim: %d na lista, %d no mapa" % [
			_convoys._tweens.size(), _convoys._tween_by_convoy.size()])
	print("[convoy] rotas no cache: %d ok, %d vazias" % [_ok_routes, _empty_routes])
	print("[convoy] stats: %d tentativas, %d com veículo, %d chegadas, %d canceladas " \
			% [spawned, int(stats["placed"]), finished, cancelled]
			+ "(%d sem trecho, %d sem rota, %d sem vaga)" % [
				int(stats["no_legs"]), int(stats["no_route"]), int(stats["no_slot"])])
	print("[convoy] %d viagens, %d terminaram, %d fantasmas" % [
		_trips, _finished_total, _ghosts])
