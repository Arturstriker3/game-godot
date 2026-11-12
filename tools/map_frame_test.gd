extends Node3D

## Teste de fumaça POR FRAME do mapa com o cenário real (nações e cidades do
## cenário, portos do ports.csv). Reproduz o fluxo do jogo — CityWorld.build + _process a
## cada frame — com a câmera viajando e dando zoom in/out por Europa, Bálcãs,
## Rússia, Ásia e Américas. Qualquer erro de script no caminho falha o teste.
##
## Também cobre os dois defeitos que derrubavam/estragavam o mapa:
##  (a) métricas de porto sem "rect" (porto atrás da câmera/fora do frustum
##      depois do deslocamento de layout) — o frame não pode estourar;
##  (b) porto da própria cidade não pode ficar sobre o ícone/nome dela: tem de
##      virar badge pequeno ao lado do selo (Copenhagen, Riga, Helsinki,
##      Amsterdã, Hamburgo);
##  (c) toda nação do cenário tem cor no mapa político e vizinhos não repetem o
##      mesmo tom.
##
## Uso (SEMPRE headless; nunca abrir a cena principal de forma interativa):
##   godot --headless --path . res://tools/map_frame_test.tscn

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const NATIONS_CSV := "res://data/definitions/scenario/nations.csv"

## Regiões cobertas pela varredura (ids de cidade do cenário).
const REGIONS: Array[String] = [
	"DEU-BER",  # Europa central
	"SRB-BEL",  # Bálcãs
	"RUS-MOS",  # Rússia
	"CHN-BEI",  # Ásia
	"USA-WAS",  # América do Norte
	"BRA-SAO",  # América do Sul
]
## Portos da própria cidade que precisam virar badge fora do selo.
const PORT_CITIES: Array[String] = ["DNK-COP", "LVA-RIG", "FIN-HEL",
		"NED-AMS", "DEU-HAM"]
## Nações que apareciam sem cor no mapa político antes da paleta do cenário.
const COLOR_SAMPLE: Array[String] = ["POL", "CZE", "ITA", "AUT", "SRB", "HRV",
		"HUN", "BGR", "ROU", "GRC", "SVK", "SVN", "BIH", "MNE", "MKD", "ALB"]

const VIEWPORT := Vector2i(1920, 1080)
const PITCH_DEG := -56.0
const CAMERA_FOV := 75.0
const NEAR_DISTANCE := 48.0
const FAR_DISTANCE := 130.0
const PORT_CHECK_DISTANCE := 52.0
const FRAMES_PER_REGION := 50
## Alinhamento ícone/rótulo/anel × cidade: amostra aleatória + os casos do
## Egito que apareciam deslocados (Al Mansurah na costa), em 3 zooms.
const ALIGN_SAMPLE := 12
const ALIGN_ZOOMS: Array[float] = [13.0, 26.0, 52.0]
const ALIGN_FIXED: Array[String] = ["EGY-ALM", "EGY-TAN", "EGY-CAI"]
const ALIGN_TOLERANCE_PX := 2.0
const RING_RADIUS_PX := 26.0
## Espelham `map/city_world.gd` (contrato do rótulo composto).
const PLACE_ICON_HEIGHT := 1.0
const PLACE_RING_SCALE := 1.45
const PLACE_ICON_GAP := 0.3
## Se a varredura travar (erro que interrompe o await), o timer encerra com
## falha em vez de deixar o runner headless pendurado no CI. A varredura leva
## ~7 s nesta máquina; 120 s é teto rígido, não orçamento.
const TEST_TIMEOUT_SEC := 120.0
## Distância RGB mínima entre vizinhos no mapa político (mesma regra do
## TerritoryMap.COLOR_MIN_DISTANCE). O ΔE ≥ 30 dos dados é auditado no gerador;
## aqui o RGB é o proxy barato que o próprio mapa aplica.
const COLOR_MIN_DISTANCE := 0.16
const OWNER_COLUMNS := 64


