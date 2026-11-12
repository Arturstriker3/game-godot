extends "res://core/economy/trade_convoys.gd"

## Extensão visual do `TradeConvoys`: separação de caminhões e aviões e
## caminhões 33% menores.
##
## O ciclo de vida (A*, tweens, pool, medalhões) continua no script base; aqui
## só se sobrescreve posicionamento e escala. O lugar natural seria o próprio
## `core/economy/trade_convoys.gd`, mas esse diretório é do Game Systems
## Designer — a extensão mantém o núcleo econômico intocado e o jogo instancia
## este script.
##
## Separação: cada veículo recebe uma faixa fixa (`lane_slot`) ao entrar num
## trecho. Caminhões usam 3 faixas laterais; aviões usam 3 camadas de altitude
## (com um deslocamento lateral leve) porque a envergadura é grande. Além da
## faixa, uma caixa direcional mantém a fila ao longo da rota (dois veículos no
## mesmo corredor não se empilham). O navio preserva o empurrão radial curto
## que já existia.
##
## Custo: consulta a um índice espacial uniforme de 9 células vizinhas; nada de
## varrer todos os pares (o teto de comboios visíveis é `MAX_CONVOYS` = 24).

## Redução pedida: o caminhão fica com 67% do comprimento que tinha.
const TRUCK_SCALE_FACTOR := 0.67

## Faixas por modo e largura (unidades de mundo) entre faixas vizinhas.
const LANES_BY_MODE := {"truck": 3, "plane": 3}
const LANE_WIDTH_BY_MODE := {"truck": 0.16, "plane": 0.22}
## Camadas de altitude do avião (aviões empilhados no mesmo corredor).
const PLANE_ALTITUDE_STEP := 0.07

## Caixa de espaçamento ao longo/lateral da rota, por modo. `along` mantém a
## fila no mesmo corredor; `lateral` decide quando a faixa vizinha já basta.
const ALONG_GAP_BY_MODE := {"truck": 0.52, "plane": 0.62}
const LATERAL_GAP_BY_MODE := {"truck": 0.20, "plane": 0.34}
## Raio de encosto (hitbox) do navio e reserva do caminhão/avião. Também vai
## para o meta `collision_radius`, usado pelos testes e por outros sistemas.
const RADIAL_RADIUS_BY_MODE := {"truck": 0.12, "ship": 0.09, "plane": 0.16}

## Célula do índice espacial: maior que o maior vão consultado (0,62 u), então
## uma vizinhança 3x3 cobre tudo que pode colidir.
const SPACE_CELL := 0.8

var _lane_slot := 0
## Índice espacial dos comboios: célula -> [Node3D] e id -> célula.
var _space: Dictionary = {}
var _space_cell: Dictionary = {}
## Buffer reaproveitado pela consulta (evita alocar a cada frame).
var _near: Array = []


## Caminhão 33% menor. A escala-base já vem cacheada do script base; o fator é
## aplicado por cima para não contaminar o cache de navio/avião.
func _scale_for(mode: String, scene: PackedScene) -> float:
	var base: float = super._scale_for(mode, scene)
	return base * TRUCK_SCALE_FACTOR if mode == "truck" else base


## Entrou num trecho: define a faixa, o raio de encosto e registra no índice.
func _place_vehicle(convoy: Node3D, mode: String, start: Vector3,
		end: Vector3) -> void:
	super._place_vehicle(convoy, mode, start, end)
	if not is_instance_valid(convoy):
		return
	_lane_slot += 1
	convoy.set_meta("lane_slot", _lane_slot)
	convoy.set_meta("collision_radius",
			float(RADIAL_RADIUS_BY_MODE.get(mode, 0.05)))
	# Origem do trecho: a captura usa para achar o navio ainda saindo do porto.
	convoy.set_meta("leg_start", start)
	# Trecho novo começa centrado: sem isso o offset do trecho anterior seria
	# herdado e o veículo nasceria deslocado da rota.
	convoy.set_meta("separation", Vector3.ZERO)
	_space_update(convoy)


## Move com faixa + espaçamento; a base (rota do tween) não muda.
func _move_vehicle(weight: float, convoy: Node3D, start: Vector3,
		end: Vector3) -> void:
	if not is_instance_valid(convoy):
		return
	var base := start.lerp(end, weight) + Vector3(0.0, HEIGHT_OFFSET, 0.0)
	var mode := String(convoy.get_meta("mode", "truck"))
	var direction := end - start
	direction.y = 0.0
	direction = direction.normalized() if direction.length_squared() > 1e-8 \
			else Vector3.FORWARD
	var target := _spacing_target(convoy, base, mode, direction)
	var current: Vector3 = convoy.get_meta("separation", Vector3.ZERO)
	convoy.set_meta("separation", current.lerp(target, SEPARATION_SMOOTHING))
	convoy.position = base + (convoy.get_meta("separation") as Vector3)
	_space_update(convoy)


## Tira o comboio do índice antes de devolvê-lo ao pool. A checagem de geração
## evita que o `finished` de uma viagem antiga remova a viagem nova do mesmo nó.
func _finish_convoy(convoy: Node3D, generation := -1) -> void:
	if is_instance_valid(convoy) and (generation < 0 or int(
			convoy.get_meta("generation", -1)) == generation):
		_space_remove(convoy)
	super._finish_convoy(convoy, generation)


# --- Separação -------------------------------------------------------------

