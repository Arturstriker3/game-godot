extends Node

## Verifica os estreitos da máscara de navegação (`map/map_data/nav_mask.png`)
## com o `NavGrid` REAL usado pelos comboios.
##
## Três níveis:
##   1. conectividade: Atlântico, Mediterrâneo, Negro, Báltico, Vermelho e Golfo
##      Pérsico no MESMO componente de água (Gibraltar, Dardanelos/Bósforo,
##      Øresund e Suez abertos no gerador `tools/generate_nav_mask.py`);
##   2. portos: a água alcançada pelo cais é o MESMO componente do mar aberto em
##      frente à costa (não uma lagoa/enseada isolada) — comparado com uma
##      semente de mar aberto;
##   3. rota: o A* de navio acha caminho pelos estreitos e respeita o orçamento
##      de fatia (`FRAME_BUDGET_US`), sem estourar o tempo total da rota.
##
##   godot --headless --path . res://tools/nav_strait_test.tscn

const ConvoysScript: Script = preload("res://core/economy/trade_convoys.gd")
const MASK_SIZE := Vector2i(2500, 1250)
const NAV_CELL_PX := 2.0
## `FRAME_BUDGET_US` do trade_convoys.gd.
const FRAME_BUDGET_US := 1500
## Teto rígido da fatia (orçamento + granularidade do fatiamento).
const MAX_SLICE_US := 3000
## Teto do custo total de uma rota (soma das fatias).
const MAX_ROUTE_MS := 2000.0
## Distância (células) do caminho até o ponto do estreito.
const STRAIT_RADIUS := 6.0
## Qualidade do traço grosseiro: número de trechos sobre terra e o maior salto
## (células) tolerados. O refino segue as passagens de divisa, então o normal é
## zero; sobra um ou outro canto de 2-3 células numa costa recortada.
const MAX_GAPS := 4
const MAX_GAP_CELLS := 4.0

## Portos de cidade cuja água precisa ser o mar aberto: o navio só atraca se a
## célula do cais for do componente principal. nome, cais (lon, lat), semente
## de mar aberto em frente à costa (lon, lat).
const PORTS := [
	["GRC-HER", Vector2(25.133, 35.344), Vector2(24.6, 35.1)],
	# Algeciras (r7): o porto fica na Baía de Gibraltar, a ~1 km do centro; a
	# semente é o mar aberto do Estreito, a sul da baía.
	["ESP-ALG", Vector2(-5.441, 36.128), Vector2(-5.45, 35.95)],
	# Singapura (r9, 2026-11-17): a cidade-estado não tinha porto no cenário e
	# ficava sem rota marítima (única cidade da landmass `singapore`). O cais é
	# Tanjong Pagar, na costa sul voltada para o Estreito de Singapura; a semente
	# é o mar aberto a leste do estreito (mar da China Meridional).
	["SGP-SIN", Vector2(103.8544, 1.2626), Vector2(105.0, 1.3)],
	# r10 (recal3, 2026-11-17): o porto principal das 14 nações-ilha/costeiras
	# que estavam sem porto. Par (cais, semente de mar aberto) medido no
	# nav_mask: os dois no componente principal e o cais a ≤ 9,8 km de terra.
	["PHL-MAN", Vector2(120.9600, 14.5900), Vector2(119.6000, 14.5000)],
	["CUB-HAV", Vector2(-82.3700, 23.1450), Vector2(-82.6000, 23.3000)],
	["NZL-AUC", Vector2(174.7769, -36.8438), Vector2(175.1000, -36.6000)],
	["MDG-TOA", Vector2(49.4267, -18.1553), Vector2(49.9000, -18.0000)],
	["JAM-KIN", Vector2(-76.8000, 17.9500), Vector2(-76.5000, 17.7500)],
	["LKA-COL", Vector2(79.8447, 6.9528), Vector2(79.4000, 6.8000)],
	["BHS-NAS", Vector2(-77.3466, 25.0791), Vector2(-77.6000, 25.0000)],
	["FJI-SUV", Vector2(178.4250, -18.1350), Vector2(178.9000, -18.3000)],
	["TTO-POR", Vector2(-61.5200, 10.6600), Vector2(-61.0000, 10.7000)],
	["BRB-BRI", Vector2(-59.6131, 13.0969), Vector2(-60.0000, 13.1000)],
	["SLB-HON", Vector2(159.9556, -9.4319), Vector2(160.5000, -9.4000)],
	["BHR-MAN", Vector2(50.6200, 26.2000), Vector2(50.9000, 26.4000)],
	["QAT-DOH", Vector2(51.6020, 25.0150), Vector2(51.9000, 25.2000)],
	["BRN-BAN", Vector2(115.0705, 5.0242), Vector2(115.4000, 5.2000)],
]
## Distância máxima (células finas) do cais até a água do componente principal:
## o porto tem de alcançar o mar, não uma lagoa/enseada isolada. Medido em
## Heraklion: 1 célula (o cais é a célula de costa, como os demais portos).
const PORT_SNAP_CELLS := 8.0

