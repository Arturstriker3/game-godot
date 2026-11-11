extends Node3D

## Anima os pedidos de comércio no mapa: o veículo de cada trecho (caminhão,
## navio ou avião) sai do ponto de origem e segue apontado para o destino.
##
## Caminhões andam só por terra e navios só por água, usando a máscara de
## navegação gerada a partir das províncias (map_data/nav_mask.png).
##
## Desempenho: nada de trabalho pesado na virada do dia. `spawn_orders()` só
## enfileira os pedidos; a busca de rota (A* sobre a máscara) e a criação dos
## veículos acontecem sob demanda, em `_process()`, com orçamento fixo por
## frame (FRAME_BUDGET_US). A busca é interrompível (retoma no frame seguinte)
## e os nós dos veículos são reciclados em pool — sem instanciar/liberar nós a
## cada pedido.

# Mesma conversão usada pelos labels 3D do mapa (pixel -> mundo).
const PIXEL_TO_WORLD := 0.2
const IMAGE_HALF_SIZE := Vector2(2500.0, 1250.0)

const TRUCK_SCENE: PackedScene = preload("res://assets/models/truck.glb")
const SHIP_SCENE: PackedScene = preload("res://assets/models/ship.glb")
const PLANE_SCENE: PackedScene = preload("res://assets/models/plane.glb")

# Tamanho alvo (unidades de mundo) do maior eixo do modelo. Caminhão e navio
# têm o mesmo comprimento: o caminhão precisa ser lido no zoom próximo (antes,
# 0,275 u, ficava menor que o medalhão e sumia atrás dele).
const MODEL_LENGTH := 0.275
const MODEL_LENGTH_BY_MODE := {"truck": 0.6875, "ship": 0.6875, "plane": 0.55}
# Altura acima do plano do mapa para o modelo não afundar no terreno.
const HEIGHT_OFFSET := 0.18
# Bandeirinha da nação de destino, acima do veículo (identifica o comboio sem
# encobri-lo: o caminhão agora tem ~0,24 u de altura).
const FLAG_HEIGHT := 0.56
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
# Raio (em células finas) do anel de snap. Cobre o caso normal — cidade
# costeira, porto ao lado — em ~1 ms; a varredura de 128 anéis custava ~176 ms.
const SNAP_NEAR_CELLS := 12
# Anéis (em células grosseiras de 16 px) da busca do componente principal
# quando não há nada navegável no raio curto.
const SNAP_COARSE_RINGS := 8
# Nós visitados pelo A* antes de desistir. Rotas oceânicas longas (Europa ->
# América do Sul) exploram um corredor de milhares de células; como a rota é
# puramente visual, um teto baixo basta: rotas reais ficam na casa dos milhares
# de nós, e rotas impossíveis (continentes diferentes) são descartadas antes de
# começar pela checagem de componente grosseiro.
const MAX_ASTAR_NODES := 24000
## Teto da busca grosseira de fallback. Quando o A* fino esgota o teto por
## explorar um oceano aberto (heurística fraca diante de um desvio longo), a
## mesma área na grade de 16 px custa 1/256 dos nós: 12 mil cobrem o oceano
## inteiro e o caminho é refinado depois em células finas.
const MAX_COARSE_NODES := 12000
## Células grosseiras refinadas por fatia (cada uma vira ~16 amostras finas
## validadas no terreno), para o refinamento não estourar o frame.
const REFINE_CHUNK := 32
## Nós grosseiros por fatia. Cada nó grosseiro testa a linha fina entre dois
## centros (16 amostras por vizinho), então custa ~16x um nó fino: 2 nós por
## leitura do relógio mantêm a fatia e o pico de frame iguais aos do A* fino.
const COARSE_CHUNK_NODES := 2
## Teto do A* fino do último trecho (depois do caminho grosseiro): o resto do
## percurso cabe em 2 células grosseiras, mas o canal pode ser estreito e
## sinuoso (enseada de Bristol), então o teto é folgado.
const MAX_TAIL_NODES := 4000
# Salto de olhar máximo ao simplificar o caminho (pontos por waypoint). Limita o
# custo a O(n) em vez de O(n²), para rotas oceânicas longas não travarem o jogo.
const SIMPLIFY_LOOKAHEAD := 48
# Heurística do A* levemente inflada (weighted A*): a rota fica até ~5% mais
# longa que a ótima, mas o número de nós explorados cai ~10x. A rota é visual:
# o ganho de frame vale muito mais do que 5% de caminho.
const ASTAR_HEURISTIC_WEIGHT := 1.05
# Orçamento de trabalho por frame (microssegundos). O pump da fila termina
# quando estoura o prazo e retoma no frame seguinte — a virada do dia nunca
# bloqueia o frame.
const FRAME_BUDGET_US := 1500
# Nós de A* entre duas leituras do relógio: granularidade do fatiamento. A
# fatia custa ~30 µs por nó (8 vizinhos, cada um testando o terreno); 8 nós
# deixam o estouro do orçamento abaixo de ~0,3 ms por frame.
const ASTAR_CHUNK_NODES := 8
# Células finas por célula da grade grosseira usada no teste de componente
# (uma rota entre continentes diferentes é descartada na hora).
const COARSE_CELL := 16

const NEIGHBORS: Array[Vector2i] = [
	Vector2i(1, 0), Vector2i(-1, 0), Vector2i(0, 1), Vector2i(0, -1),
	Vector2i(1, 1), Vector2i(1, -1), Vector2i(-1, 1), Vector2i(-1, -1),
]
# Mesma vizinhança em inteiros, para o laço quente do A* (k >= 4 = diagonal).
const NEIGHBOR_DX: Array[int] = [1, -1, 0, 0, 1, 1, -1, -1]
const NEIGHBOR_DY: Array[int] = [0, 0, 1, -1, 1, -1, 1, -1]
# Limites de terreno no byte da máscara, equivalentes a COAST_THRESHOLD (0,01)
# sobre 0..255: água é 0..2, terra é 253..255.
const MASK_WATER_MAX := 2
const MASK_LAND_MIN := 253

var _active_convoys := 0
var _depart_slot := 0
var _convoys: Array[Node3D] = []
var _scale_cache := {}
var _pair_cache := {}
var _path_cache := {}
var _snap_cache := {}
## Fila de pedidos do dia e o pedido em preparação (rotas sendo calculadas).
var _queue: Array = []
var _prep := {}
## Pool de veículos por modo e de nós de comboio (reciclagem, sem free).
var _pool_root: Node3D
var _vehicle_pool := {}
var _convoy_pool: Array[Node3D] = []
var _nav: NavGrid
var _disc_image: Image

## Tweens em andamento, para acompanhar a velocidade/pausa da simulação.
var _tweens: Array[Tween] = []
## Tween vivo de cada comboio (por instance id). Ao reciclar o nó, o tween
## antigo é morto: sem isso um callback atrasado move/encerra a viagem nova.
var _tween_by_convoy := {}
var _generation := 0
var _time_scale := 1.0
var _paused := false