func _spacing_target(convoy: Node3D, base: Vector3, mode: String,
		direction: Vector3) -> Vector3:
	if mode == "ship":
		return _radial_push(convoy, base, mode)
	var lane := _lane_offset(convoy, direction, mode)
	var push := _box_push(convoy, base, mode, direction)
	var target := lane + push
	# A faixa pode cair fora da estrada (costa, viaduto estreito): nesse caso
	# fica só a fila; se nem ela couber no terreno, não desloca nada.
	if mode == "truck" and not _navigable(base + target, mode):
		target = push
		if not _navigable(base + target, mode):
			target = Vector3.ZERO
	return target


## Deslocamento lateral fixo (e altitude, no avião) da faixa do comboio.
func _lane_offset(convoy: Node3D, direction: Vector3, mode: String) -> Vector3:
	var lanes := int(LANES_BY_MODE.get(mode, 1))
	if lanes <= 1:
		return Vector3.ZERO
	var slot := int(convoy.get_meta("lane_slot", 0)) % lanes
	var centered := float(slot) - float(lanes - 1) * 0.5
	var side := Vector3(-direction.z, 0.0, direction.x)
	var offset := side * (centered * float(LANE_WIDTH_BY_MODE.get(mode, 0.0)))
	if mode == "plane":
		offset.y = centered * PLANE_ALTITUDE_STEP
	return offset


## Empurrão em caixa: separa quem divide o mesmo corredor (fila) e quem ficaria
## sobreposto na mesma faixa, sem o empurrão radial que jogaria o caminhão para
## fora da estrada.
func _box_push(convoy: Node3D, base: Vector3, mode: String,
		direction: Vector3) -> Vector3:
	var along := float(ALONG_GAP_BY_MODE.get(mode, 0.0))
	var lateral := float(LATERAL_GAP_BY_MODE.get(mode, 0.0))
	if along <= 0.0 or lateral <= 0.0:
		return Vector3.ZERO
	var side := Vector3(-direction.z, 0.0, direction.x)
	var push := Vector3.ZERO
	for other: Node3D in _space_near(base):
		if other == convoy or not is_instance_valid(other):
			continue
		if String(other.get_meta("mode", "")) != mode:
			continue
		var delta := base - other.position
		delta.y = 0.0
		var d_along := delta.dot(direction)
		var d_side := delta.dot(side)
		var overlap_along := along - absf(d_along)
		var overlap_side := lateral - absf(d_side)
		if overlap_along <= 0.0 or overlap_side <= 0.0:
			continue
		# Mesma faixa (quase sem desvio lateral): resolve pela fila, que é o
		# que o jogador lê numa estrada. Fora dela, sai pelo eixo de menor
		# penetração. Empate exato (dois veículos nascendo no mesmo ponto) usa
		# o id como desempate: cada um recebe um sinal oposto, senão os dois
		# andariam juntos e continuariam empilhados.
		var same_lane := absf(d_side) < float(
				LANE_WIDTH_BY_MODE.get(mode, 0.16)) * 0.5
		if same_lane or overlap_along <= overlap_side:
			var sign_along := signf(d_along)
			if sign_along == 0.0:
				sign_along = 1.0 if int(convoy.get_instance_id()) \
						> int(other.get_instance_id()) else -1.0
			push += direction * (sign_along * overlap_along)
		else:
			var sign_side := signf(d_side)
			if sign_side == 0.0:
				sign_side = 1.0 if int(convoy.get_instance_id()) \
						> int(other.get_instance_id()) else -1.0
			push += side * (sign_side * overlap_side)
	return push


## Empurrão radial curto do navio (comportamento anterior, agora só entre as
## células vizinhas do índice espacial).
func _radial_push(convoy: Node3D, base: Vector3, mode: String) -> Vector3:
	var radius := float(convoy.get_meta("collision_radius",
			float(RADIAL_RADIUS_BY_MODE.get(mode, 0.05))))
	var push := Vector3.ZERO
	for other: Node3D in _space_near(base):
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
	if not _navigable(base + push, mode):
		return Vector3.ZERO
	return push


func _navigable(position: Vector3, mode: String) -> bool:
	if _nav == null:
		return false
	return _nav.walkable(_to_cell(_world_to_pixel(position)), mode)


# --- Índice espacial -------------------------------------------------------

func _space_key(position: Vector3) -> Vector2i:
	return Vector2i(floori(position.x / SPACE_CELL), floori(position.z / SPACE_CELL))


func _space_update(convoy: Node3D) -> void:
	var id := int(convoy.get_instance_id())
	var key := _space_key(convoy.position)
	if _space_cell.has(id) and _space_cell[id] == key:
		return
	_space_remove(convoy)
	var bucket: Array = _space.get(key, [])
	bucket.append(convoy)
	_space[key] = bucket
	_space_cell[id] = key


func _space_remove(convoy: Node3D) -> void:
	if not is_instance_valid(convoy):
		return
	var id := int(convoy.get_instance_id())
	if not _space_cell.has(id):
		return
	var key: Vector2i = _space_cell[id]
	_space_cell.erase(id)
	var bucket: Array = _space.get(key, [])
	bucket.erase(convoy)
	if bucket.is_empty():
		_space.erase(key)


## Comboios nas 9 células ao redor de `position` (buffer reaproveitado).
func _space_near(position: Vector3) -> Array:
	var key := _space_key(position)
	_near.clear()
	for dz in range(-1, 2):
		for dx in range(-1, 2):
			var bucket: Variant = _space.get(key + Vector2i(dx, dz), null)
			if bucket != null:
				_near.append_array(bucket as Array)
	return _near