## nome, origem (lon, lat), destino (lon, lat), ponto do estreito (lon, lat)
const ROUTES := [
	["Marseille→Bristol", Vector2(5.37, 43.30), Vector2(-2.59, 51.45),
			Vector2(-5.6, 35.95)],
	["Marseille→Saint-Nazaire", Vector2(5.37, 43.30), Vector2(-2.21, 47.27),
			Vector2(-5.6, 35.95)],
	["Pireu→Odessa", Vector2(23.63, 37.94), Vector2(30.73, 46.48),
			Vector2(29.0, 41.1)],
	["Hamburgo→Gdansk", Vector2(9.99, 53.55), Vector2(18.65, 54.35),
			Vector2(12.6, 55.7)],
]

var _failures: Array[String] = []


func _ready() -> void:
	print("=== nav_strait_test ===")
	var texture: Texture2D = load("res://map/map_data/nav_mask.png")
	var image := texture.get_image()
	if image.get_format() != Image.FORMAT_RGBA8:
		image.convert(Image.FORMAT_RGBA8)
	var nav = ConvoysScript.NavGrid.new()
	var started := Time.get_ticks_usec()
	nav.build(image)
	print("mascara %dx%d; grafo grosso %dx%d em %.1f ms" % [
		nav.w, nav.h, nav.cw, nav.ch, _ms(started)])

	_check_seas(nav)
	_check_ports(nav)
	for route in ROUTES:
		_check_route(nav, route)

	if _failures.is_empty():
		print("PASS: mares conectados, portos no mar aberto e rotas por Gibraltar, Dardanelos/Bósforo e Øresund")
		get_tree().quit(0)
		return
	for failure in _failures:
		push_error("[nav_strait] " + failure)
	get_tree().quit(1)


func _check_seas(nav) -> void:
	print("--- componentes de agua (main=%d)" % nav.main_water)
	var seas := [
		["Atlantico", -20.0, 45.0],
		["Mediterraneo", 20.0, 34.0],
		["Negro", 34.0, 43.0],
		["Baltico", 19.0, 58.0],
		["Vermelho", 38.0, 20.0],
		["Golfo Persico", 51.0, 27.5],
	]
	for sea in seas:
		var cell := _cell_of(Vector2(float(sea[1]), float(sea[2])))
		var comp: int = nav.component(cell, "ship")
		var ok: bool = comp >= 0 and comp == int(nav.main_water)
		print("  %-14s comp=%-4d %s" % [sea[0], comp, "ok" if ok else "SEPARADO"])
		if not ok:
			_failures.append("%s separado do oceano (comp %d, main %d)" % [
				sea[0], comp, nav.main_water])


## A água que o cais alcança é o MESMO componente do mar aberto em frente à
## costa? Compara com a semente de mar aberto e com o componente principal, e
## limita a distância do cais (porto não pode atracar numa lagoa distante).
func _check_ports(nav) -> void:
	print("--- agua nos portos (main=%d)" % nav.main_water)
	for entry in PORTS:
		var label: String = String(entry[0])
		var port_cell := _cell_of(entry[1])
		var sea_comp: int = nav.component(_cell_of(entry[2]), "ship")
		var snapped: Vector2i = nav.snap(port_cell, "ship")
		var comp: int = nav.component(snapped, "ship")
		var cells: float = Vector2(port_cell).distance_to(Vector2(snapped))
		var ok: bool = snapped.x >= 0 and sea_comp >= 0 and comp == sea_comp \
				and comp == int(nav.main_water) and cells <= PORT_SNAP_CELLS
		print("  %-8s comp=%-4d mar=%-4d cais->agua %.1f cel %s" % [
			label, comp, sea_comp, cells, "ok" if ok else "SEPARADO"])
		if not ok:
			_failures.append(
					("%s: agua do porto nao e o mar aberto (comp %d, mar %d, main %d, "
					+ "cais->agua %.1f celulas, teto %.0f)") % [
					label, comp, sea_comp, nav.main_water, cells, PORT_SNAP_CELLS])