## Coleta todo erro reportado pela engine (SCRIPT ERROR incluso) enquanto o
## teste roda. Sem isso um erro de script passaria como frame "ok".
class ErrorLogger extends Logger:
	var errors: Array[String] = []

	func _log_error(function: String, file: String, line: int, code: String,
			_rationale: String, _editor_notify: bool, error_type: int,
			_script_backtraces: Array[ScriptBacktrace]) -> void:
		# ERROR_TYPE_WARNING (1) não é erro de script; só erro/script/shader
		# (0/2/3) derrubam o teste.
		if error_type == 1:
			return
		errors.append("%s (%s:%d, %s)" % [code, file, line, function])

	func take() -> Array[String]:
		var out := errors.duplicate()
		errors.clear()
		return out


var _failures: Array[String] = []
var _logger := ErrorLogger.new()
var _watchdog: Timer
var _world: Node3D
var _camera: Camera3D
var _pivot: Node3D


func _ready() -> void:
	_watchdog = Timer.new()
	_watchdog.wait_time = TEST_TIMEOUT_SEC
	_watchdog.one_shot = true
	_watchdog.timeout.connect(_on_timeout)
	add_child(_watchdog)
	_watchdog.start()
	_run.call_deferred()


## O `_run` é uma corrotina: um erro no meio dela pararia a varredura sem
## chamar `_finish`. O watchdog garante saída não-zero em vez de runner pendurado.
func _on_timeout() -> void:
	print("[map_frames] TIMEOUT: varredura não concluiu em %.0f s"
			% TEST_TIMEOUT_SEC)
	get_tree().quit(124)


func _load_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_failures.append("não abriu " + path)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


func _run() -> void:
	OS.add_logger(_logger)
	get_tree().root.size = VIEWPORT
	await get_tree().process_frame

	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	if cities.is_empty() or ports.is_empty():
		_finish()
		return

	_world = load("res://map/city_world.gd").new()
	add_child(_world)
	_world.build(cities, ports)

	_pivot = Node3D.new()
	add_child(_pivot)
	_camera = Camera3D.new()
	_camera.fov = CAMERA_FOV
	_camera.near = 0.1
	_camera.far = 4000.0
	_pivot.add_child(_camera)
	_camera.current = true
	await get_tree().process_frame

	_check_port_metrics_guard()
	_check_port_offset_edge()
	await get_tree().process_frame

	# ~300 frames de câmera em movimento (zoom in/out + giro curto).
	for region: String in REGIONS:
		await _sweep(region)

	for city_id: String in PORT_CITIES:
		await _check_port_badge(city_id)

	await _check_city_alignment(cities)
	await _check_capital_flags()
	_check_nation_colors()

	get_tree().root.size = VIEWPORT
	await get_tree().process_frame
	_finish()


## Move a câmera sobre uma região por FRAMES_PER_REGION frames, alternando
## aproximação e afastamento e girando um pouco — é o que faz portos entrarem e
## saírem do frustum (o caso que estourava o layout).
func _sweep(city_id: String) -> void:
	_focus(city_id)
	for index in FRAMES_PER_REGION:
		var t := float(index) / float(FRAMES_PER_REGION - 1)
		var zoom := 0.5 - 0.5 * cos(t * TAU)
		_camera.position = Vector3(0.0, 0.0,
				lerpf(NEAR_DISTANCE, FAR_DISTANCE, zoom))
		_pivot.rotation_degrees = Vector3(
				PITCH_DEG + sin(t * TAU) * 5.0, sin(t * TAU) * 9.0, 0.0)
		await get_tree().process_frame
		_collect_errors("sweep %s frame %d" % [city_id, index])


func _focus(city_id: String) -> void:
	_world.position = Vector3.ZERO
	if _world.has_method("city_world_position"):
		_world.position = -_world.city_world_position(city_id)


