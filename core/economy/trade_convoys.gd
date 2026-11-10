extends Node3D

## Anima os pedidos de comércio no mapa: o veículo de cada trecho (caminhão,
## navio ou avião) sai do ponto de origem e segue apontado para o destino.
##
## Caminhões andam só por terra e navios só por água, usando a máscara de
## navegação gerada a partir das províncias (map_data/nav_mask.png).

# Mesma conversão usada pelos labels 3D do mapa (pixel -> mundo).
const PIXEL_TO_WORLD := 0.2
const IMAGE_HALF_SIZE := Vector2(2500.0, 1250.0)

const TRUCK_SCENE: PackedScene = preload("res://assets/models/truck.glb")
const SHIP_SCENE: PackedScene = preload("res://assets/models/ship.glb")
const PLANE_SCENE: PackedScene = preload("res://assets/models/plane.glb")

# Tamanho alvo (unidades de mundo) do maior eixo do modelo. Veículos devem ser
# pequenos diante da cidade (a cidade tem ~6–8 unidades de diâmetro). O navio é
# 2x maior: no mar ele não compete com a malha urbana e fica legível.
const MODEL_LENGTH := 0.275
const MODEL_LENGTH_BY_MODE := {"truck": 0.275, "ship": 0.6875, "plane": 0.275}
# Altura acima do plano do mapa para o modelo não afundar no terreno.
const HEIGHT_OFFSET := 0.18
# Bandeirinha da nação de destino, acima do veículo (identifica o comboio).
const FLAG_HEIGHT := 0.34
# Medalhões origem+destino: textura do par gerada em runtime (selo escuro + aro
# de latão + bandeira), lado a lado com um chevron de latão entre eles.
const PAIR_TEX_W := 180
const PAIR_TEX_H := 72
const PAIR_HEIGHT := 0.17  # altura do conjunto em unidades de mundo
# Duração visual de cada trecho: acompanha o tempo de trânsito simulado e o
# ritmo do relógio (1 dia = 60 s). Veículos deixam de "teleportar".
const SECONDS_PER_DAY := 60.0
## Piso de duração por modo. O avião é rápido demais para o olho (transit_hours
## mínimo), então ganha um piso maior; caminhão e navio já ficam legíveis.
const MIN_SEGMENT_BY_MODE := {"truck": 2.0, "ship": 4.5, "plane": 10.0}
const MAX_SEGMENT_SECONDS := 45.0
## Teto de velocidade visual por modo (unidades de mundo por segundo). O teto
## de 45 s vale para trechos curtos, mas numa travessia longa (Atlântico) ele
## faria o navio cruzar o oceano num piscar; abaixo disso o trecho ganha mais
## tempo. O avião mantém um teto maior para continuar visivelmente mais rápido.
const MAX_VISUAL_SPEED_BY_MODE := {"truck": 1.2, "ship": 0.7, "plane": 1.8}
const MAX_CONVOYS := 8
# Escalonamento da partida (segundos de tween): comboios que nascem no mesmo
# tick saem em instantes diferentes e próximos, formando uma linha (cara de
# comboio). Quando as rotas divergem, cada um segue para o seu destino.
const DEPART_STAGGER := 0.4
const DEPART_STAGGER_STEPS := 8
# Colisão bem pequena (largura de encosto) e suavizada: só separa quando dois
# veículos praticamente se tocam, sem "salto". O empurrão é descartado se for
# tirar o veículo do terreno correto (nunca joga caminhão na água nem navio na
# terra).
const COLLISION_RADIUS_BY_MODE := {"truck": 0.05, "ship": 0.09, "plane": 0.05}
const SEPARATION_SMOOTHING := 0.15

# Os modelos foram feitos com a frente no eixo +X. O Godot usa -Z como frente
# (é o eixo que look_at() aponta para o alvo), então giramos +90° em Y.
const MODEL_YAW_OFFSET := PI * 0.5