## Contadores do ciclo de vida, lidos pelo harness `convoy_lifecycle_test`:
## `spawned` = pedidos que chegaram a `_spawn_convoy` (tentativas), `finished` =
## comboios que completaram o percurso, `cancelled` = tentativas recusadas por
## `no_legs` (pedido sem trechos), `no_route` (nenhum trecho com caminho
## desenhável) ou `no_slot` (teto de comboios ativos), `placed` = comboios que
## nasceram já com o veículo no ponto de partida. No fim de uma rodada limpa
## valem `spawned == finished + cancelled` e `placed == spawned - cancelled`.
var stats := {
	"spawned": 0,
	"finished": 0,
	"cancelled": 0,
	"no_legs": 0,
	"no_route": 0,
	"no_slot": 0,
	"placed": 0,
	"last_cancel": "",
}


## Grade de navegação: máscara em memória, teste de terreno O(1), componentes
## grosseiros (para descartar rotas impossíveis) e a busca A* interrompível.
class NavGrid:
	var w := 0
	var h := 0
	var data := PackedByteArray()
	var cw := 0
	var ch := 0
	var land_component := PackedInt32Array()
	var water_component := PackedInt32Array()
	var main_land := -1
	var main_water := -1

	# Estado da busca corrente (uma por vez: o pump é sequencial).
	var start := Vector2i(-1, -1)
	var goal := Vector2i(-1, -1)
	var mode := "truck"
	var active := false
	var phase := 0  # 0 = busca, 1 = simplificação, 2 = pronta, 3 = sem rota
	var raw := PackedVector2Array()
	var out := PackedVector2Array()
	var _i := 2
	var _anchor := 0
	var visited := 0
	var _hp := PackedFloat32Array()
	var _hc := PackedInt32Array()
	var _g := PackedFloat32Array()
	var _gen := PackedInt32Array()
	var _parent := PackedInt32Array()
	## Células já expandidas nesta busca. Sem isso a heurística inflada (peso
	## > 1) reabre a mesma célula dezenas de vezes em ziguezague diagonal: o
	## teto de nós contava pops duplicados, não nós trabalhados (medido:
	## 24.001 pops para 1.104 células em Calais->Belfast).
	var _closed := PackedInt32Array()
	var _gen_id := 0

	## Fallback grosseiro (16 px por célula): estado próprio da busca e do
	## refinamento do caminho em células finas.
	var coarse := false
	var used_coarse := false
	var _cg := PackedFloat32Array()
	var _cgen := PackedInt32Array()
	var _cclosed := PackedInt32Array()
	var _cparent := PackedInt32Array()
	var _cgen_id := 0
	var _cstart := -1
	var _cgoal := -1
	var _cpath: Array[Vector2i] = []
	var _refine_index := 0
	var _refine_cell := Vector2i(-1, -1)
	## Último trecho em A* fino depois do caminho grosseiro.
	var tail := false
	var node_limit := MAX_ASTAR_NODES
	var _tail_merged := false
	var _tail_from := Vector2i(-1, -1)
	var _tail_to := Vector2i(-1, -1)
	var _prefix := PackedVector2Array()
	## Cortar canto diagonal é proibido no A* normal. No trecho final o porto
	## pode estar ligado ao mar só por diagonais (cais de 1 célula): ali o
	## canto é liberado, senão o último pixel nunca fecha.
	var strict_corners := true

	func build(image: Image) -> void:
		w = image.get_width()
		h = image.get_height()
		data = image.get_data()
		cw = int(ceil(float(w) / float(COARSE_CELL)))
		ch = int(ceil(float(h) / float(COARSE_CELL)))
		var nx := cw
		var ny := ch
		land_component.resize(nx * ny)
		land_component.fill(-1)
		water_component.resize(nx * ny)
		water_component.fill(-1)
		_cg.resize(nx * ny)
		_cgen.resize(nx * ny)
		_cclosed.resize(nx * ny)
		_cparent.resize(nx * ny)
		_g.resize(w * h)
		_gen.resize(w * h)
		_parent.resize(w * h)
		_closed.resize(w * h)
		var land_sizes := _label(land_component, true)
		var water_sizes := _label(water_component, false)
		main_land = _largest(land_sizes)
		main_water = _largest(water_sizes)

	## Rotula os componentes navegáveis da grade grosseira (8-vizinhos) e devolve
	## o tamanho de cada componente.
	func _label(components: PackedInt32Array, land: bool) -> Array:
		var sizes: Array = []
		var queue := PackedInt32Array()
		for cy in range(ch):
			for cx in range(cw):
				var index := cy * cw + cx
				if components[index] != -1:
					continue
				var sample := Vector2i(cx, cy)
				if not _coarse_walkable(sample, land):
					continue
				var id := sizes.size()
				sizes.append(0)
				components[index] = id
				queue.clear()
				queue.append(index)
				var head := 0
				while head < queue.size():
					var current: int = queue[head]
					head += 1
					sizes[id] = int(sizes[id]) + 1
					var x: int = current % cw
					var y: int = current / cw
					for direction in NEIGHBORS:
						var nxx: int = x + direction.x
						var nyy: int = y + direction.y
						if nxx < 0 or nyy < 0 or nxx >= cw or nyy >= ch:
							continue
						var next: int = nyy * cw + nxx
						if components[next] != -1:
							continue
						if not _coarse_walkable(Vector2i(nxx, nyy), land):
							continue
						components[next] = id
						queue.append(next)
		return sizes

	func _largest(sizes: Array) -> int:
		var best := -1
		var best_size := 0
		for id in range(sizes.size()):
			if int(sizes[id]) > best_size:
				best_size = int(sizes[id])
				best = id
		return best

	func _coarse_walkable(coarse: Vector2i, land: bool) -> bool:
		var x: int = mini(coarse.x * COARSE_CELL + COARSE_CELL / 2, w - 1)
		var y: int = mini(coarse.y * COARSE_CELL + COARSE_CELL / 2, h - 1)
		return walkable(Vector2i(x, y), "truck" if land else "ship")

	func walkable(cell: Vector2i, cell_mode: String) -> bool:
		if cell.x < 0 or cell.y < 0 or cell.x >= w or cell.y >= h:
			return false
		var value: int = data[(cell.y * w + cell.x) * 4]
		if cell_mode == "ship":
			return value <= MASK_WATER_MAX
		return value >= MASK_LAND_MIN

	## Componente grosseiro (continente/oceano) da célula fina. A célula fina é
	## lida pelo centro da célula grossa; numa faixa costeira esse centro pode
	## cair no terreno oposto e a célula grossa ficar sem rótulo para o modo.
	## Nesse caso a vizinhança 3x3 é consultada — sem isso um ponto de água
	## legítimo na costa seria rejeitado e o snap sairia procurando outro mar.
	func component(cell: Vector2i, cell_mode: String) -> int:
		if cell.x < 0 or cell.y < 0 or cell.x >= w or cell.y >= h:
			return -1
		var table: PackedInt32Array = water_component if cell_mode == "ship" \
				else land_component
		var cx: int = cell.x / COARSE_CELL
		var cy: int = cell.y / COARSE_CELL
		var own: int = table[cy * cw + cx]
		if own >= 0:
			return own
		for dy in range(-1, 2):
			for dx in range(-1, 2):
				if dx == 0 and dy == 0:
					continue
				var nx: int = cx + dx
				var ny: int = cy + dy
				if nx < 0 or ny < 0 or nx >= cw or ny >= ch:
					continue
				var near: int = table[ny * cw + nx]
				if near >= 0:
					return near
		return -1

	func main_component(cell_mode: String) -> int:
		return main_water if cell_mode == "ship" else main_land

	## Célula navegável mais próxima. Prefere o componente principal (o oceano /
	## o continente): a água mais próxima de uma cidade costeira pode ser uma
	## lagoa isolada, e uma rota até ela nunca fecha.
	##
	## Duas fases, porque a varredura completa custa caro: um anel de 128 células
	## são ~65 mil testes de terreno (~176 ms medidos) e acontecia em toda cidade
	## cujo terreno vizinho não fosse do componente principal.
	##   1. anel fino curto (SNAP_NEAR_CELLS): resolve o caso normal;
	##   2. só quando não há nada navegável por perto, anda pela grade grosseira
	##      até achar o componente principal e afina dentro da célula.
	## Sem célula do componente principal, cai para a mais próxima de qualquer
	## componente (comportamento antigo).
	func snap(cell: Vector2i, cell_mode: String) -> Vector2i:
		if walkable(cell, cell_mode):
			var own := component(cell, cell_mode)
			if own == main_component(cell_mode):
				return cell
		var main := main_component(cell_mode)
		var fallback := Vector2i(-1, -1)
		for radius in range(1, SNAP_NEAR_CELLS + 1):
			var best := Vector2i(-1, -1)
			var best_distance := INF
			for dy in range(-radius, radius + 1):
				for dx in range(-radius, radius + 1):
					if maxi(absi(dx), absi(dy)) != radius:
						continue
					var candidate := cell + Vector2i(dx, dy)
					if not walkable(candidate, cell_mode):
						continue
					if fallback.x < 0:
						fallback = candidate
					if component(candidate, cell_mode) != main:
						continue
					var distance := float(dx * dx + dy * dy)
					if distance < best_distance:
						best = candidate
						best_distance = distance
			if best.x >= 0:
				return best
		if fallback.x < 0:
			var coarse := _coarse_snap(cell, cell_mode)
			if coarse.x >= 0:
				return coarse
		return fallback

	## Célula fina do componente principal mais próxima, procurada na grade
	## grosseira (16 px por célula) e afinada dentro da célula encontrada.
	func _coarse_snap(cell: Vector2i, cell_mode: String) -> Vector2i:
		var table: PackedInt32Array = water_component if cell_mode == "ship" \
				else land_component
		var main := main_component(cell_mode)
		var origin := Vector2i(cell.x / COARSE_CELL, cell.y / COARSE_CELL)
		for ring in range(0, SNAP_COARSE_RINGS + 1):
			var best := Vector2i(-1, -1)
			var best_distance := INF
			for dy in range(-ring, ring + 1):
				for dx in range(-ring, ring + 1):
					if maxi(absi(dx), absi(dy)) != ring:
						continue
					var cx: int = origin.x + dx
					var cy: int = origin.y + dy
					if cx < 0 or cy < 0 or cx >= cw or cy >= ch:
						continue
					if table[cy * cw + cx] != main:
						continue
					var distance := float(dx * dx + dy * dy)
					if distance < best_distance:
						best = Vector2i(cx, cy)
						best_distance = distance
			if best.x < 0:
				continue
			var found := _refine_component(best, cell, cell_mode)
			if found.x >= 0:
				return found
		return Vector2i(-1, -1)

	## Célula fina navegável do componente principal, a mais próxima do alvo,
	## dentro de uma célula grossa.
	func _refine_component(coarse: Vector2i, target: Vector2i,
			cell_mode: String) -> Vector2i:
		var main := main_component(cell_mode)
		var best := Vector2i(-1, -1)
		var best_distance := INF
		var x0: int = coarse.x * COARSE_CELL
		var y0: int = coarse.y * COARSE_CELL
		for y in range(y0, mini(y0 + COARSE_CELL, h)):
			for x in range(x0, mini(x0 + COARSE_CELL, w)):
				var candidate := Vector2i(x, y)
				if not walkable(candidate, cell_mode):
					continue
				if component(candidate, cell_mode) != main:
					continue
				var dx := x - target.x
				var dy := y - target.y
				var distance := float(dx * dx + dy * dy)
				if distance < best_distance:
					best = candidate
					best_distance = distance
		return best

	## Prepara uma busca. Devolve false quando a rota é impossível de antemão
	## (extremos em componentes grosseiros diferentes).
	func begin(from: Vector2i, to: Vector2i, cell_mode: String) -> bool:
		start = from
		goal = to
		mode = cell_mode
		active = true
		phase = 0
		visited = 0
		raw = PackedVector2Array()
		out = PackedVector2Array()
		_i = 2
		_anchor = 0
		coarse = false
		used_coarse = false
		tail = false
		strict_corners = true
		node_limit = MAX_ASTAR_NODES
		_tail_merged = false
		_tail_from = Vector2i(-1, -1)
		_tail_to = Vector2i(-1, -1)
		_prefix = PackedVector2Array()
		_cpath.clear()
		_refine_index = 0
		_refine_cell = Vector2i(-1, -1)
		if from.x < 0 or to.x < 0:
			phase = 3
			active = false
			return false
		var n := w * h
		if _g.size() != n:
			_g.resize(n)
			_gen.resize(n)
			_parent.resize(n)
			_closed.resize(n)
		# Marcação por geração: nenhum vetor de 3,1 M células é limpo entre as
		# buscas (limpar custaria ~3 ms por trecho).
		_gen_id += 1
		if _gen_id >= 2147483000:
			_gen.fill(0)
			_gen_id = 1
		_hp.clear()
		_hc.clear()
		var start_index: int = from.y * w + from.x
		_gen[start_index] = _gen_id
		_g[start_index] = 0.0
		_heap_push(_heuristic(from, to) * ASTAR_HEURISTIC_WEIGHT, start_index)
		# Extremos em continentes/oceanos diferentes: nenhuma rota existe.
		if component(from, cell_mode) != component(to, cell_mode):
			phase = 3
			active = false
			return false
		return true

	## Avança a busca dentro do prazo. 0 = ainda rodando, 1 = rota pronta,
	## 2 = sem rota.
	func step(deadline_usec: int) -> int:
		if not active:
			return 1 if phase == 2 else 2
		if phase == 0:
			if coarse:
				return _coarse_step(deadline_usec)
			var start_index: int = start.y * w + start.x
			var goal_index: int = goal.y * w + goal.x
			var water_mode: bool = mode == "ship"
			var budget := 0
			var pops := 0
			while not _hp.is_empty():
				var current := _heap_pop()
				pops += 1
				if _closed[current] == _gen_id:
					# Duplicado de uma célula já expandida (a heurística inflada
					# não é consistente): descarta e segue, sem gastar orçamento.
					if pops % 64 == 0 and Time.get_ticks_usec() >= deadline_usec:
						return 0
					continue
				_closed[current] = _gen_id
				visited += 1
				budget += 1
				if current == goal_index:
					_build_raw(start_index, goal_index)
					phase = 1
					break
				if budget >= ASTAR_CHUNK_NODES:
					budget = 0
					if Time.get_ticks_usec() >= deadline_usec:
						return 0
				if visited > node_limit:
					# A rota é visual e longa: em vez de desistir, repete a
					# busca na grade grossa de 16 px e refina o caminho. Se já
					# estamos no trecho final, desiste de verdade.
					if tail or not _begin_coarse():
						_fail()
						return 2
					return 0
				var cx: int = current % w
				var cy: int = current / w
				var g_current: float = _g[current]
				for k in 8:
					var nx: int = cx + NEIGHBOR_DX[k]
					var ny: int = cy + NEIGHBOR_DY[k]
					if nx < 0 or ny < 0 or nx >= w or ny >= h:
						continue
					var next: int = ny * w + nx
					var value: int = data[next * 4]
					var passable: bool
					if water_mode:
						passable = value <= MASK_WATER_MAX
					else:
						passable = value >= MASK_LAND_MIN
					if not passable:
						continue
					var diagonal: bool = k >= 4
					if diagonal and strict_corners:
						# Sem cortar o canto: os dois lados precisam ser do mesmo terreno.
						var side_x: int = data[(cy * w + nx) * 4]
						var side_y: int = data[(ny * w + cx) * 4]
						if water_mode:
							if side_x > MASK_WATER_MAX or side_y > MASK_WATER_MAX:
								continue
						elif side_x < MASK_LAND_MIN or side_y < MASK_LAND_MIN:
							continue
					var tentative: float = g_current + (1.41421356 if diagonal else 1.0)
					# O nó pode já estar aberto: só reabre com custo menor. A lista
					# fechada ficou de fora de propósito — reexpandir um duplicado do
					# heap é raro e mais barato que manter um dicionário por nó.
					if _gen[next] == _gen_id and tentative >= _g[next]:
						continue
					_gen[next] = _gen_id
					_g[next] = tentative
					_parent[next] = current
					_heap_push(tentative
							+ _heuristic_xy(nx, ny, goal.x, goal.y) * ASTAR_HEURISTIC_WEIGHT,
							next)
			if _hp.is_empty() and phase == 0:
				_fail()
				return 2
		if tail and phase == 1 and not _tail_merged:
			# Junta o prefixo grosseiro (já refinado) com o trecho fino final,
			# que foi buscado do destino para o prefixo: entra invertido.
			var tail_raw := raw
			raw = _prefix
			for i in range(tail_raw.size() - 1, -1, -1):
				var point := tail_raw[i]
				if raw.is_empty() or raw[raw.size() - 1].distance_to(point) > 0.01:
					raw.append(point)
			_tail_merged = true
			_i = 2
			_anchor = 0
			out = PackedVector2Array()
		if phase == 4:
			# Caminho grosseiro pronto: refina em células finas por fatias e só
			# então entra na simplificação normal (fase 1).
			if not _refine_step(deadline_usec):
				return 0
			_prefix = raw
			# Último trecho em A* fino: vai do destino para o fim do prefixo
			# (a enseada é pequena; buscar a partir dela é bem mais barato) e o
			# resultado é invertido na junção.
			if _refine_cell != goal and _begin_tail(goal, _refine_cell):
				return 0
			phase = 1
			_i = 2
			_anchor = 0
			out = PackedVector2Array()
		if phase == 1:
			if raw.size() <= 2:
				out = raw
				phase = 2
				active = false
				return 1
			if out.is_empty():
				out.append(raw[0])
			var done := 0
			while _i < raw.size():
				if _i - _anchor > SIMPLIFY_LOOKAHEAD \
						or not _line_walkable(raw[_anchor], raw[_i], mode):
					out.append(raw[_i - 1])
					_anchor = _i - 1
				_i += 1
				done += 1
				if done % 8 == 0 and Time.get_ticks_usec() >= deadline_usec:
					return 0
			out.append(raw[raw.size() - 1])
			phase = 2
			active = false
			return 1
		return 1 if phase == 2 else 2

	func _fail() -> void:
		phase = 3
		active = false
		out = PackedVector2Array()

	## Célula grosseira navegável que representa um ponto fino. Prefere uma cujo
	## centro é alcançável pela linha fina (o caminho grosseiro começa/termina
	## com uma amostra válida); se nenhuma, aceita a mais próxima só navegável.
	func _coarse_cell_for(fine: Vector2i, land: bool) -> Vector2i:
		var origin := Vector2i(fine.x / COARSE_CELL, fine.y / COARSE_CELL)
		var from_point := _cell_center(fine)
		var linked := Vector2i(-1, -1)
		var linked_distance := INF
		var nearest := Vector2i(-1, -1)
		var nearest_distance := INF
		for dy in range(-1, 2):
			for dx in range(-1, 2):
				var candidate := origin + Vector2i(dx, dy)
				if not _coarse_walkable(candidate, land):
					continue
				var distance := float(dx * dx + dy * dy)
				if distance < nearest_distance:
					nearest = candidate
					nearest_distance = distance
				if distance < linked_distance \
						and _line_walkable(from_point, _coarse_center(candidate), mode):
					linked = candidate
					linked_distance = distance
		if linked.x >= 0:
			return linked
		return nearest

	## Prepara a busca grosseira de fallback. Devolve false quando nem ela pode
	## começar (extremos sem célula grossa navegável ou em componentes
	## diferentes) — aí a rota não existe mesmo.
	func _begin_coarse() -> bool:
		var land: bool = mode != "ship"
		var a := _coarse_cell_for(start, land)
		var b := _coarse_cell_for(goal, land)
		if a.x < 0 or b.x < 0:
			return false
		_cstart = a.y * cw + a.x
		_cgoal = b.y * cw + b.x
		var table: PackedInt32Array = water_component if not land else land_component
		if table[_cstart] != table[_cgoal]:
			return false
		coarse = true
		used_coarse = true
		phase = 0
		active = true
		visited = 0
		_cgen_id += 1
		if _cgen_id >= 2147483000:
			_cgen.fill(0)
			_cclosed.fill(0)
			_cgen_id = 1
		# O heap é o mesmo da busca fina (a busca fina acabou de desistir).
		_hp.clear()
		_hc.clear()
		_cgen[_cstart] = _cgen_id
		_cg[_cstart] = 0.0
		_heap_push(_heuristic_xy(a.x, a.y, b.x, b.y) * ASTAR_HEURISTIC_WEIGHT, _cstart)
		_cpath.clear()
		return true

	## A* de 8 vizinhos sobre a grade grosseira, na mesma fatia de orçamento.
	## O movimento só vale quando a linha fina entre os dois centros é
	## navegável: sem isso uma célula grossa "de água pelo centro" deixaria o
	## navio cortar uma faixa de terra de 16 px (Brittany, ilhas do Canal).
	func _coarse_step(deadline_usec: int) -> int:
		var land: bool = mode != "ship"
		var budget := 0
		var pops := 0
		while not _hp.is_empty():
			var current := _heap_pop()
			pops += 1
			if _cclosed[current] == _cgen_id:
				if pops % 64 == 0 and Time.get_ticks_usec() >= deadline_usec:
					return 0
				continue
			_cclosed[current] = _cgen_id
			visited += 1
			budget += 1
			# Aceita a até duas células grosseiras do destino. O canal final pode
			# ser mais estreito que a célula de 16 px (enseada de Bristol): aqui
			# ele não é atravessado, e sim resolvido pelo A* fino do último
			# trecho (fase 4 -> cauda).
			var cell := Vector2i(current % cw, current / cw)
			if maxi(absi(cell.x - _cgoal % cw), absi(cell.y - _cgoal / cw)) <= 2:
				_build_coarse_path(current)
				phase = 4
				raw = PackedVector2Array()
				_refine_index = 0
				_refine_cell = start
				return 0
			if budget >= COARSE_CHUNK_NODES:
				budget = 0
				if Time.get_ticks_usec() >= deadline_usec:
					return 0
			if visited > MAX_COARSE_NODES:
				_fail()
				return 2
			var cx: int = current % cw
			var cy: int = current / cw
			var g_current: float = _cg[current]
			var from_point := _coarse_center(Vector2i(cx, cy))
			for k in 8:
				var nx: int = cx + NEIGHBOR_DX[k]
				var ny: int = cy + NEIGHBOR_DY[k]
				if nx < 0 or ny < 0 or nx >= cw or ny >= ch:
					continue
				if not _coarse_walkable(Vector2i(nx, ny), land):
					continue
				if not _line_walkable(from_point, _coarse_center(Vector2i(nx, ny)),
						mode):
					continue
				var next: int = ny * cw + nx
				var tentative: float = g_current + (1.41421356 if k >= 4 else 1.0)
				if _cgen[next] == _cgen_id and tentative >= _cg[next]:
					continue
				_cgen[next] = _cgen_id
				_cg[next] = tentative
				_cparent[next] = current
				_heap_push(tentative
						+ _heuristic_xy(nx, ny, _cgoal % cw, _cgoal / cw)
						* ASTAR_HEURISTIC_WEIGHT,
						next)
		if _hp.is_empty() and phase == 0:
			_fail()
			return 2
		return 0

	static func _coarse_center(coarse: Vector2i) -> Vector2:
		return Vector2((float(coarse.x * COARSE_CELL) + COARSE_CELL * 0.5) * NAV_CELL_PX,
				(float(coarse.y * COARSE_CELL) + COARSE_CELL * 0.5) * NAV_CELL_PX)

	## Centros das células grosseiras do caminho, como células finas.
	func _build_coarse_path(target_index: int) -> void:
		_cpath.clear()
		var walk := target_index
		var guard := 0
		while walk != _cstart and guard < cw * ch:
			guard += 1
			var cx: int = walk % cw
			var cy: int = walk / cw
			_cpath.append(Vector2i(cx * COARSE_CELL + COARSE_CELL / 2,
					cy * COARSE_CELL + COARSE_CELL / 2))
			if _cgen[walk] != _cgen_id:
				break
			walk = _cparent[walk]
		_cpath.reverse()

	## Refina o caminho grosseiro em células finas, em fatias. Cada amostra é
	## validada no terreno fino: a célula grosseira é "água" pelo pixel central
	## e uma barreira fina dentro dela ainda precisa ser respeitada.
	func _refine_step(deadline_usec: int) -> bool:
		var want := component(start, mode)
		if raw.is_empty():
			_refine_cell = start
			raw.append(_cell_center(start))
		var budget := 0
		while _refine_index < _cpath.size():
			var cell: Vector2i = _cpath[_refine_index]
			var placed := _snap_near(cell, _refine_cell, mode, want, 4)
			if placed.x >= 0:
				_fill_between(raw[raw.size() - 1], _cell_center(placed), mode, want)
				_refine_cell = placed
			_refine_index += 1
			budget += 1
			if budget >= REFINE_CHUNK and Time.get_ticks_usec() >= deadline_usec:
				return false
		return true

	## Amostras finas a cada NAV_CELL_PX entre dois pontos, cada uma encaixada
	## no terreno navegável do mesmo componente (o traço grosseiro pode cortar
	## a ponta de uma costa).
	func _fill_between(a: Vector2, b: Vector2, cell_mode: String, want: int) -> void:
		var steps: int = int(ceil(a.distance_to(b) / NAV_CELL_PX))
		if steps < 1:
			return
		var last := Vector2i(int(a.x / NAV_CELL_PX), int(a.y / NAV_CELL_PX))
		for i in range(1, steps + 1):
			var point := a.lerp(b, float(i) / float(steps))
			var cell := Vector2i(int(point.x / NAV_CELL_PX), int(point.y / NAV_CELL_PX))
			if cell == last:
				continue
			var placed := _snap_near(cell, last, cell_mode, want, 2)
			if placed.x < 0:
				continue
			last = placed
			var sample := _cell_center(placed)
			if raw[raw.size() - 1].distance_to(sample) > 0.01:
				raw.append(sample)

	## A* fino do último trecho, com teto próprio: cobre canais mais estreitos
	## que a célula grosseira (enseada de Bristol).
	func _begin_tail(from: Vector2i, to: Vector2i) -> bool:
		var prefix := _prefix
		if not begin(from, to, mode):
			_prefix = prefix
			return false
		_prefix = prefix
		_tail_from = from
		_tail_to = to
		tail = true
		strict_corners = false
		node_limit = MAX_TAIL_NODES
		return true

	## Célula navegável mais próxima do alvo, dentro do anel de raio `radius`.
	func _snap_near(cell: Vector2i, target: Vector2i, cell_mode: String, want: int,
			radius: int) -> Vector2i:
		if walkable(cell, cell_mode) \
				and (want < 0 or component(cell, cell_mode) == want):
			return cell
		var best := Vector2i(-1, -1)
		var best_distance := INF
		for ring in range(1, radius + 1):
			for dy in range(-ring, ring + 1):
				for dx in range(-ring, ring + 1):
					if maxi(absi(dx), absi(dy)) != ring:
						continue
					var candidate := cell + Vector2i(dx, dy)
					if not walkable(candidate, cell_mode):
						continue
					if want >= 0 and component(candidate, cell_mode) != want:
						continue
					var distance := float((candidate.x - target.x) * (candidate.x - target.x)
							+ (candidate.y - target.y) * (candidate.y - target.y))
					if distance < best_distance:
						best = candidate
						best_distance = distance
			if best.x >= 0:
				return best
		return Vector2i(-1, -1)

	static func _cell_center(cell: Vector2i) -> Vector2:
		return Vector2((float(cell.x) + 0.5) * NAV_CELL_PX,
				(float(cell.y) + 0.5) * NAV_CELL_PX)

	func _build_raw(start_index: int, goal_index: int) -> void:
		var cells: Array[Vector2i] = []
		var walk := goal_index
		while walk != start_index:
			cells.append(Vector2i(walk % w, walk / w))
			if _gen[walk] != _gen_id:
				return
			walk = _parent[walk]
		cells.append(Vector2i(walk % w, walk / w))
		cells.reverse()
		var path := PackedVector2Array()
		for cell in cells:
			path.append(Vector2(
				(float(cell.x) + 0.5) * NAV_CELL_PX,
				(float(cell.y) + 0.5) * NAV_CELL_PX,
			))
		raw = path

	func _line_walkable(a: Vector2, b: Vector2, cell_mode: String) -> bool:
		var steps: int = int(maxf(absf(a.x - b.x), absf(a.y - b.y)) / NAV_CELL_PX)
		for i in range(steps + 1):
			var t: float = float(i) / float(maxi(steps, 1))
			var point := a.lerp(b, t)
			if not walkable(Vector2i(int(point.x / NAV_CELL_PX),
					int(point.y / NAV_CELL_PX)), cell_mode):
				return false
		return true

	func _heuristic(a: Vector2i, b: Vector2i) -> float:
		return _heuristic_xy(a.x, a.y, b.x, b.y)

	## Heurística octile por coordenadas (sem montar Vector2i no laço do A*).
	func _heuristic_xy(x: int, y: int, target_x: int, target_y: int) -> float:
		var dx := absi(x - target_x)
		var dy := absi(y - target_y)
		return float(maxi(dx, dy)) + 0.41421356 * float(mini(dx, dy))

	func _heap_push(priority: float, cell: int) -> void:
		_hp.append(priority)
		_hc.append(cell)
		var index: int = _hp.size() - 1
		while index > 0:
			var parent: int = (index - 1) / 2
			if _hp[parent] <= _hp[index]:
				break
			var tp := _hp[parent]
			var tc := _hc[parent]
			_hp[parent] = _hp[index]
			_hc[parent] = _hc[index]
			_hp[index] = tp
			_hc[index] = tc
			index = parent

	func _heap_pop() -> int:
		var top: int = _hc[0]
		var last_priority: float = _hp[_hp.size() - 1]
		var last_cell: int = _hc[_hc.size() - 1]
		_hp.resize(_hp.size() - 1)
		_hc.resize(_hc.size() - 1)
		if not _hp.is_empty():
			_hp[0] = last_priority
			_hc[0] = last_cell
			var index := 0
			var size := _hp.size()
			while true:
				var left: int = index * 2 + 1
				var right: int = left + 1
				var smallest := index
				if left < size and _hp[left] < _hp[smallest]:
					smallest = left
				if right < size and _hp[right] < _hp[smallest]:
					smallest = right
				if smallest == index:
					break
				var tp := _hp[smallest]
				var tc := _hc[smallest]
				_hp[smallest] = _hp[index]
				_hc[smallest] = _hc[index]
				_hp[index] = tp
				_hc[index] = tc
				index = smallest
		return top