## (a) Para N cidades (amostra fixa do Egito + aleatórias) em 3 zooms:
##   |pos_ícone − pos_cidade| < 2 px;
##   |centro_anel − pos_ícone| < 2 px e raio do anel = 26 px;
##   rótulo ancorado no ícone por offset fixo (mesmo y/z, x = selo+respiro+texto).
## É a regressão do +1 u na âncora: no zoom próximo ele jogava o símbolo para o
## norte (Al Mansurah, que fica ~1 u da costa, aparecia em cima da praia).
func _check_city_alignment(cities: Array) -> void:
	var sample := _alignment_sample(cities)
	var worst_icon := 0.0
	var worst_ring := 0.0
	var checked := 0
	for city_id: String in sample:
		if not _world._records_by_id.has(city_id):
			_failures.append("alinhamento: cidade %s fora do mapa" % city_id)
			continue
		for zoom: float in ALIGN_ZOOMS:
			_focus(city_id)
			_pivot.rotation_degrees = Vector3(PITCH_DEG, 0.0, 0.0)
			_camera.position = Vector3(0.0, 0.0, zoom)
			await get_tree().process_frame
			await get_tree().process_frame
			var record: Dictionary = _world._records_by_id[city_id]
			var icon_world: Vector3 = _world._city_icon_world(record)
			var city_world: Vector3 = _world.city_world_position(city_id)
			if _camera.is_position_behind(icon_world):
				_failures.append("alinhamento %s d%03d: ícone atrás da câmera"
						% [city_id, int(zoom)])
				continue
			var icon_px := _camera.unproject_position(icon_world)
			var city_px := _camera.unproject_position(city_world)
			var icon_delta := icon_px.distance_to(city_px)
			worst_icon = maxf(worst_icon, icon_delta)
			if icon_delta > ALIGN_TOLERANCE_PX:
				_failures.append("alinhamento %s d%03d: ícone a %.1f px da cidade"
						% [city_id, int(zoom), icon_delta])

			_world.set_hover(city_id)
			await get_tree().process_frame
			await get_tree().process_frame
			var hover: MeshInstance3D = _world._hover
			if hover == null or not hover.visible:
				_failures.append("alinhamento %s d%03d: anel de hover ausente"
						% [city_id, int(zoom)])
				_world.set_hover("")
				continue
			var ring_px := _camera.unproject_position(hover.global_position)
			var ring_delta := ring_px.distance_to(icon_px)
			worst_ring = maxf(worst_ring, ring_delta)
			if ring_delta > ALIGN_TOLERANCE_PX:
				_failures.append("alinhamento %s d%03d: anel a %.1f px do ícone"
						% [city_id, int(zoom), ring_delta])
			var ring_radius: float = MapPick.icon_radius_px(_camera,
					hover.global_position, hover.scale.x)
			if absf(ring_radius - RING_RADIUS_PX) > 1.0:
				_failures.append("alinhamento %s d%03d: anel com raio %.1f px"
						% [city_id, int(zoom), ring_radius])
			_check_label_anchor(city_id, record, zoom)
			_world.set_hover("")
			await get_tree().process_frame
			checked += 1
	print("[map_frames] alinhamento: %d medições, pior ícone=%.2f px, pior anel=%.2f px"
			% [checked, worst_icon, worst_ring])


## Amostra reprodutível: os casos fixos do Egito + cidades sorteadas.
func _alignment_sample(cities: Array) -> Array[String]:
	var ids: Array[String] = []
	for fixed in ALIGN_FIXED:
		if _world._records_by_id.has(fixed):
			ids.append(fixed)
	var pool: Array[String] = []
	for city: Dictionary in cities:
		var id := String(city.get("id", ""))
		if not id.is_empty() and not ids.has(id):
			pool.append(id)
	pool.sort()
	var rng := RandomNumberGenerator.new()
	rng.seed = 20261112
	while ids.size() < ALIGN_SAMPLE and not pool.is_empty():
		var index := rng.randi_range(0, pool.size() - 1)
		ids.append(pool[index])
		pool.remove_at(index)
	return ids