# Navegação: cada célula da máscara cobre 2x2 px do mapa.
# Valores: 255 = só terra, 0 = só água, 128 = costa (misto).
const NAV_MASK: Texture2D = preload("res://map/map_data/nav_mask.png")
const NAV_CELL_PX := 2.0
const COAST_THRESHOLD := 0.01
# Raio de busca da célula navegável mais próxima (snap). Navios saem de cidades
# do interior (ex.: Paris fica ~15 células da costa), então o raio precisa ser
# grande o bastante para alcançar o mar — senão o snap falha e o navio cruza o
# terreno em linha reta.
const SNAP_SEARCH_CELLS := 128
# Nós visitados pelo A* antes de desistir. Rotas oceânicas longas (Europa ->
# América do Sul) exploram um corredor de milhares de células; um limite baixo
# fazia o navio cair no fallback em linha reta (por cima do terreno).
const MAX_ASTAR_NODES := 200000
# Salto de olhar máximo ao simplificar o caminho (pontos por waypoint). Limita o
# custo a O(n) em vez de O(n²), para rotas oceânicas longas não travarem o jogo.
const SIMPLIFY_LOOKAHEAD := 48

const NEIGHBORS: Array[Vector2i] = [
	Vector2i(1, 0), Vector2i(-1, 0), Vector2i(0, 1), Vector2i(0, -1),
	Vector2i(1, 1), Vector2i(1, -1), Vector2i(-1, 1), Vector2i(-1, -1),
]

var _active_convoys := 0
var _depart_slot := 0
var _convoys: Array[Node3D] = []
var _scale_cache := {}
var _pair_cache := {}
var _nav_image: Image
var _path_cache := {}
## Tweens em andamento, para acompanhar a velocidade/pausa da simulação.
var _tweens: Array[Tween] = []
var _time_scale := 1.0
var _paused := false


## Fila de prioridade mínima simples (heap binário) para o A*.
class MinHeap:
	var _items: Array = []

	func is_empty() -> bool:
		return _items.is_empty()

	func push(priority: float, cell: Vector2i) -> void:
		_items.append([priority, cell])
		var index: int = _items.size() - 1
		while index > 0:
			var parent: int = (index - 1) / 2
			if _items[parent][0] <= _items[index][0]:
				break
			var swap: Array = _items[parent]
			_items[parent] = _items[index]
			_items[index] = swap
			index = parent

	func pop() -> Vector2i:
		var top: Vector2i = _items[0][1]
		var last: Array = _items.pop_back()
		if not _items.is_empty():
			_items[0] = last
			var index := 0
			while true:
				var left: int = index * 2 + 1
				var right: int = left + 1
				var smallest := index
				if left < _items.size() and _items[left][0] < _items[smallest][0]:
					smallest = left
				if right < _items.size() and _items[right][0] < _items[smallest][0]:
					smallest = right
				if smallest == index:
					break
				var swap: Array = _items[smallest]
				_items[smallest] = _items[index]
				_items[index] = swap
				index = smallest
		return top


func _ready() -> void:
	_nav_image = NAV_MASK.get_image()


## Cria os comboios de um tick de comércio.
func spawn_orders(orders: Array) -> void:
	for order in _prioritize(orders):
		if _active_convoys >= MAX_CONVOYS:
			return
		_spawn_order(order)


## Pedidos entre nações primeiro: são os que mostram navio e avião no mapa.
func _prioritize(orders: Array) -> Array:
	var sorted := orders.duplicate()
	sorted.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		var a_cross: bool = a["from_nation"] != a["to_nation"]
		var b_cross: bool = b["from_nation"] != b["to_nation"]
		return a_cross and not b_cross
	)
	return sorted