func _ready() -> void:
	_pool_root = Node3D.new()
	_pool_root.name = "VehiclePool"
	_pool_root.visible = false
	add_child(_pool_root)
	var image := NAV_MASK.get_image()
	# A busca lê os bytes crus: converte uma vez para RGBA8 (a máscara pode vir
	# em RGB8/importada com outro formato).
	if image.get_format() != Image.FORMAT_RGBA8:
		image.convert(Image.FORMAT_RGBA8)
	_nav = NavGrid.new()
	_nav.build(image)


## Enfileira os comboios de um tick de comércio. Barato de propósito: o custo
## (busca de rota + criação do nó) fica no pump de `_process()`.
func spawn_orders(orders: Array) -> void:
	_queue = _prioritize(orders).slice(0, MAX_CONVOYS)
	_depart_slot = 0


## Pedidos entre nações primeiro: são os que mostram navio e avião no mapa.
func _prioritize(orders: Array) -> Array:
	var sorted := orders.duplicate()
	sorted.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		var a_cross: bool = a["from_nation"] != a["to_nation"]
		var b_cross: bool = b["from_nation"] != b["to_nation"]
		return a_cross and not b_cross
	)
	return sorted


func _process(_delta: float) -> void:
	if _queue.is_empty() and _prep.is_empty() and (_nav == null or not _nav.active):
		return
	_pump(Time.get_ticks_usec() + FRAME_BUDGET_US)