## Rótulo ancorado ao ícone: mesmo y/z da âncora e deslocamento em X = meio
## selo + respiro + meio texto. Verifica também que o Label3D está no texto.
func _check_label_anchor(city_id: String, record: Dictionary, zoom: float) -> void:
	var label: Label3D = record.get("label")
	if label == null:
		_failures.append("alinhamento %s d%03d: rótulo não atribuído"
				% [city_id, int(zoom)])
		return
	var wrap := float(record.get("wrap_x", 0.0))
	var anchor: Vector3 = record["anchor"] + Vector3(wrap, 0.0, 0.0)
	var em := float(record["font_size"]) * float(record["pixel_size"])
	var ring_h := em * PLACE_ICON_HEIGHT * PLACE_RING_SCALE
	var gap := em * PLACE_ICON_GAP
	var text_w: float = _world._city_text_width(record)
	var expected := ring_h * 0.5 + gap + text_w * 0.5
	var center: Vector3 = record["text_center"]
	var delta := center - anchor
	if absf(delta.x - expected) > 0.002 or absf(delta.y) > 0.002 \
			or absf(delta.z) > 0.002:
		_failures.append(("alinhamento %s d%03d: rótulo fora da âncora "
				+ "(dx=%.3f esperado=%.3f, dy=%.3f, dz=%.3f)")
				% [city_id, int(zoom), delta.x, expected, delta.y, delta.z])
	if not label.position.is_equal_approx(center):
		_failures.append("alinhamento %s d%03d: Label3D fora de text_center"
				% [city_id, int(zoom)])


## (b) 100% das capitais visíveis no LOD têm bandeira carregada e acesa.
func _check_capital_flags() -> void:
	_focus("EGY-CAI")
	_pivot.rotation_degrees = Vector3(PITCH_DEG, 0.0, 0.0)
	_camera.position = Vector3(0.0, 0.0, PORT_CHECK_DISTANCE)
	await get_tree().process_frame
	await get_tree().process_frame
	# Contrato global: TODAS as capitais do cenário têm bandeira carregada.
	var all_capitals := 0
	for record in _world._city_nodes:
		if not bool(record.get("capital", false)):
			continue
		all_capitals += 1
		var flag: Sprite3D = record.get("flag")
		if flag == null or flag.texture == null:
			_failures.append("bandeira: capital %s sem textura no build"
					% String(record["id"]))
	if all_capitals != _expected_capitals():
		_failures.append("bandeira: %d capitais no cenário (esperado %d)"
				% [all_capitals, _expected_capitals()])

	# E as que estão de fato visíveis neste frame acendem a bandeira.
	var capitals := 0
	for record in _world._visible_city_records:
		if not bool(record.get("capital", false)):
			continue
		capitals += 1
		var id := String(record["id"])
		var flag: Sprite3D = record.get("flag")
		if flag == null:
			_failures.append("bandeira: capital %s sem Sprite3D" % id)
			continue
		if flag.texture == null:
			_failures.append("bandeira: capital %s sem textura" % id)
		if not flag.visible:
			_failures.append("bandeira: capital %s com bandeira apagada" % id)
	if capitals == 0:
		_failures.append("bandeira: nenhuma capital visível para auditar")
	else:
		print("[map_frames] bandeiras: %d capitais visíveis, 100%% com textura"
				% capitals)