func _spawn_order(order: Dictionary) -> void:
	var legs: Array = order.get("legs", [])
	if legs.is_empty():
		return

	var convoy := Node3D.new()
	add_child(convoy)
	_active_convoys += 1
	_convoys.append(convoy)
	# Bandeira da nação que vai RECEBER o produto, acima do veículo.
	_add_flags(convoy, String(order.get("from_nation", "")),
			String(order.get("to_nation", "")))

	var tween := create_tween()
	# O tween nasce no ritmo atual da simulação (1x = 60 s por dia) e entra na
	# lista para acompanhar mudanças de velocidade e pausa.
	tween.set_speed_scale(_time_scale)
	if _paused:
		tween.pause()
	_tweens.append(tween)
	tween.finished.connect(_on_tween_finished.bind(tween))
	# Escalonamento de partida: comboios do mesmo tick saem em instantes
	# diferentes, evitando que andem colados sem tirar ninguém do caminho.
	var delay := DEPART_STAGGER * float(_depart_slot % DEPART_STAGGER_STEPS)
	_depart_slot += 1
	if delay > 0.0:
		tween.tween_interval(delay)
	for leg in legs:
		_append_leg(tween, convoy, leg)

	tween.tween_callback(_finish_convoy.bind(convoy))


## Acompanha o relógio da economia: `rate` = quantas vezes o ritmo normal
## (1x). `paused` congela os veículos existentes até o play.
func set_time_scale(rate: float, paused: bool) -> void:
	_time_scale = maxf(rate, 0.01)
	_paused = paused
	for index in range(_tweens.size() - 1, -1, -1):
		var tween: Tween = _tweens[index]
		if tween == null or not tween.is_valid():
			_tweens.remove_at(index)
			continue
		tween.set_speed_scale(_time_scale)
		if _paused:
			tween.pause()
		else:
			tween.play()


func _on_tween_finished(tween: Tween) -> void:
	_tweens.erase(tween)


## Enfileira no tween cada segmento do caminho do trecho.
func _append_leg(tween: Tween, convoy: Node3D, leg: Dictionary) -> void:
	var mode: String = leg["mode"]
	var path := _leg_path(leg["from_px"], leg["to_px"], mode)
	if path.size() < 2:
		return

	var total_px := 0.0
	for i in range(1, path.size()):
		total_px += path[i].distance_to(path[i - 1])
	if total_px <= 0.0:
		return

	var duration := _segment_seconds(mode, float(leg.get("transit_hours", 0.0)),
			float(leg.get("distance_km", 0.0)), total_px)

	var first_start := _pixel_to_world(path[0])
	var first_end := _pixel_to_world(path[1])
	tween.tween_callback(_place_vehicle.bind(convoy, mode, first_start, first_end))

	for i in range(1, path.size()):
		var start := _pixel_to_world(path[i - 1])
		var end := _pixel_to_world(path[i])
		var step_seconds: float = maxf(duration * start.distance_to(end) / total_px, 0.05)
		if i > 1:
			tween.tween_callback(_face_target.bind(convoy, start, end))
		tween.tween_method(_move_vehicle.bind(convoy, start, end), 0.0, 1.0, step_seconds)


## Duração visual do trecho: acompanha o tempo simulado, mas nunca deixa o
## veículo ultrapassar MAX_VISUAL_SPEED_BY_MODE — o teto fixo de 45 s comprimia
## rotas oceânicas longas (ex.: Guiana–Europa) a uma velocidade absurda.
static func _segment_seconds(mode: String, transit_hours: float,
		distance_km: float, total_px: float) -> float:
	var min_seconds: float = float(MIN_SEGMENT_BY_MODE.get(mode, 2.0))
	var duration: float
	if transit_hours > 0.0:
		duration = clampf(transit_hours / 24.0 * SECONDS_PER_DAY,
				min_seconds, MAX_SEGMENT_SECONDS)
	else:
		# Fallback: ~2 h por 200 km de caminhão.
		duration = clampf(distance_km / 200.0 * 8.0,
				min_seconds, MAX_SEGMENT_SECONDS)
	var length_world := total_px * PIXEL_TO_WORLD
	var max_speed: float = float(MAX_VISUAL_SPEED_BY_MODE.get(mode, 1.0))
	return maxf(duration, length_world / max_speed)