## Trabalha na fila até o prazo. Cada passo é curto: ou uma fatia do A*, ou um
## passo da simplificação, ou a criação de um comboio já com as rotas prontas.
func _pump(deadline_usec: int) -> void:
	while true:
		if _nav != null and _nav.active:
			var status := _nav.step(deadline_usec)
			if status == 0:
				return
			_store_leg_path(status == 1)
			continue
		if _prep.is_empty():
			if not _start_next_order():
				return
			continue
		var legs: Array = _prep["legs"]
		var index: int = int(_prep["index"])
		if index >= legs.size():
			_spawn_convoy(_prep["order"], _prep["paths"])
			_prep = {}
			continue
		var leg: Dictionary = legs[index]
		var key := _leg_key(leg)
		if key.is_empty():
			# Avião vai em linha reta; sem busca.
			_prep["paths"].append(PackedVector2Array([leg["from_px"], leg["to_px"]]))
			_prep["index"] = index + 1
			continue
		if _path_cache.has(key):
			_prep["paths"].append(_path_cache[key])
			_prep["index"] = index + 1
			continue
		if not _begin_leg_search(leg, key):
			_path_cache[key] = PackedVector2Array()
			_prep["paths"].append(PackedVector2Array())
			_prep["index"] = index + 1
			continue
		if Time.get_ticks_usec() >= deadline_usec:
			return