## Regressão direta do crash: `_port_metrics` pode voltar SEM "rect" depois de
## um deslocamento (porto atrás da câmera/fora do frustum). O layout não pode
## ler a chave crua — `_metrics_rect` devolve Rect2() e o porto não é desenhado.
func _check_port_metrics_guard() -> void:
	var ports: Array = _world.get("_port_nodes")
	if ports.is_empty():
		_failures.append("sem registros de porto para o teste de métricas")
		return
	var record: Dictionary = ports[0]
	var saved_marker: Vector3 = record["marker_world"]
	_camera.position = Vector3(0.0, 0.0, PORT_CHECK_DISTANCE)
	_pivot.rotation_degrees = Vector3(PITCH_DEG, 0.0, 0.0)
	var screen: Vector2 = get_viewport().get_visible_rect().size
	record["marker_world"] = _world.to_local(
			_camera.project_position(screen * 0.5, PORT_CHECK_DISTANCE))
	record["badge"] = false
	var before: Dictionary = _world.call("_port_metrics", record, _camera)
	var before_rect: Rect2 = _world.call("_metrics_rect", before)
	if before_rect.size.x <= 0.0:
		_failures.append("métricas de porto sem rect no centro da tela")
	# Deslocamento absurdo (equivalente a um candidato de layout para fora da
	# tela): as métricas voltam vazias, que era o caso que estourava o acesso.
	_world.call("_apply_port_offset", record, Vector2(0.0, -100000.0), _camera)
	var after: Dictionary = _world.call("_port_metrics", record, _camera)
	if not after.is_empty():
		_failures.append("porto fora da tela ainda com métricas (teste inválido)")
	var after_rect: Rect2 = _world.call("_metrics_rect", after)
	if after_rect.size.x > 0.0:
		_failures.append("_metrics_rect não protegeu métricas vazias")
	record["marker_world"] = saved_marker
	_collect_errors("métricas de porto")


## Reproduz o crash de forma determinística: o porto está a poucos px da borda
## direita, o selo de uma cidade "fantasma" minúscula colide com ele e o layout
## escolhe o candidato de 8 px para o lado do mar. Esse deslocamento tira o
## porto do frustum e `_port_metrics` volta SEM "rect" — exatamente o estado em
## que o layout lia `metrics["rect"]` cru e estourava.
func _check_port_offset_edge() -> void:
	var ports: Array = _world.get("_port_nodes")
	if ports.is_empty():
		_failures.append("sem registros para o teste de borda do porto")
		return
	var port: Dictionary = ports[0]
	var saved_ports: Array = _world.get("_visible_port_records")
	var saved_cities: Array = _world.get("_visible_city_records")
	var saved_active: Array = _world.get("_active_labels")
	var port_saved := {
		"marker_world": port["marker_world"],
		"marker_base": port["marker_base"],
		"badge": bool(port.get("badge", false)),
		"fused": bool(port["fused"]),
		"label_alpha": float(port["label_alpha"]),
		"city_id": String(port["city_id"]),
		"sea_dir_world": port["sea_dir_world"],
		"draw": bool(port.get("draw", false)),
		"pixel_size": float(port.get("pixel_size", 0.05)),
	}

	# Câmera de cima para a borda da tela ser previsível.
	_pivot.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
	_camera.position = Vector3(0.0, 0.0, 50.0)
	var screen: Vector2 = get_viewport().get_visible_rect().size
	var world_point := _camera.project_position(
			Vector2(screen.x - 3.0, screen.y * 0.5), 50.0)
	var marker_local: Vector3 = _world.to_local(world_point)
	var view_basis := _camera.global_transform.basis
	var px_per_world := _camera.unproject_position(world_point).distance_to(
			_camera.unproject_position(world_point + view_basis.y))
	if px_per_world <= 0.0:
		_failures.append("borda do porto: escala de tela inválida")
		return

	port["marker_world"] = marker_local
	port["marker_base"] = marker_local
	port["badge"] = false
	port["fused"] = false
	port["city_id"] = ""
	port["label_alpha"] = 1.0
	port["sea_dir_world"] = view_basis.x
	# Selo de ~2 px: um ícone grande nunca sairia do candidato máximo de 12 px.
	port["pixel_size"] = 0.008

	# Coluna de selos "fantasma" de 4 em 4 px: os candidatos verticais (até
	# 12 px) sempre encostam em outro selo, então o primeiro candidato livre é o
	# do mar (+4 px à direita). Com o porto a 3 px da borda, esse deslocamento
	# o joga para fora do frustum — métricas vazias depois do offset.
	var ghosts: Array = []
	for offset_px: float in [-8.0, -4.0, 0.0, 4.0, 8.0]:
		var ghost_pos: Vector3 = marker_local \
				+ view_basis.y * (offset_px / px_per_world)
		ghosts.append({
			"id": "EDGE-%d" % int(offset_px),
			"kind": "city",
			"city": {"name": "Edge"},
			"anchor": ghost_pos,
			"text_center": ghost_pos,
			"font_size": 2,
			"pixel_size": 0.01,
			"priority": 1.0,
			"capital": false,
			"label_alpha": 1.0,
			"wrap_x": 0.0,
			"label": null,
		})
	_world.set("_visible_port_records", [port])
	_world.set("_visible_city_records", ghosts)
	_world.call("_layout_place_labels", _camera)
	_collect_errors("borda do porto")

	var report: Array = _world.label_layout_report()
	var port_entry := _entry(report, "port", String(port["id"]))
	if port_entry.is_empty():
		_failures.append("borda do porto: porto fora do relatório do layout")
	elif bool(port_entry["marker_visible"]):
		_failures.append("borda do porto: selo desenhado sem retângulo válido")
	var moved: Vector3 = port["marker_world"]
	if moved.is_equal_approx(marker_local):
		_failures.append("borda do porto: candidato de layout não foi aplicado")

	# Restaura o estado real; o próximo frame refaz layout normalmente.
	port["marker_world"] = port_saved["marker_world"]
	port["marker_base"] = port_saved["marker_base"]
	port["badge"] = port_saved["badge"]
	port["fused"] = port_saved["fused"]
	port["label_alpha"] = port_saved["label_alpha"]
	port["city_id"] = port_saved["city_id"]
	port["sea_dir_world"] = port_saved["sea_dir_world"]
	port["draw"] = port_saved["draw"]
	port["pixel_size"] = port_saved["pixel_size"]
	_world.set("_visible_port_records", saved_ports)
	_world.set("_visible_city_records", saved_cities)
	_world.set("_active_labels", saved_active)
	_world.set("_layout_dirty", true)