## Caminho em pixels do trecho; avião vai em linha reta.
func _leg_path(from_px: Vector2, to_px: Vector2, mode: String) -> PackedVector2Array:
	if mode == "plane":
		return PackedVector2Array([from_px, to_px])

	var key := "%s|%.0f,%.0f|%.0f,%.0f" % [mode, from_px.x, from_px.y, to_px.x, to_px.y]
	if _path_cache.has(key):
		return _path_cache[key]

	var path := _find_path(from_px, to_px, mode)
	_path_cache[key] = path
	return path


func _find_path(from_px: Vector2, to_px: Vector2, mode: String) -> PackedVector2Array:
	var start := _snap_cell(_to_cell(from_px), mode)
	var goal := _snap_cell(_to_cell(to_px), mode)
	# Sem célula navegável (snap ou A* falhou): NÃO desenha linha reta. Uma reta
	# cruzaria o terreno errado (caminhão sobre o mar, navio sobre a terra).
	# Devolve vazio: o trecho é pulado (o comércio acontece, mas sem veículo).
	if start.x < 0 or goal.x < 0:
		return PackedVector2Array()

	var cells := _astar(start, goal, mode)
	if cells.is_empty():
		return PackedVector2Array()

	var path := PackedVector2Array()
	for cell in cells:
		path.append(_cell_to_pixel(cell))
	return _simplify(path, mode)


func _astar(start: Vector2i, goal: Vector2i, mode: String) -> Array[Vector2i]:
	var open := MinHeap.new()
	var came_from := {}
	var cost := {start: 0.0}
	var visited := 0
	open.push(_heuristic(start, goal), start)

	while not open.is_empty() and visited < MAX_ASTAR_NODES:
		var current: Vector2i = open.pop()
		visited += 1
		if current == goal:
			return _reconstruct(came_from, current)

		for direction in NEIGHBORS:
			var next: Vector2i = current + direction
			if not _is_walkable(next, mode):
				continue
			var diagonal: bool = direction.x != 0 and direction.y != 0
			if diagonal and (
				not _is_walkable(current + Vector2i(direction.x, 0), mode)
				or not _is_walkable(current + Vector2i(0, direction.y), mode)
			):
				continue

			var step: float = sqrt(2.0) if diagonal else 1.0
			var tentative: float = float(cost[current]) + step
			if cost.has(next) and tentative >= float(cost[next]):
				continue

			came_from[next] = current
			cost[next] = tentative
			open.push(tentative + _heuristic(next, goal), next)

	return []


func _reconstruct(came_from: Dictionary, current: Vector2i) -> Array[Vector2i]:
	var cells: Array[Vector2i] = [current]
	while came_from.has(current):
		current = came_from[current]
		cells.append(current)
	cells.reverse()
	return cells


## Remove pontos intermediários quando a reta entre eles é navegável. O salto de
## olhar é limitado (SIMPLIFY_LOOKAHEAD) para não virar O(n²) em rotas longas —
## antes, uma travessia oceânica travava o jogo por minutos ao simplificar.
func _simplify(path: PackedVector2Array, mode: String) -> PackedVector2Array:
	if path.size() <= 2:
		return path

	var result := PackedVector2Array([path[0]])
	var anchor := 0
	for i in range(2, path.size()):
		if i - anchor > SIMPLIFY_LOOKAHEAD \
				or not _line_walkable(path[anchor], path[i], mode):
			result.append(path[i - 1])
			anchor = i - 1
	result.append(path[path.size() - 1])
	return result