func _start_next_order() -> bool:
	if _queue.is_empty():
		return false
	if _active_convoys >= MAX_CONVOYS:
		return false
	var order: Dictionary = _queue.pop_front()
	_prep = {
		"order": order,
		"legs": (order.get("legs", []) as Array),
		"index": 0,
		"paths": [],
		"key": "",
	}
	if (_prep["legs"] as Array).is_empty():
		stats["spawned"] = int(stats["spawned"]) + 1
		_cancel_spawn("no_legs")
		_prep = {}
		return true
	return true


## Chave de cache do caminho: extremos já encaixados na grade (dois pares de
## cidades vizinhas compartilham a rota) + modo.
func _leg_key(leg: Dictionary) -> String:
	var mode: String = String(leg.get("mode", "truck"))
	if mode == "plane":
		return ""
	var from_cell := _snap_cell(_to_cell(leg["from_px"]), mode)
	var to_cell := _snap_cell(_to_cell(leg["to_px"]), mode)
	if from_cell.x < 0 or to_cell.x < 0:
		return "none|%s|%s" % [leg["from_px"], leg["to_px"]]
	return "%s|%d,%d|%d,%d" % [mode, from_cell.x, from_cell.y, to_cell.x, to_cell.y]


func _begin_leg_search(leg: Dictionary, key: String) -> bool:
	if _nav == null:
		return false
	var mode: String = String(leg.get("mode", "truck"))
	var from_cell := _snap_cell(_to_cell(leg["from_px"]), mode)
	var to_cell := _snap_cell(_to_cell(leg["to_px"]), mode)
	if from_cell.x < 0 or to_cell.x < 0:
		return false
	_prep["key"] = key
	return _nav.begin(from_cell, to_cell, mode)