func _collect_errors(tag: String) -> void:
	for error: String in _logger.take():
		_failures.append("%s: erro de script: %s" % [tag, error])


## O porto da própria cidade tem de estar como badge (ou não desenhado), nunca
## sobre o retângulo do selo/nome da cidade.
func _check_port_badge(city_id: String) -> void:
	_focus(city_id)
	_camera.position = Vector3(0.0, 0.0, PORT_CHECK_DISTANCE)
	_pivot.rotation_degrees = Vector3(PITCH_DEG, 0.0, 0.0)
	for _i in 3:
		await get_tree().process_frame
	_collect_errors("porta %s" % city_id)

	var report: Array = _world.label_layout_report()
	var city_entry := _entry(report, "city", city_id)
	var port_entry := _entry(report, "port", city_id)
	if city_entry.is_empty():
		_failures.append("%s: cidade fora do layout" % city_id)
		return
	if port_entry.is_empty():
		_failures.append("%s: porto fora do layout" % city_id)
		return
	if not bool(port_entry["marker_visible"]):
		# Sem vaga ao lado do selo: regra aceita (só o modelo 3D fica na costa).
		return
	if not bool(port_entry["fused"]):
		_failures.append("%s: porto da cidade desenhado como selo de costa" % city_id)
	if (port_entry["rect"] as Rect2).intersects(city_entry["rect"]):
		_failures.append("%s: porto sobre o selo/nome da cidade" % city_id)
	print("[map_frames] %s: porto %s badge=%s offset=%.1f" % [
		city_id, port_entry["id"], str(port_entry["badge"]),
		(port_entry["offset"] as Vector2).length()])


func _entry(report: Array, kind: String, id: String) -> Dictionary:
	for entry: Dictionary in report:
		if String(entry["kind"]) == kind and String(entry["id"]) == id:
			return entry
	return {}