func _line_walkable(a: Vector2, b: Vector2, mode: String) -> bool:
	var steps: int = int(maxf(absf(a.x - b.x), absf(a.y - b.y)) / NAV_CELL_PX)
	for i in range(steps + 1):
		var t: float = float(i) / float(maxi(steps, 1))
		if not _is_walkable(_to_cell(a.lerp(b, t)), mode):
			return false
	return true


## Célula utilizável pelo modo; se o ponto caiu na costa, busca a mais próxima.
func _snap_cell(cell: Vector2i, mode: String) -> Vector2i:
	if _is_walkable(cell, mode):
		return cell

	var best := Vector2i(-1, -1)
	var best_distance := INF
	for radius in range(1, SNAP_SEARCH_CELLS + 1):
		for dy in range(-radius, radius + 1):
			for dx in range(-radius, radius + 1):
				if maxi(absi(dx), absi(dy)) != radius:
					continue
				var candidate := cell + Vector2i(dx, dy)
				if not _is_walkable(candidate, mode):
					continue
				var distance := float(dx * dx + dy * dy)
				if distance < best_distance:
					best = candidate
					best_distance = distance
		if best.x >= 0:
			return best
	return best


func _is_walkable(cell: Vector2i, mode: String) -> bool:
	if _nav_image == null:
		return true
	if cell.x < 0 or cell.y < 0:
		return false
	if cell.x >= _nav_image.get_width() or cell.y >= _nav_image.get_height():
		return false

	var land: float = _nav_image.get_pixel(cell.x, cell.y).r
	if mode == "ship":
		return land <= COAST_THRESHOLD
	return land >= 1.0 - COAST_THRESHOLD


func _to_cell(point: Vector2) -> Vector2i:
	return Vector2i(int(point.x / NAV_CELL_PX), int(point.y / NAV_CELL_PX))


func _cell_to_pixel(cell: Vector2i) -> Vector2:
	return Vector2(
		(float(cell.x) + 0.5) * NAV_CELL_PX,
		(float(cell.y) + 0.5) * NAV_CELL_PX,
	)


func _heuristic(a: Vector2i, b: Vector2i) -> float:
	var dx := float(absi(a.x - b.x))
	var dy := float(absi(a.y - b.y))
	return maxf(dx, dy) + (sqrt(2.0) - 1.0) * minf(dx, dy)


## Bandeirinha da nação de destino presa ao comboio (billboard: sempre virada
## para a câmera). Criada uma vez e mantida entre os trechos.
## Medalhões origem+destino acima do comboio: selo escuro + aro de latão +
## bandeira, lado a lado com um chevron de latão no meio ("de onde → para onde").
func _add_flags(convoy: Node3D, from_nation: String, to_nation: String) -> void:
	if from_nation.is_empty() and to_nation.is_empty():
		return
	var texture := _pair_texture(from_nation, to_nation)
	if texture == null:
		return
	var flags := Sprite3D.new()
	flags.name = "Flags"
	flags.texture = texture
	flags.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	flags.no_depth_test = true
	flags.render_priority = 10
	flags.pixel_size = PAIR_HEIGHT / float(PAIR_TEX_H)
	flags.position = Vector3(0.0, FLAG_HEIGHT, 0.0)
	convoy.add_child(flags)


## Textura do par de medalhões (cacheada por origem|destino).
func _pair_texture(from_nation: String, to_nation: String) -> Texture2D:
	var key := from_nation + "|" + to_nation
	if _pair_cache.has(key):
		return _pair_cache[key]
	var img := Image.create(PAIR_TEX_W, PAIR_TEX_H, false, Image.FORMAT_RGBA8)
	img.fill(Color(0, 0, 0, 0))
	_draw_medallion(img, Vector2(48.0, PAIR_TEX_H * 0.5), from_nation)
	_draw_medallion(img, Vector2(PAIR_TEX_W - 48.0, PAIR_TEX_H * 0.5), to_nation)
	_draw_chevron(img, Vector2(PAIR_TEX_W * 0.5, PAIR_TEX_H * 0.5))
	var tex := ImageTexture.create_from_image(img)
	_pair_cache[key] = tex
	return tex