func _store_leg_path(found: bool) -> void:
	var key: String = String(_prep.get("key", ""))
	var path: PackedVector2Array = _nav.out if found else PackedVector2Array()
	if not key.is_empty():
		_path_cache[key] = path
	if _prep.is_empty():
		return
	_prep["paths"].append(path)
	_prep["index"] = int(_prep["index"]) + 1
	_prep["key"] = ""


func _snap_cell(cell: Vector2i, mode: String) -> Vector2i:
	if _nav == null:
		return Vector2i(-1, -1)
	var key := "%s|%d,%d" % [mode, cell.x, cell.y]
	if _snap_cache.has(key):
		return _snap_cache[key]
	var result := _nav.snap(cell, mode)
	_snap_cache[key] = result
	return result


func _to_cell(point: Vector2) -> Vector2i:
	return Vector2i(int(point.x / NAV_CELL_PX), int(point.y / NAV_CELL_PX))


func _spawn_convoy(order: Dictionary, paths: Array) -> void:
	stats["spawned"] = int(stats["spawned"]) + 1
	var legs: Array = order.get("legs", [])
	if legs.is_empty():
		_cancel_spawn("no_legs")
		return
	if _active_convoys >= MAX_CONVOYS:
		_cancel_spawn("no_slot")
		return
	# Só entra nos planos um trecho desenhável: avião voa em linha reta; caminhão
	# e navio precisam de um caminho com pelo menos dois pontos. Sem nenhum, o
	# comboio nasceria como medalhão solto e sumiria no fim do tween.
	var usable: Array[int] = []
	for i in range(mini(paths.size(), legs.size())):
		var leg: Dictionary = legs[i]
		var path: PackedVector2Array = paths[i]
		if String(leg.get("mode", "truck")) == "plane" or path.size() >= 2:
			usable.append(i)
	if usable.is_empty():
		_cancel_spawn("no_route", "%s->%s" % [
			String(order.get("from_city", "")), String(order.get("to_city", ""))])
		return

	var convoy: Node3D = _acquire_convoy()
	_active_convoys += 1
	_convoys.append(convoy)
	convoy.visible = true
	_generation += 1
	var generation := _generation
	convoy.set_meta("generation", generation)
	# Nó reciclado: mata o tween da viagem anterior antes de enfileirar o novo.
	_kill_tween(convoy)
	# Bandeira da nação que vai RECEBER o produto, acima do veículo. Fica oculta
	# até haver veículo (ver `_refresh_flags`).
	_add_flags(convoy, String(order.get("from_nation", "")),
			String(order.get("to_nation", "")))
	# O veículo já nasce no ponto de partida: o comboio nunca aparece só com o
	# medalhão, nem durante o escalonamento de partida.
	var first: Dictionary = legs[usable[0]]
	var first_path: PackedVector2Array = paths[usable[0]]
	if first_path.size() >= 2:
		_place_vehicle(convoy, String(first.get("mode", "truck")),
				_pixel_to_world(first_path[0]), _pixel_to_world(first_path[1]))
		stats["placed"] = int(stats["placed"]) + 1

	var tween := create_tween()
	# O tween nasce no ritmo atual da simulação (1x = 60 s por dia) e entra na
	# lista para acompanhar mudanças de velocidade e pausa.
	tween.set_speed_scale(_time_scale)
	if _paused:
		tween.pause()
	_tweens.append(tween)
	_tween_by_convoy[int(convoy.get_instance_id())] = tween
	tween.finished.connect(_on_tween_finished.bind(tween, convoy, generation))
	# Escalonamento de partida: comboios do mesmo tick saem em instantes
	# diferentes, evitando que andem colados sem tirar ninguém do caminho.
	var delay := DEPART_STAGGER * float(_depart_slot % DEPART_STAGGER_STEPS)
	_depart_slot += 1
	if delay > 0.0:
		tween.tween_interval(delay)
	for index in usable:
		_append_leg(tween, convoy, legs[index], paths[index], index == usable[0])

	tween.tween_callback(_finish_convoy.bind(convoy, generation))