## Toda nação do cenário tem cor no mapa político, a textura de províncias
## carrega essa cor e vizinhos não repetem o mesmo tom.
func _check_nation_colors() -> void:
	var territory = _world.call("territory")
	if territory == null:
		_failures.append("TerritoryMap nulo após build")
		return
	var tags := _scenario_tags()
	if tags.is_empty():
		return
	var painted := 0
	for tag: String in tags:
		var color: Color = territory.nation_color(tag)
		if color.a <= 0.0:
			_failures.append("%s: sem cor no mapa político" % tag)
			continue
		painted += 1
		# Vizinhança: reporta o par uma vez (tag < vizinho).
		var neighbors: Array = territory.adjacent_nations(tag)
		for neighbor_value: Variant in neighbors:
			var neighbor := String(neighbor_value)
			if tag > neighbor or not tags.has(neighbor):
				continue
			var other: Color = territory.nation_color(neighbor)
			if _distance(color, other) < COLOR_MIN_DISTANCE:
				_failures.append("vizinhos %s/%s com tons iguais (%.2f)" % [
					tag, neighbor, _distance(color, other)])

	# A cor tem de chegar na textura que o shader lê (província -> dono).
	var image: Image = territory.owner_image()
	if image == null:
		_failures.append("owner_image nulo (shader sem cores)")
		return
	for tag: String in COLOR_SAMPLE:
		var expected: Color = territory.nation_color(tag)
		var found := false
		for province: Dictionary in territory.provinces():
			if String(province["nation"]) != tag:
				continue
			var pid := int(province["id"])
			var pixel := image.get_pixel(pid % OWNER_COLUMNS, pid / OWNER_COLUMNS)
			found = true
			if absf(pixel.r - expected.r) > 0.02 \
					or absf(pixel.g - expected.g) > 0.02 \
					or absf(pixel.b - expected.b) > 0.02:
				_failures.append("%s: textura de dono sem a cor do cenário" % tag)
			if _is_beige(pixel):
				_failures.append("%s: província continua bege no mapa político" % tag)
			break
		if not found:
			_failures.append("%s: sem província no atlas (não dá para checar cor)" % tag)
	print("[map_frames] %d/%d nações com cor; textura de províncias confere"
			% [painted, tags.size()])


func _scenario_tags() -> Array[String]:
	var out: Array[String] = []
	var file := FileAccess.open(NATIONS_CSV, FileAccess.READ)
	if file == null:
		_failures.append("não abriu " + NATIONS_CSV)
		return out
	var header := file.get_csv_line()
	var id_index := header.find("id")
	if id_index < 0:
		file.close()
		return out
	while not file.eof_reached():
		var row := file.get_csv_line()
		if row.size() <= id_index:
			continue
		var tag := String(row[id_index]).strip_edges()
		if tag != "":
			out.append(tag)
	file.close()
	return out


static func _distance(a: Color, b: Color) -> float:
	var dr := a.r - b.r
	var dg := a.g - b.g
	var db := a.b - b.b
	return sqrt(dr * dr + dg * dg + db * db)


## Bege neutro do MAPCOLOR9 do atlas (o que aparecia nas nações sem cor).
static func _is_beige(color: Color) -> bool:
	return color.r > 0.62 and color.r < 0.83 and color.g > 0.6 \
			and color.g < 0.8 and color.b > 0.5 and color.b < 0.72 \
			and absf(color.r - color.g) < 0.12


func _expected_capitals() -> int:
	## Contagem de capitais vem do próprio cenário; nada de constante mágica.
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(CITIES_PATH))
	if parsed == null:
		return 0
	var total := 0
	for city in parsed:
		if bool(city.get("capital", false)):
			total += 1
	return total


func _finish() -> void:
	_watchdog.stop()
	OS.remove_logger(_logger)
	for error: String in _logger.take():
		_failures.append("erro de script no encerramento: " + error)
	if _failures.is_empty():
		print("[map_frames] OK: %d frames em %d regiões, portos badge e cores das %d nações."
				% [FRAMES_PER_REGION * REGIONS.size(), REGIONS.size(),
				_scenario_tags().size()])
		get_tree().quit(0)
		return
	print("[map_frames] FALHAS (%d):" % _failures.size())
	for failure in _failures:
		print(" - " + failure)
	get_tree().quit(1)