## Desenha um medalhão: disco escuro + aro de latão + bandeira da nação no centro.
func _draw_medallion(img: Image, center: Vector2, nation: String) -> void:
	var radius := 30.0
	var dark := Color("#363431")
	var brass := Color("#C9A24A")
	for y in range(maxi(0, int(center.y - radius)), mini(img.get_height(), int(center.y + radius) + 1)):
		for x in range(maxi(0, int(center.x - radius)), mini(img.get_width(), int(center.x + radius) + 1)):
			var d := Vector2(x + 0.5, y + 0.5).distance_to(center)
			if d <= radius:
				img.set_pixel(x, y, brass if d > radius - 5.0 else dark)
	if nation.is_empty():
		return
	var flag := Economy.flag_texture(nation)
	if flag == null:
		return
	var fi := flag.get_image()
	if fi == null or fi.get_width() == 0:
		return
	var fw := int(radius * 1.15)
	var fh := int(fw * fi.get_height() / float(fi.get_width()))
	fi = fi.duplicate()
	# blend_rect exige o mesmo formato do destino (RGBA8).
	if fi.get_format() != Image.FORMAT_RGBA8:
		fi.convert(Image.FORMAT_RGBA8)
	fi.resize(fw, fh, Image.INTERPOLATE_LANCZOS)
	img.blend_rect(fi, Rect2i(0, 0, fw, fh),
			Vector2i(int(center.x - fw / 2.0), int(center.y - fh / 2.0)))


## Chevron de latão "›" entre os dois medalhões (indica origem → destino).
func _draw_chevron(img: Image, center: Vector2) -> void:
	var brass := Color("#C9A24A")
	for t in range(0, 9):
		for w in 2:
			var x := int(center.x) - 7 + t + w
			for y in [int(center.y) - t, int(center.y) + t]:
				if x >= 0 and x < img.get_width() and y >= 0 and y < img.get_height():
					img.set_pixel(x, y, brass)


## Posiciona o veículo no início do trecho, orientado para o destino.
func _place_vehicle(convoy: Node3D, mode: String, start: Vector3, end: Vector3) -> void:
	if not is_instance_valid(convoy):
		return

	# Troca só o veículo; a bandeira ("Flag") é mantida. Remove o modelo antigo
	# do comboio IMEDIATAMENTE (remove_child) antes de adicionar o novo: senão
	# dois nós com o mesmo nome coexistiriam no frame e o `get_node` do trecho
	# seguinte erraria o alvo, deixando o modelo antigo (navio) pendurado.
	for child in convoy.get_children():
		if String(child.name) != "Flags":
			convoy.remove_child(child)
			child.queue_free()

	var model := _instantiate_model(mode)
	convoy.add_child(model)
	convoy.set_meta("mode", mode)
	convoy.set_meta("collision_radius", COLLISION_RADIUS_BY_MODE.get(mode, 0.05))
	convoy.position = start + Vector3(0.0, HEIGHT_OFFSET, 0.0)
	_face_target(convoy, start, end)


func _move_vehicle(weight: float, convoy: Node3D, start: Vector3, end: Vector3) -> void:
	if not is_instance_valid(convoy):
		return
	var base := start.lerp(end, weight) + Vector3(0.0, HEIGHT_OFFSET, 0.0)
	# Separação suavizada e minúscula: mantém a linha sem "salto"; e só é
	# aplicada se o veículo continuar no terreno correto.
	var mode := String(convoy.get_meta("mode", "truck"))
	var target := _separation(convoy, base, mode)
	var current: Vector3 = convoy.get_meta("separation", Vector3.ZERO)
	convoy.set_meta("separation", current.lerp(target, SEPARATION_SMOOTHING))
	convoy.position = base + (convoy.get_meta("separation") as Vector3)