## Recusa uma tentativa de comboio com motivo explícito (nada de medalhão que
## aparece e some sem chegar).
func _cancel_spawn(reason: String, detail := "") -> void:
	stats["cancelled"] = int(stats["cancelled"]) + 1
	if stats.has(reason):
		stats[reason] = int(stats[reason]) + 1
	stats["last_cancel"] = reason if detail.is_empty() else "%s %s" % [reason, detail]


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


func _on_tween_finished(tween: Tween, convoy: Node3D, generation: int) -> void:
	_tweens.erase(tween)
	if not is_instance_valid(convoy):
		return
	var id := int(convoy.get_instance_id())
	if _tween_by_convoy.get(id, null) == tween:
		_tween_by_convoy.erase(id)
	# Tween de uma geração antiga: o comboio já foi reciclado e este `finished`
	# não pode mexer na viagem nova.
	if int(convoy.get_meta("generation", -1)) != generation:
		return


## Mata o tween de uma viagem anterior do comboio reciclado. O `finished` de um
## tween morto não é emitido, mas callbacks já enfileirados na mesma fatia
## seriam; a geração em `_finish_convoy` cobre esse resto.
func _kill_tween(convoy: Node3D) -> void:
	var id := int(convoy.get_instance_id())
	var old: Tween = _tween_by_convoy.get(id, null)
	if old == null:
		return
	_tween_by_convoy.erase(id)
	_tweens.erase(old)
	if old.is_valid():
		old.kill()


## Enfileira no tween cada segmento do caminho do trecho.
func _append_leg(tween: Tween, convoy: Node3D, leg: Dictionary,
		path: PackedVector2Array, already_placed := false) -> void:
	var mode: String = leg["mode"]
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
	if not already_placed:
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


## Bandeirinha da nação de destino presa ao comboio (billboard: sempre virada
## para a câmera). Criada uma vez e mantida entre os trechos.
## Medalhões origem+destino acima do comboio: selo escuro + aro de latão +
## bandeira, lado a lado com um chevron de latão no meio ("de onde → para onde").
func _add_flags(convoy: Node3D, from_nation: String, to_nation: String) -> void:
	var flags: Sprite3D = convoy.get_node_or_null("Flags")
	if flags == null:
		return
	var texture := _pair_texture(from_nation, to_nation)
	flags.texture = texture
	_refresh_flags(convoy)


## O medalhão origem→destino só aparece quando há veículo. Sem ele, o comboio
## viraria um medalhão solto no mapa que some no fim do tween.
func _refresh_flags(convoy: Node3D) -> void:
	if not is_instance_valid(convoy):
		return
	var flags := convoy.get_node_or_null("Flags") as Sprite3D
	if flags == null:
		return
	flags.visible = flags.texture != null and _vehicle_of(convoy) != null