func _check_route(nav, route: Array) -> void:
	var label: String = String(route[0])
	var from: Vector2i = nav.snap(_cell_of(route[1]), "ship")
	var to: Vector2i = nav.snap(_cell_of(route[2]), "ship")
	if from.x < 0 or to.x < 0:
		_failures.append("%s: snap falhou (from=%s to=%s)" % [label, from, to])
		return
	var started := Time.get_ticks_usec()
	var ok: bool = nav.begin(from, to, "ship")
	if not ok:
		_failures.append("%s: begin recusou a rota (componentes %d/%d)" % [
			label, nav.component(from, "ship"), nav.component(to, "ship")])
		return
	var slices := 0
	var max_slice := 0
	var frames := 0
	while nav.active:
		var deadline := Time.get_ticks_usec() + FRAME_BUDGET_US
		var slice_start := Time.get_ticks_usec()
		var status: int = nav.step(deadline)
		max_slice = maxi(max_slice, Time.get_ticks_usec() - slice_start)
		slices += 1
		if status == 0:
			frames += 1
			continue
		break
	var total_ms := _ms(started)
	var found: bool = nav.phase == 2 and nav.out.size() >= 2
	var through: bool = found and _passes_near(nav.out, route[3], STRAIT_RADIUS)
	# Qualidade do traço: o caminho bruto (antes da simplificação) não pode ter
	# saltos sobre terra — é o que o navio desenha no mapa.
	var gaps := 0
	var worst_gap := 0.0
	var gap_details: Array[String] = []
	var previous_raw := Vector2(-1.0, -1.0)
	for sample in nav.raw:
		if previous_raw.x >= 0.0 and not nav._line_walkable(previous_raw, sample, "ship"):
			gaps += 1
			worst_gap = maxf(worst_gap, previous_raw.distance_to(sample) / NAV_CELL_PX)
			gap_details.append("%s->%s" % [Vector2i(previous_raw / NAV_CELL_PX),
					Vector2i(sample / NAV_CELL_PX)])
		previous_raw = sample
	var route_state := "ok" if found else "SEM ROTA"
	var strait_state := "passa pelo estreito" if through else "NAO passa pelo estreito"
	print("  %-24s %-8s %d pontos, %d fatias, %d frames, %d ms, fatia max %.2f ms, %s, "
			% [label, route_state, nav.out.size(), slices, frames, int(total_ms),
			float(max_slice) / 1000.0, strait_state]
			+ "%d lacunas (maior %.0f celulas), tail=%s prefixo=%d" % [
			gaps, worst_gap, nav.tail, nav._prefix.size()])
	if gaps > MAX_GAPS or worst_gap > MAX_GAP_CELLS:
		_failures.append("%s: traco com %d lacunas (maior %.0f celulas) - teto %d/%.0f: %s"
				% [label, gaps, worst_gap, MAX_GAPS, MAX_GAP_CELLS,
				", ".join(gap_details)])
	if not found:
		_failures.append("%s: sem rota (visited=%d, phase=%d)" % [
			label, nav.visited, nav.phase])
	elif not through:
		_failures.append("%s: rota nao passa pelo estreito %s" % [label, route[3]])
	if max_slice > MAX_SLICE_US:
		_failures.append("%s: fatia de %.2f ms acima do teto (%.2f ms)" % [
			label, float(max_slice) / 1000.0, float(MAX_SLICE_US) / 1000.0])
	if total_ms > MAX_ROUTE_MS:
		_failures.append("%s: rota custou %.0f ms (teto %.0f ms)" % [
			label, total_ms, MAX_ROUTE_MS])


## Célula fina (espaço do nav_mask) de uma coordenada geográfica.
func _cell_of(point: Vector2) -> Vector2i:
	var size := Vector2(MASK_SIZE)
	var x: int = int(round((point.x + 180.0) / 360.0 * size.x - 0.5))
	var y: int = int(round((90.0 - point.y) / 180.0 * size.y - 0.5))
	return Vector2i(x, y)


## O caminho (em px de província) chega a `radius` células do ponto?
func _passes_near(path: PackedVector2Array, point: Vector2, radius: float) -> bool:
	var target := Vector2(_cell_of(point)) * NAV_CELL_PX
	for sample in path:
		if sample.distance_to(target) <= radius * NAV_CELL_PX:
			return true
	return false


func _lon_of(cell_x: int) -> float:
	return (float(cell_x) + 0.5) / float(MASK_SIZE.x) * 360.0 - 180.0


func _lat_of(cell_y: int) -> float:
	return 90.0 - (float(cell_y) + 0.5) / float(MASK_SIZE.y) * 180.0


func _ms(started_usec: int) -> float:
	return (Time.get_ticks_usec() - started_usec) / 1000.0