## Empurrão minúsculo para separar veículos que praticamente se tocam. Se o
## empurrão levaria o veículo para o terreno errado (caminhão -> água, navio ->
## terra), ele é descartado.
func _separation(convoy: Node3D, base: Vector3, mode: String) -> Vector3:
	var radius := float(convoy.get_meta("collision_radius", 0.05))
	var push := Vector3.ZERO
	for other in _convoys:
		if other == convoy or not is_instance_valid(other):
			continue
		var other_radius := float(other.get_meta("collision_radius", radius))
		var delta := base - other.position
		delta.y = 0.0
		var distance := delta.length()
		var min_distance := radius + other_radius
		if distance < min_distance:
			if distance > 0.001:
				push += delta.normalized() * (min_distance - distance)
			else:
				push += Vector3(0.2, 0.0, 0.0)
	if push.length_squared() < 1e-8:
		return Vector3.ZERO
	var pushed_cell := _to_cell(_world_to_pixel(base + push))
	if not _is_walkable(pushed_cell, mode):
		return Vector3.ZERO
	return push


func _finish_convoy(convoy: Node3D) -> void:
	_active_convoys = maxi(0, _active_convoys - 1)
	_convoys.erase(convoy)
	if is_instance_valid(convoy):
		convoy.queue_free()


## Aponta o comboio para o destino; sem deslocamento, mantém a orientação atual.
func _face_target(convoy: Node3D, start: Vector3, end: Vector3) -> void:
	var direction := end - start
	direction.y = 0.0
	if direction.length_squared() < 1e-8:
		return
	convoy.look_at(convoy.global_position + direction.normalized(), Vector3.UP)


func _instantiate_model(mode: String) -> Node3D:
	var scene: PackedScene = TRUCK_SCENE
	match mode:
		"ship":
			scene = SHIP_SCENE
		"plane":
			scene = PLANE_SCENE

	var model := scene.instantiate() as Node3D
	model.rotation.y = MODEL_YAW_OFFSET
	model.scale = Vector3.ONE * _scale_for(mode, scene)
	return model


## Escala o modelo para que seu maior eixo meça MODEL_LENGTH em unidades.
func _scale_for(mode: String, scene: PackedScene) -> float:
	if _scale_cache.has(mode):
		return _scale_cache[mode]

	var probe := scene.instantiate() as Node3D
	add_child(probe)
	var bounds := _combined_aabb(probe)
	probe.queue_free()

	var longest := maxf(bounds.size.x, maxf(bounds.size.y, bounds.size.z))
	var length: float = float(MODEL_LENGTH_BY_MODE.get(mode, MODEL_LENGTH))
	var scale: float = 1.0 if longest <= 0.0 else length / longest
	_scale_cache[mode] = scale
	return scale


## AABB combinada das malhas do modelo, em espaço local da raiz.
func _combined_aabb(root: Node3D) -> AABB:
	var result := AABB()
	var found := false
	var stack: Array[Node] = [root]
	while not stack.is_empty():
		var node: Node = stack.pop_back()
		if node is MeshInstance3D and node.mesh != null:
			var box: AABB = node.transform * node.get_aabb()
			result = box if not found else result.merge(box)
			found = true
		stack.append_array(node.get_children())
	return result


static func _pixel_to_world(point: Vector2) -> Vector3:
	return Vector3(
		(point.x - IMAGE_HALF_SIZE.x) * PIXEL_TO_WORLD,
		0.0,
		(point.y - IMAGE_HALF_SIZE.y) * PIXEL_TO_WORLD,
	)


## Inverso de _pixel_to_world (mundo -> pixel do mapa), para checar o terreno.
static func _world_to_pixel(position: Vector3) -> Vector2:
	return Vector2(
		position.x / PIXEL_TO_WORLD + IMAGE_HALF_SIZE.x,
		position.z / PIXEL_TO_WORLD + IMAGE_HALF_SIZE.y,
	)