## Veículo preso ao comboio (o único filho Node3D que não é o medalhão).
func _vehicle_of(convoy: Node3D) -> Node3D:
	for child in convoy.get_children():
		if child is Node3D and String(child.name) != "Flags":
			return child as Node3D
	return null


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


## Disco escuro + aro de latão. O disco é um carimbo pronto (Image carimbada),
## copiado com `blit_rect` (C++): desenhar 60x60 pixels com set_pixel a cada par
## custava ~1,7 ms por medalhão na primeira vez.
func _draw_medallion(img: Image, center: Vector2, nation: String) -> void:
	if _disc_image == null:
		_disc_image = Image.create(60, 60, false, Image.FORMAT_RGBA8)
		_disc_image.fill(Color(0, 0, 0, 0))
		var radius := 30.0
		var dark := Color("#363431")
		var brass := Color("#C9A24A")
		for y in range(60):
			for x in range(60):
				var d := Vector2(x + 0.5, y + 0.5).distance_to(Vector2(30.0, 30.0))
				if d <= radius:
					_disc_image.set_pixel(x, y, brass if d > radius - 5.0 else dark)
	img.blit_rect(_disc_image, Rect2i(0, 0, 60, 60),
			Vector2i(int(center.x - 30.0), int(center.y - 30.0)))
	if nation.is_empty():
		return
	var flag := Economy.flag_texture(nation)
	if flag == null:
		return
	var fi := flag.get_image()
	if fi == null or fi.get_width() == 0:
		return
	var fw := int(30.0 * 1.15)
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

	# Troca só o veículo; a bandeira ("Flags") é mantida. O modelo antigo volta
	# para o pool (em vez de ser liberado): o nó é reciclado no próximo trecho.
	for child in convoy.get_children():
		if String(child.name) != "Flags":
			convoy.remove_child(child)
			_park_vehicle(child, String(convoy.get_meta("mode", "")))

	var model := _acquire_vehicle(mode)
	convoy.add_child(model)
	convoy.set_meta("mode", mode)
	convoy.set_meta("collision_radius", COLLISION_RADIUS_BY_MODE.get(mode, 0.05))
	convoy.position = start + Vector3(0.0, HEIGHT_OFFSET, 0.0)
	_face_target(convoy, start, end)
	_refresh_flags(convoy)


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
	if _nav == null or not _nav.walkable(pushed_cell, mode):
		return Vector3.ZERO
	return push


func _finish_convoy(convoy: Node3D, generation := -1) -> void:
	if not is_instance_valid(convoy):
		return
	# O comboio pode ter sido reciclado por um spawn novo antes deste callback
	# (tween antigo). A geração guarda a viagem: só a dona pode encerrá-la.
	if generation >= 0 and int(convoy.get_meta("generation", -1)) != generation:
		return
	_active_convoys = maxi(0, _active_convoys - 1)
	_convoys.erase(convoy)
	stats["finished"] = int(stats["finished"]) + 1
	var id := int(convoy.get_instance_id())
	if _tween_by_convoy.get(id, null) != null:
		_tween_by_convoy.erase(id)
	# Recicla: devolve o veículo ao pool e esconde o nó do comboio (nada de
	# queue_free — o próximo comboio reusa o mesmo nó).
	for child in convoy.get_children():
		if String(child.name) != "Flags":
			convoy.remove_child(child)
			_park_vehicle(child, String(convoy.get_meta("mode", "")))
	convoy.visible = false
	convoy.set_meta("separation", Vector3.ZERO)
	_refresh_flags(convoy)
	_convoy_pool.append(convoy)


## Aponta o comboio para o destino; sem deslocamento, mantém a orientação atual.
func _face_target(convoy: Node3D, start: Vector3, end: Vector3) -> void:
	if not is_instance_valid(convoy):
		return
	var direction := end - start
	direction.y = 0.0
	if direction.length_squared() < 1e-8:
		return
	convoy.look_at(convoy.global_position + direction.normalized(), Vector3.UP)


func _acquire_convoy() -> Node3D:
	if not _convoy_pool.is_empty():
		return _convoy_pool.pop_back()
	var convoy := Node3D.new()
	var flags := Sprite3D.new()
	flags.name = "Flags"
	flags.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	flags.no_depth_test = true
	flags.render_priority = 10
	flags.pixel_size = PAIR_HEIGHT / float(PAIR_TEX_H)
	flags.position = Vector3(0.0, FLAG_HEIGHT, 0.0)
	convoy.add_child(flags)
	add_child(convoy)
	return convoy


func _acquire_vehicle(mode: String) -> Node3D:
	var pool: Array = _vehicle_pool.get(mode, [])
	while not pool.is_empty():
		var model: Node3D = pool.pop_back()
		if not is_instance_valid(model):
			continue
		# O nó dorme dentro de `_pool_root`: o motor recusa `add_child` num nó
		# que ainda tem pai (`already has a parent 'VehiclePool'`), e o comboio
		# ficava só com o medalhão. Tira do pool antes de devolver.
		var parent := model.get_parent()
		if parent != null:
			parent.remove_child(model)
		model.visible = true
		return model
	return _instantiate_model(mode)


func _park_vehicle(model: Node, mode: String) -> void:
	if not is_instance_valid(model):
		return
	if not (model is Node3D):
		model.queue_free()
		return
	var node := model as Node3D
	# O modo vem do próprio modelo (o comboio pode estar sem `mode` no primeiro
	# quadro): um caminhão nunca pode cair no pool de navio.
	var actual := String(node.get_meta("vehicle_mode", mode))
	if actual.is_empty():
		actual = "truck"
	var pool: Array = _vehicle_pool.get(actual, [])
	if pool.has(node):
		return
	# Normaliza escala/transformação/visibilidade: o modelo volta ao pool como
	# saiu da cena, pronto para o próximo trecho.
	var scale_value := float(node.get_meta("vehicle_scale", 1.0))
	node.visible = false
	node.transform = Transform3D(
			Basis(Vector3.UP, MODEL_YAW_OFFSET).scaled(Vector3.ONE * scale_value),
			Vector3.ZERO)
	node.set_meta("vehicle_mode", actual)
	if node.get_parent() != _pool_root:
		var parent := node.get_parent()
		if parent != null:
			parent.remove_child(node)
		_pool_root.add_child(node)
	pool.append(node)
	_vehicle_pool[actual] = pool


func _instantiate_model(mode: String) -> Node3D:
	var scene: PackedScene = TRUCK_SCENE
	match mode:
		"ship":
			scene = SHIP_SCENE
		"plane":
			scene = PLANE_SCENE

	var model := scene.instantiate() as Node3D
	var scale_value := _scale_for(mode, scene)
	model.rotation.y = MODEL_YAW_OFFSET
	model.scale = Vector3.ONE * scale_value
	# Marcas usadas por `_park_vehicle` (reciclagem para o pool certo e com a
	# escala certa, mesmo que o comboio troque de modo entre trechos).
	model.set_meta("vehicle_mode", mode)
	model.set_meta("vehicle_scale", scale_value)
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
