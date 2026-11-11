extends Node3D
class_name CityWorld

## Mundo baseado em cidades com duas bases visuais:
##   - Modo "political" (atlas): fundo chapado com preenchimento opaco das
##     nações e fronteiras nítidas, estilo atlas histórico.
##   - Demais modos: base branca (terra marfim, mar branco, relevo suave e
##     contorno dos continentes) via paper_map.gdshader, para os mapas de
##     densidade/recurso desenharem por cima.
##
## A camada geopolítica (Natural Earth) fica por cima do terreno. Cidades 3D,
## rótulos, seleção e veículos funcionam em ambos os modos.
##
## Três níveis de representação, escolhidos pela distância da câmera:
##   local         — cidade orgânica com módulos 3D detalhados;
##   intermediário — a mesma cidade em volumes simplificados (silhueta igual);
##   continental   — marcador discreto e rótulo.
## A cidade é construída sob demanda e liberada ao afastar (histerese).

const MAP_SIZE_PX := Vector2(5000.0, 2500.0)
const PLANE_SIZE := Vector2(1000.0, 500.0)
const PIXEL_TO_WORLD := 0.2

const PAPER_FLAT_TEXTURE: Texture2D = preload("res://map/map_data/paper_flat.png")
const RELIEF_TEXTURE: Texture2D = preload("res://map/map_data/normal.png")
const MAP_SHADER: Shader = preload("res://map/map_shaders/paper_map.gdshader")
const ATLAS_SHADER: Shader = preload("res://map/map_shaders/atlas_political.gdshader")
const ATLAS_REGION_SDF: Texture2D = preload("res://map/map_data/atlas/region_sdf.png")
const ATLAS_REGION_COLOR: Texture2D = preload("res://map/map_data/atlas/region_color.png")
const ATLAS_WORLD_SDF: Texture2D = preload("res://map/map_data/atlas/world_sdf.png")
const ATLAS_WORLD_COLOR: Texture2D = preload("res://map/map_data/atlas/world_color.png")
const ATLAS_NATIONS_PATH := "res://map/map_data/atlas/nations.json"
## Cenário: usado para flags que o núcleo não devolve no status (capital).
const CITIES_PATH := "res://data/definitions/scenario/cities.json"
## Máscara terra/água (2500×1250) usada para achar o lado do mar de um porto.
const NAV_MASK_PATH := "res://map/map_data/nav_mask.png"
const ATLAS_REGION_PROVINCE: Texture2D = preload("res://map/map_data/atlas/region_province.png")
const ATLAS_WORLD_PROVINCE: Texture2D = preload("res://map/map_data/atlas/world_province.png")
const NATION_LABEL_FONT: Font = preload("res://assets/fonts/Cinzel-Variable.ttf")
const POLITICAL_SHADER: Shader = preload("res://map/map_shaders/political_overlay.gdshader")
const LABEL_FONT: Font = preload("res://assets/fonts/BarlowCondensed-SemiBold.ttf")

const ModeLayerScript: Script = preload("res://map/map_mode_layer.gd")
const UrbanGrowthScript: Script = preload("res://core/urban/urban_growth_manager.gd")
const IndustryProfileScript: Script = preload("res://core/urban/industry_profile.gd")
const TerritoryMapScript: Script = preload("res://map/territory_map.gd")
const GeographyScript: Script = preload("res://core/economy/geography_service.gd")
const PORT_SCENE: PackedScene = preload("res://assets/models/port.glb")
const PORT_SIZE := 0.6
const CITY_ICON: Texture2D = preload("res://assets/icons/places/city.png")
const STAR_ICON: Texture2D = preload("res://assets/icons/places/star.png")
const PORT_ICON: Texture2D = preload("res://assets/icons/places/port.png")
## Ícone de lugar: ao lado do nome, com a mesma tinta e altura proporcional ao
## texto — ícone + nome leem como um selo único ancorado na cidade.
const PLACE_ICON_SOURCE := 512.0
const PLACE_ICON_HEIGHT := 1.0  # altura do ícone em ems do rótulo
const PLACE_ICON_GAP := 0.3     # respiro entre ícone e início do texto, em ems
const BADGE_TEXTURE: Texture2D = preload("res://assets/icons/places/badge.png")
const RING_SCALE := 1.45        # selo ~45% maior que o ícone
const FLAG_SCALE := 0.6         # bandeira ~60% da altura do ícone
## Abaixo desta distância (u) o porto coincide com a cidade: a âncora vira um
## badge pequeno sobre o ícone da cidade, sem aro próprio.
const PORT_ICON_MERGE_DISTANCE := 0.5
## Âncora de porto: menor que o ícone de cidade, sem nome e sem bandeira.
const PORT_ICON_HEIGHT := 0.8
const PORT_BADGE_SCALE := 0.55

## Raio de captação de recursos (só no modo recursos): o anel no chão mostra
## até onde a cidade alcança; o detalhe fica na legenda ao clicar na cidade.
const MINERAL_KEYS := ["coal", "copper", "gas", "iron", "oil",
		"bauxite", "lead", "gold", "phosphate", "tungsten",
		"rare_earths", "sulfur", "diamond", "uranium"]
## Culturas renováveis no mesmo catálogo da economia (GeographyService
## .apply_to_sim): 6 culturas + madeira (cobertura florestal). A ficha da cidade
## lista as que a NAÇÃO alcança acima do limiar do investidor (MIN_SUITABILITY).
const RENEWABLE_CROPS := ["wheat", "rice", "maize", "potato", "soy", "cotton", "wood"]
const CROP_ACCESS_MIN := 0.30

const MARKER_COLOR := Color("#0A1212")
const MARKER_DOT := Color("#E8E2D0")

## Orçamento de renderização das cidades.
const MAX_DETAILED_CITIES := 40
## Layout de rótulos: sem linhas-guia. O nome fica ao lado do ícone, ancorado na
## cidade; quando dois nomes ainda colidem na tela, a cidade de menor população
## mostra só o ícone (o nome reaparece ao aproximar ou no hover). Portos são um
## ícone de âncora sem nome, no ponto de costa do ports.csv.
const LABEL_COLLIDE_PAD_PX := 3.0
## Deslocamento máximo, em pixels de tela, do ícone de porto para escapar do
## rótulo da própria cidade. O porto nunca "voa" para longe da costa.
const PORT_MAX_OFFSET_PX := 12.0

## Regras de LOD (unidades do mundo, 1 unidade ≈ 40 km no equador).
## Uma única representação da cidade, sem modelos concorrentes:
##   próximo        (< 34)       -> módulos 3D detalhados (UrbanRenderer)
##   intermediário  (34 .. 100)  -> a mesma cidade em caixas simplificadas
##   distante       (> 100)      -> marcador discreto + rótulo
## A cidade é construída a 96 u e liberada a 112 u (histerese); o marcador
## entra com fade só depois que a cidade já sumiu.
const URBAN_BUILD_DISTANCE := 96.0
const CITY_VISIBLE_DISTANCE := 100.0
const MARKER_FADE_IN_START := 100.0
const MARKER_FADE_IN_END := 112.0
## Escala da cidade no mapa. As cidades ficam em posições geográficas reais:
## Bruxelas–Amsterdã dista ~4,5 u, então o diâmetro precisa ficar bem abaixo
## disso para não encostar nas vizinhas.
const URBAN_SCALE := 0.165
## Raio (u do mundo) em volta de cada porto onde a cidade não constrói, para não
## sobrepor o modelo do porto.
const URBAN_PORT_KEEPOUT := 0.4
const LABEL_PIXEL_PER_UNIT := 0.0008
const LABEL_PIXEL_MIN := 0.006
const LABEL_PIXEL_MAX := 0.12

## Hierarquia de rótulos: texto pequeno e padronizado. Capital vem do CSV
## (coluna `capital`) e fica apenas ~2–3 px maior na tela; todo o resto usa o
## mesmo tamanho. Com a fonte Barlow Condensed, 20 px × pixel_size do LOD dá
## ~11–12 px de altura na tela no zoom padrão.
const LABEL_SIZE_CAPITAL := 24
const LABEL_SIZE_STANDARD := 20
const LABEL_COLOR := Color("#E8E2D0")
const LABEL_HOVER_COLOR := Color("#C9A24A")  # latão: hover/indica clicável
const ICON_COLOR := Color("#E8E2D0")          # marfim: alto contraste no selo escuro
## Fade de rótulo por distância (LOD): no zoom out ficam só capitais e cidades
## grandes; a transição é suave, sem linhas nem saltos.
const LABEL_FADE_CAPITAL_START := 170.0
const LABEL_FADE_CAPITAL_END := 260.0
const LABEL_FADE_LARGE_START := 100.0
const LABEL_FADE_LARGE_END := 160.0
const LABEL_FADE_MEDIUM_START := 75.0
const LABEL_FADE_MEDIUM_END := 110.0
const LABEL_FADE_SMALL_START := 58.0
const LABEL_FADE_SMALL_END := 90.0
const LABEL_LARGE_POPULATION := 1000000.0
const LABEL_MEDIUM_POPULATION := 500000.0

## Nomes de nação gravados no mapa.
const NATION_LABEL_COLOR := Color(0.07, 0.08, 0.09, 0.68)
const NATION_LABEL_NEAR := 34.0
const NATION_LABEL_NEAR_SMALL := 95.0
const NATION_LABEL_FADE := 18.0
const NATION_LABEL_SMALL_EXTENT := 12.0

const NATION_COLORS := {
	"GBR": Color("#8C3A3C"),
	"FRA": Color("#3C5A86"),
	"DEU": Color("#5E6668"),
	"NED": Color("#A1653A"),
	"BEL": Color("#A39244"),
}

var _content: Node3D
var _gps_mesh: MeshInstance3D
var _atlas_mesh: MeshInstance3D
var _political_layer: MeshInstance3D
var _atlas_material: ShaderMaterial
var _political_material: ShaderMaterial
var _mode_layer: Node3D
var _city_nodes: Array[Dictionary] = []
var _port_nodes: Array[Dictionary] = []
var _hovered_id := ""
var _mode := "political"
var _selection: MeshInstance3D
var _selection_dot: MeshInstance3D
var _hover: MeshInstance3D
var _selected_id := ""
var _nation_labels: Array[Label3D] = []
var _nation_anchors: Dictionary = {}

## Território dinâmico (províncias -> dono).
var _territory: TerritoryMap
var _territory_overrides: Dictionary = {}
var _geo: RefCounted
var _geo_catchment_ready := false
var _capital_by_id := {}
## Filtro de recursos da legenda: {} = todos; senão kind -> bool.
var _resource_filter: Dictionary = {}
var _last_cities: Array = []
var _minimap_texture: ImageTexture
## Layout de rótulos: métricas de tela do último frame (relatório do teste).
var _label_rects: Array[Dictionary] = []
var _nav_image: Image

## Emitido quando o conjunto de donos muda (o minimapa precisa se atualizar).
signal territory_changed


## Monta o mundo: chão de satélite, camada geopolítica e modelos de cidade.
func build(cities: Array, ports: Array) -> void:
	_load_city_flags()
	_content = Node3D.new()
	add_child(_content)

	_build_gps_plane()
	_build_political_layer()
	_build_mode_layer(cities)
	_build_cities(cities)
	_build_ports(ports)
	_build_nation_labels()
	_init_territory(cities)
	set_mode(_mode)

	# Cópias laterais: o mundo dá a volta em X (cilíndrico), então quem sai por
	# um lado reaparece no outro.
	for offset in [-PLANE_SIZE.x, PLANE_SIZE.x]:
		var copy := _content.duplicate() as Node3D
		copy.position = Vector3(offset, 0.0, 0.0)
		add_child(copy)


## Troca o modo de mapa sem reconstruir a geometria: só visibilidade e uniformes.
## O modo "political" usa base de atlas (chapado); os demais usam satélite.
func set_mode(mode: String) -> void:
	_mode = mode
	var is_atlas := mode == "political"
	var show_overlay := mode not in ["geographic", "political"]

	# Aplica em todas as cópias laterais (wrap), não só na central.
	for root in _content_roots():
		_set_child_visible(root, "MapBase", not is_atlas)
		_set_child_visible(root, "AtlasBase", is_atlas)
		# O atlas já desenha costa e fronteiras; o overlay serve aos modos de
		# calor, que continuam sobre o satélite com fronteiras discretas.
		_set_child_visible(root, "PoliticalLayer", show_overlay)
		var labels := root.get_node_or_null("NationLabels")
		if labels != null:
			labels.visible = is_atlas
	if _political_material != null:
		_political_material.set_shader_parameter("atlas_mode", false)
		_political_material.set_shader_parameter("fill_opacity", 0.028)
		_political_material.set_shader_parameter("border_opacity", 0.42)
	if _mode_layer != null:
		_mode_layer.set_mode(mode)


## Filtro da legenda de recursos: esconde marcadores do mapa.
func set_resource_filter(enabled: Dictionary) -> void:
	_resource_filter = enabled.duplicate()
	if _mode_layer != null:
		_mode_layer.set_resource_filter(_resource_filter)


func _content_roots() -> Array[Node3D]:
	var roots: Array[Node3D] = []
	for child in get_children():
		if child is Node3D and child.has_node("AtlasBase"):
			roots.append(child)
	return roots


static func _set_child_visible(root: Node, child_name: String, value: bool) -> void:
	var node := root.get_node_or_null(child_name) as Node3D
	if node != null:
		node.visible = value


func current_mode() -> String:
	return _mode


## Reaplica os valores dos modos quando a economia avança; sem reconstruir nada.
func refresh_modes(cities: Array) -> void:
	if _mode_layer != null:
		_mode_layer.refresh(cities)
	_refresh_territory(cities)


## Ponto (px do mapa) onde a câmera começa: MEDIANA das cidades, não a média —
## uma cidade isolada (Caiena) puxaria a média para o meio do Atlântico.
static func framing_center_px(cities: Array) -> Vector2:
	var xs: Array[float] = []
	var ys: Array[float] = []
	for city in cities:
		xs.append(float(city["x"]))
		ys.append(float(city["y"]))
	return Vector2(_median(xs), _median(ys))


static func _median(values: Array[float]) -> float:
	if values.is_empty():
		return 0.0
	values.sort()
	var mid := values.size() / 2
	if values.size() % 2 == 1:
		return values[mid]
	return (values[mid - 1] + values[mid]) * 0.5


func pixel_to_world(px: Vector2) -> Vector3:
	return Vector3(
		(px.x - MAP_SIZE_PX.x * 0.5) * PIXEL_TO_WORLD,
		0.0,
		(px.y - MAP_SIZE_PX.y * 0.5) * PIXEL_TO_WORLD,
	)


## Cidade sob o cursor considerando a ÁREA do corpo urbano, não só o centro.
## Sem física: projeta o centro e um ponto na borda (raio do footprint) para
## obter o raio em pixels; `radius_px` é o mínimo para cidades distantes.
func pick_city(screen_position: Vector2, camera: Camera3D, radius_px: float = 26.0) -> String:
	if camera == null:
		return ""
	var best_id := ""
	var best_score := 1.0
	var right := camera.global_transform.basis.x
	for record in _city_nodes:
		var world: Vector3 = to_global(record["position"])
		if camera.is_position_behind(world):
			continue
		var center := camera.unproject_position(world)
		var footprint := _city_footprint(String(record["id"]))
		var edge := camera.unproject_position(world + right * footprint)
		var city_radius := maxf(radius_px, center.distance_to(edge))
		var score := center.distance_to(screen_position) / city_radius
		score = minf(score, _label_pick_score(record, screen_position, camera, right))

		if score <= 1.0 and (best_id.is_empty() or score < best_score):
			best_score = score
			best_id = String(record["id"])
	return best_id


## Score normalizado do retângulo do rótulo (nome + bandeira/selo). INF se não
## houver rótulo visível. Clicar no nome seleciona a cidade/porto.
func _label_pick_score(record: Dictionary, screen_position: Vector2,
		camera: Camera3D, right: Vector3) -> float:
	var label: Label3D = record.get("label")
	if label == null or not label.visible:
		return INF
	var label_world := to_global(label.position)
	if camera.is_position_behind(label_world):
		return INF
	var em := float(label.font_size) * label.pixel_size
	var text_w := label.font.get_string_size(label.text,
			HORIZONTAL_ALIGNMENT_LEFT, -1, label.font_size).x * label.pixel_size
	var half := right * (text_w * 0.5 + em * 1.6)
	var a := camera.unproject_position(label_world - half)
	var b := camera.unproject_position(label_world + half)
	var top := camera.unproject_position(label_world + Vector3.UP * em * 0.7)
	var radius := maxf(20.0, a.distance_to(top))
	return _segment_distance(screen_position, a, b) / radius


## Distância de um ponto ao segmento AB (em 2D de tela).
static func _segment_distance(point: Vector2, a: Vector2, b: Vector2) -> float:
	var ab := b - a
	var length_squared := ab.length_squared()
	if length_squared <= 0.0001:
		return point.distance_to(a)
	var t := clampf((point - a).dot(ab) / length_squared, 0.0, 1.0)
	return point.distance_to(a + ab * t)


## Depósito sob o cursor no mapa de recursos; -1 se nenhum.
func pick_deposit(screen_position: Vector2, camera: Camera3D) -> int:
	if _mode != "resources" or _mode_layer == null:
		return -1
	return _mode_layer.pick_deposit(screen_position, camera)


## Dados de um depósito (para a legenda).
func deposit_info(index: int) -> Dictionary:
	if _mode_layer == null:
		return {}
	return _mode_layer.deposit_info(index)


## Badge "i" sobre o depósito sob o mouse (-1 esconde).
func set_deposit_hover(index: int) -> void:
	if _mode_layer != null:
		_mode_layer.set_deposit_hover(index)


## Anel persistente no depósito selecionado (-1 limpa).
func set_deposit_selected(index: int) -> void:
	if _mode_layer != null:
		_mode_layer.set_deposit_selected(index)


## Recursos que a cidade alcança (para a legenda): minérios no raio + trigo.
func city_resource_entries(city_id: String) -> Array:
	for record in _city_nodes:
		if String(record["id"]) == city_id:
			var city: Dictionary = record["city"]
			return _resource_entries(city_id, String(city.get("nation", "")), Vector2(
					float(city.get("x", 0.0)), float(city.get("y", 0.0))))
	return []


## Posição global da cidade (para centralizar a câmera).
func city_world_position(city_id: String) -> Vector3:
	for record in _city_nodes:
		if String(record["id"]) == city_id:
			return to_global(record["position"])
	return Vector3.ZERO


## Porto sob o cursor (como `pick_city`), por raio fixo de pixels.
func pick_port(screen_position: Vector2, camera: Camera3D, radius_px: float = 20.0) -> String:
	if camera == null:
		return ""
	var best_id := ""
	var best_score := 1.0
	var right := camera.global_transform.basis.x
	for record in _port_nodes:
		var world: Vector3 = to_global(record["position"])
		if camera.is_position_behind(world):
			continue
		var center := camera.unproject_position(world)
		var score := center.distance_to(screen_position) / radius_px
		score = minf(score, _label_pick_score(record, screen_position, camera, right))
		if score <= 1.0 and (best_id.is_empty() or score < best_score):
			best_score = score
			best_id = String(record["id"])
	return best_id


## Posição global do porto (para centralizar a câmera).
func port_world_position(port_id: String) -> Vector3:
	for record in _port_nodes:
		if String(record["id"]) == port_id:
			return to_global(record["position"])
	return Vector3.ZERO


## Nome do porto pelo id (para feedback de clique).
func port_name(port_id: String) -> String:
	for record in _port_nodes:
		if String(record["id"]) == port_id:
			return I18n.port_name(port_id, String(record["name"]))
	return ""


## Seleciona um porto (mesmo anel da cidade, no ponto do porto).
func select_port(port_id: String) -> void:
	_selected_id = port_id
	if _selection == null:
		_create_indicators()
	if _selection == null:
		return
	var active := not port_id.is_empty()
	_selection.visible = active
	_selection_dot.visible = active
	if active:
		var base: Vector3 = port_world_position(port_id) + Vector3(0.0, 0.15, 0.0)
		_selection.scale = Vector3(1.0, 1.0, 1.0)
		_selection.global_position = base
		_selection_dot.global_position = base + Vector3(0.0, 0.03, 0.0)


func selected_city() -> String:
	return _selected_id


## Seleção discreta: um anel fino rente ao chão + um ponto central pequeno.
## Não esconde edifícios nem atravessa o terreno (fica a ~0,15 u de altura).
func select_city(city_id: String) -> void:
	_selected_id = city_id
	_set_territory_highlight(city_id)
	if _selection == null:
		_create_indicators()
	if _selection == null:
		return
	var active := not city_id.is_empty()
	_selection.visible = active
	_selection_dot.visible = active
	if active:
		var footprint := _city_footprint(city_id)
		var base: Vector3 = city_world_position(city_id) + Vector3(0.0, 0.15, 0.0)
		_selection.scale = Vector3(footprint, 1.0, footprint)
		_selection.global_position = base
		_selection_dot.global_position = base + Vector3(0.0, 0.03, 0.0)


## Realça no mapa o território da nação da cidade selecionada (clareia as
## províncias donas no shader). Sem seleção, limpa o destaque.
func _set_territory_highlight(city_id: String) -> void:
	if _territory == null:
		return
	var index := -1
	if not city_id.is_empty():
		var nation := _nation_of_city(city_id)
		if not nation.is_empty():
			index = _territory.owner_index(nation)
	if _atlas_material != null:
		_atlas_material.set_shader_parameter("highlight_owner", index)
	if _political_material != null:
		_political_material.set_shader_parameter("highlight_owner", index)


func _nation_of_city(city_id: String) -> String:
	for city in _last_cities:
		if String(city.get("id", "")) == city_id:
			return String(city.get("nation", ""))
	return ""


## Realce de hover: versão ainda mais discreta, no mesmo ponto da seleção.
func set_hover(place_id: String) -> void:
	# Cor do nome + ícone: destaca a construção sob o cursor (indica clicável).
	if _hovered_id != place_id:
		_set_label_color(_hovered_id, false)
		_hovered_id = place_id
		_set_label_color(place_id, true)

	if _selection == null:
		_create_indicators()
	if _hover == null:
		return
	if place_id == _selected_id:
		_hover.visible = false
		return
	_hover.visible = not place_id.is_empty()
	if _hover.visible:
		var footprint := _city_footprint(place_id) if _is_city(place_id) else 1.0
		_hover.scale = Vector3(footprint, 1.0, footprint)
		_hover.global_position = _place_position(place_id) + Vector3(0.0, 0.15, 0.0)


## Muda a cor do nome e do ícone da construção (cidade ou porto). No hover, o
## nome vai para latão e o ícone também; fora, nome claro e ícone aço.
func _set_label_color(place_id: String, hovered: bool) -> void:
	if place_id.is_empty():
		return
	for record in _city_nodes:
		if String(record["id"]) == place_id:
			_tint_place(record, hovered)
			return
	for record in _port_nodes:
		if String(record["id"]) == place_id:
			_tint_place(record, hovered)
			return


func _tint_place(record: Dictionary, hovered: bool) -> void:
	var label_color := LABEL_HOVER_COLOR if hovered else LABEL_COLOR
	var icon_color := LABEL_HOVER_COLOR if hovered else ICON_COLOR
	var label: Label3D = record.get("label")
	if label != null:
		label.modulate = Color(label_color.r, label_color.g, label_color.b, label.modulate.a)
	var icon: Sprite3D = record.get("icon")
	if icon != null:
		icon.modulate = Color(icon_color.r, icon_color.g, icon_color.b, 1.0)


func _is_city(place_id: String) -> bool:
	for record in _city_nodes:
		if String(record["id"]) == place_id:
			return true
	return false


func _place_position(place_id: String) -> Vector3:
	for record in _city_nodes:
		if String(record["id"]) == place_id:
			return to_global(record["position"])
	for record in _port_nodes:
		if String(record["id"]) == place_id:
			return to_global(record["position"])
	return Vector3.ZERO


## Raio da cidade em unidades de mundo, para o anel de seleção abraçar o tecido.
func _city_footprint(city_id: String) -> float:
	for record in _city_nodes:
		if String(record["id"]) == city_id:
			var urban = record.get("urban")
			if urban != null and urban.has_method("extent"):
				return maxf(0.6, float(urban.extent()) * URBAN_SCALE + 0.18)
			break
	return 1.8


func _create_indicators() -> void:
	_selection = _make_ring("CitySelection", 1.0, 1.025, Color("#C9A24A"), 0.30)
	_selection.visible = false
	add_child(_selection)

	_selection_dot = _make_dot("CitySelectionDot", Color("#C9A24A"), 0.65)
	_selection_dot.visible = false
	add_child(_selection_dot)

	_hover = _make_ring("CityHover", 1.0, 1.025, Color("#E8E2D0"), 0.16)
	_hover.visible = false
	add_child(_hover)


func _make_ring(node_name: String, inner: float, outer: float,
		color: Color, alpha: float) -> MeshInstance3D:
	var torus := TorusMesh.new()
	torus.inner_radius = inner
	torus.outer_radius = outer
	torus.rings = 48
	torus.ring_segments = 6

	var ring_color := color
	ring_color.a = alpha

	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.albedo_color = ring_color

	var instance := MeshInstance3D.new()
	instance.name = node_name
	instance.mesh = torus
	instance.material_override = material
	# TorusMesh já nasce deitado no plano XZ; nada de girar (senão vira arco).
	instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	return instance


func _make_dot(node_name: String, color: Color, alpha: float) -> MeshInstance3D:
	var cylinder := CylinderMesh.new()
	cylinder.top_radius = 0.12
	cylinder.bottom_radius = 0.12
	cylinder.height = 0.02
	cylinder.radial_segments = 24

	var dot_color := color
	dot_color.a = alpha

	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.albedo_color = dot_color

	var instance := MeshInstance3D.new()
	instance.name = node_name
	instance.mesh = cylinder
	instance.material_override = material
	instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	return instance


## Flags do cenário que o núcleo não carrega no status: hoje, `capital`.
func _load_city_flags() -> void:
	_capital_by_id.clear()
	var data: Variant = JSON.parse_string(FileAccess.get_file_as_string(CITIES_PATH))
	if typeof(data) != TYPE_ARRAY:
		return
	for row in data:
		_capital_by_id[String(row.get("id", ""))] = bool(row.get("capital", false))


func _build_gps_plane() -> void:
	var plane := PlaneMesh.new()
	plane.size = PLANE_SIZE

	# Base branca (modos geográfico/densidade/recurso)
	var gps_material := ShaderMaterial.new()
	gps_material.shader = MAP_SHADER
	gps_material.set_shader_parameter("region_sdf", ATLAS_REGION_SDF)
	gps_material.set_shader_parameter("world_sdf", ATLAS_WORLD_SDF)
	gps_material.set_shader_parameter("relief_texture", RELIEF_TEXTURE)

	_gps_mesh = MeshInstance3D.new()
	_gps_mesh.name = "MapBase"
	_gps_mesh.mesh = plane
	_gps_mesh.material_override = gps_material
	_content.add_child(_gps_mesh)

	# Base de atlas (modo político): campos de distância do Natural Earth +
	# terreno do satélite; costa, fronteiras e províncias numa passada opaca.
	_atlas_material = ShaderMaterial.new()
	_atlas_material.shader = ATLAS_SHADER
	_atlas_material.set_shader_parameter("region_sdf", ATLAS_REGION_SDF)
	_atlas_material.set_shader_parameter("region_color", ATLAS_REGION_COLOR)
	_atlas_material.set_shader_parameter("world_sdf", ATLAS_WORLD_SDF)
	_atlas_material.set_shader_parameter("world_color", ATLAS_WORLD_COLOR)
	_atlas_material.set_shader_parameter("terrain_texture", PAPER_FLAT_TEXTURE)

	_atlas_mesh = MeshInstance3D.new()
	_atlas_mesh.name = "AtlasBase"
	_atlas_mesh.mesh = plane
	_atlas_mesh.material_override = _atlas_material
	_atlas_mesh.visible = false
	_content.add_child(_atlas_mesh)


func _build_political_layer() -> void:
	var plane := PlaneMesh.new()
	plane.size = PLANE_SIZE

	_political_material = ShaderMaterial.new()
	_political_material.shader = POLITICAL_SHADER
	# Overlay dos modos de calor lê os mesmos SDFs/cores do atlas político.
	_political_material.set_shader_parameter("region_sdf", ATLAS_REGION_SDF)
	_political_material.set_shader_parameter("region_color", ATLAS_REGION_COLOR)
	_political_material.set_shader_parameter("world_sdf", ATLAS_WORLD_SDF)
	_political_material.set_shader_parameter("world_color", ATLAS_WORLD_COLOR)

	_political_layer = MeshInstance3D.new()
	_political_layer.name = "PoliticalLayer"
	_political_layer.mesh = plane
	_political_layer.position = Vector3(0.0, 0.13, 0.0)
	_political_layer.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_political_layer.material_override = _political_material
	_content.add_child(_political_layer)


## Nomes das nações jogáveis gravados no chão, como numa carta. A âncora é o
## centroide da maior massa de terra de cada nação (tools/build_atlas_layers.py).
func _build_nation_labels() -> void:
	# Conecta antes de qualquer retorno: os rótulos de cidade também dependem
	# deste sinal para acompanhar o idioma.
	if not I18n.locale_changed.is_connected(_on_locale_changed):
		I18n.locale_changed.connect(_on_locale_changed)
	_nation_anchors.clear()
	if not FileAccess.file_exists(ATLAS_NATIONS_PATH):
		return
	var anchors: Variant = JSON.parse_string(FileAccess.get_file_as_string(ATLAS_NATIONS_PATH))
	if typeof(anchors) != TYPE_DICTIONARY:
		return
	_nation_anchors = anchors

	var font := FontVariation.new()
	font.base_font = NATION_LABEL_FONT
	font.variation_opentype = {"wght": 600}
	font.spacing_glyph = 10

	var holder := Node3D.new()
	holder.name = "NationLabels"
	_content.add_child(holder)
	for tag: Variant in anchors:
		var label := Label3D.new()
		label.name = "Nation_" + String(tag)
		label.set_meta("tag", String(tag))
		label.text = I18n.t("nation." + String(tag)).to_upper()
		label.font = font
		label.font_size = 64
		label.outline_size = 0
		label.modulate = NATION_LABEL_COLOR
		label.billboard = BaseMaterial3D.BILLBOARD_DISABLED
		label.double_sided = false
		label.alpha_cut = Label3D.ALPHA_CUT_DISABLED
		label.render_priority = 2
		label.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
		holder.add_child(label)
		_place_nation_label(label)


func _on_locale_changed(_locale: String) -> void:
	for root in _content_roots():
		var holder := root.get_node_or_null("NationLabels")
		if holder != null:
			for label in holder.get_children():
				(label as Label3D).text = I18n.t("nation." + String(label.get_meta("tag"))).to_upper()
				_fit_nation_label(label)
		# Nomes das cidades, incluindo as cópias laterais do mundo cilíndrico.
		for node in root.find_children("*", "Label3D", true, false):
			var label := node as Label3D
			if label.has_meta("city_id"):
				label.text = I18n.city_name(
						String(label.get_meta("city_id")),
						String(label.get_meta("city_fallback"))).to_upper()
			elif label.has_meta("port_id"):
				label.text = I18n.port_name(
						String(label.get_meta("port_id")),
						String(label.get_meta("port_fallback"))).to_upper()


## Capitulares proporcionais ao país, mas o nome nunca passa de 85% da largura
## dele (nomes longos como "PAÍSES BAJOS" encolhem em vez de vazar).
static func _fit_nation_label(label: Label3D) -> void:
	var extent := float(label.get_meta("extent"))
	var pixel_size := clampf(extent * 0.11, 0.9, 2.4) / 45.0
	var width := label.font.get_string_size(label.text, HORIZONTAL_ALIGNMENT_LEFT,
			-1, label.font_size).x
	if width > 0.0:
		pixel_size = minf(pixel_size, extent * 0.85 / width)
	label.pixel_size = pixel_size


## Posiciona/redimensiona um rótulo de nação: com território original usa a
## âncora do nations.json; senão usa a âncora do território atual. Sem
## território, esconde (meta "has_territory" = false).
func _place_nation_label(label: Label3D) -> void:
	var tag := String(label.get_meta("tag"))
	var anchor: Dictionary = {}
	var extent_px := 40.0
	if _territory != null and not _territory.is_original(tag):
		anchor = _territory.nation_anchor(tag)
		if anchor.is_empty():
			label.set_meta("has_territory", false)
			label.visible = false
			return
		extent_px = float(anchor.get("extent_px", 40.0))
	elif _nation_anchors.has(tag):
		anchor = _nation_anchors[tag]
		extent_px = float(anchor.get("extent_px", 40.0))
	else:
		label.set_meta("has_territory", false)
		label.visible = false
		return

	label.set_meta("has_territory", true)
	var extent := extent_px * PIXEL_TO_WORLD
	label.set_meta("extent", extent)
	label.set_meta("min_distance", NATION_LABEL_NEAR if extent >= NATION_LABEL_SMALL_EXTENT
			else NATION_LABEL_NEAR_SMALL)
	label.position = pixel_to_world(Vector2(float(anchor["x"]), float(anchor["y"]))) \
			+ Vector3(0.0, 0.2, 0.0)
	_fit_nation_label(label)


## Reaplica âncora/extensão em todas as cópias laterais (wrap cilíndrico).
func _reposition_nation_labels() -> void:
	for root in _content_roots():
		var holder := root.get_node_or_null("NationLabels") as Node3D
		if holder == null:
			continue
		for child in holder.get_children():
			_place_nation_label(child as Label3D)


## --- Território dinâmico ---------------------------------------------------

## Acesso ao TerritoryMap (para teste/debug).
func territory() -> TerritoryMap:
	return _territory


func _init_territory(cities: Array) -> void:
	_last_cities = cities.duplicate()
	_territory = TerritoryMapScript.new()
	_territory.load_data()
	_territory.assign(cities)
	var changed: bool = _territory.update(cities, _territory_overrides)
	_apply_territory_materials()
	if changed:
		_reposition_nation_labels()


func _refresh_territory(cities: Array) -> void:
	if _territory == null:
		return
	_last_cities = cities.duplicate()
	var changed: bool = _territory.update(cities, _territory_overrides)
	if changed:
		_minimap_texture = null  # invalida: o minimapa regenera sob demanda
		_reposition_nation_labels()
		_update_city_nation_colors(cities)
		territory_changed.emit()


## Textura do minimapa (cor do dono por província), gerada e cacheada sob
## demanda. Recalculada quando os donos mudam (territory_changed invalida).
func minimap_texture() -> ImageTexture:
	if _territory == null:
		return null
	if _minimap_texture == null:
		_minimap_texture = _territory.minimap_texture()
	return _minimap_texture


## Quando o dono de uma cidade muda (override de território), o marcador passa
## a usar a cor da nação efetiva, não a original. O rótulo já é neutro (#E8E2D0).
func _update_city_nation_colors(cities: Array) -> void:
	for city in cities:
		var city_id := String(city.get("id", ""))
		var tag := String(_territory_overrides.get(city_id, city.get("nation", "")))
		for node in _city_nodes:
			if String(node.get("id", "")) != city_id:
				continue
			var color: Color = NATION_COLORS.get(tag, Color.WHITE)
			var discs: Array = node.get("discs", [])
			if discs.size() > 1:
				var mat: StandardMaterial3D = (discs[1] as MeshInstance3D).material_override
				if mat != null:
					mat.albedo_color = color
			break


func _apply_territory_materials() -> void:
	if _territory == null:
		return
	var owner: Texture2D = _territory.owner_texture()
	if _atlas_material != null:
		_atlas_material.set_shader_parameter("region_province", ATLAS_REGION_PROVINCE)
		_atlas_material.set_shader_parameter("world_province", ATLAS_WORLD_PROVINCE)
		_atlas_material.set_shader_parameter("province_owner", owner)
		_atlas_material.set_shader_parameter("dynamic_territory", true)
	if _political_material != null:
		_political_material.set_shader_parameter("region_province", ATLAS_REGION_PROVINCE)
		_political_material.set_shader_parameter("world_province", ATLAS_WORLD_PROVINCE)
		_political_material.set_shader_parameter("province_owner", owner)
		_political_material.set_shader_parameter("dynamic_territory", true)


## Debug: força a nação de uma cidade (override de território).
func debug_set_city_nation(city_id: String, tag: String) -> void:
	_territory_overrides[city_id] = tag
	if _territory != null:
		_refresh_territory(_last_cities)


## Debug: limpa todos os overrides de território.
func debug_clear_territory_overrides() -> void:
	_territory_overrides.clear()
	if _territory != null:
		_refresh_territory(_last_cities)


## Nomes de nação entram ao afastar e somem perto, para não disputar com as
## cidades; nações pequenas (Benelux) só aparecem quando os nomes de cidade saem.
func _update_nation_labels(camera_position: Vector3) -> void:
	for root in _content_roots():
		var holder := root.get_node_or_null("NationLabels") as Node3D
		if holder == null or not holder.visible:
			continue
		for child in holder.get_children():
			var label := child as Label3D
			# Nação sem território (override moveu tudo) não mostra o nome.
			if label.has_meta("has_territory") and not bool(label.get_meta("has_territory")):
				label.visible = false
				continue
			var near := float(label.get_meta("min_distance"))
			var distance := camera_position.distance_to(label.global_position)
			var alpha := _fade_in(distance, near, near + NATION_LABEL_FADE)
			label.visible = alpha > 0.01
			label.modulate.a = NATION_LABEL_COLOR.a * alpha


func _build_mode_layer(cities: Array) -> void:
	_mode_layer = ModeLayerScript.new()
	_mode_layer.name = "ModeLayer"
	add_child(_mode_layer)
	_mode_layer.setup(cities)


func _build_cities(cities: Array) -> void:
	# Prioriza as maiores cidades para o detalhe 3D; o resto usa MultiMesh.
	var ordered := cities.duplicate()
	ordered.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("population", 0.0)) > float(b.get("population", 0.0))
	)
	var detailed := mini(MAX_DETAILED_CITIES, ordered.size())
	for index in detailed:
		var city: Dictionary = ordered[index]
		var position: Vector3 = pixel_to_world(Vector2(city["x"], city["y"]))
		var color: Color = NATION_COLORS.get(city["nation"], Color.WHITE)
		var city_id := String(city.get("id", ""))
		var city_name := String(city.get("name", ""))
		var discs := _spawn_city_marker(position, city_name, color)
		var capital := bool(_capital_by_id.get(city_id, false))
		var label := _add_city_label_hierarchical(position,
				I18n.city_name(city_id, city_name), color,
				capital, true)
		var icon: Sprite3D = null
		var ring: Sprite3D = null
		var flag: Sprite3D = null
		if label != null:
			# Guarda a chave/fallback para atualizar o texto ao trocar idioma.
			label.set_meta("city_id", city_id)
			label.set_meta("city_fallback", city_name)
			var icon_texture := STAR_ICON if capital else CITY_ICON
			# Bandeira só na capital: nas cidades comuns ela alargava o rótulo
			# e forçava o nome a sair de cima das vizinhas.
			var marker := _add_place_icon(10, icon_texture, String(city["nation"]), capital)
			icon = marker["icon"]
			ring = marker["ring"]
			flag = marker["flag"]
		_city_nodes.append({
			"id": city_id,
			"kind": "city",
			"city": city,
			"position": position,
			"anchor": position + Vector3(0.0, 1.0, 0.0),
			"ground": position,
			"priority": float(city.get("population", 0.0)),
			"capital": capital,
			"discs": discs,
			"label": label,
			"label_alpha": 1.0,
			"icon": icon,
			"ring": ring,
			"flag": flag,
			"urban": null,
		})

	if ordered.size() > detailed:
		_build_city_multimesh(ordered.slice(detailed))


func _build_city_multimesh(cities: Array) -> void:
	if cities.is_empty():
		return
	var mesh := CylinderMesh.new()
	mesh.top_radius = 0.55
	mesh.bottom_radius = 0.55
	mesh.height = 0.08
	mesh.radial_segments = 12

	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.vertex_color_use_as_albedo = true

	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.use_colors = true
	multimesh.mesh = mesh
	multimesh.instance_count = cities.size()

	var basis := Basis()
	for index in cities.size():
		var city: Dictionary = cities[index]
		var position: Vector3 = pixel_to_world(Vector2(city["x"], city["y"]))
		multimesh.set_instance_transform(index,
			Transform3D(basis, position + Vector3(0.0, 0.16, 0.0)))
		multimesh.set_instance_color(index,
			NATION_COLORS.get(city["nation"], Color.WHITE))

	var instance := MultiMeshInstance3D.new()
	instance.multimesh = multimesh
	instance.material_override = material
	_content.add_child(instance)


## Portos como entidades 3D: modelo port.glb (sem disco de fundo, como a cidade).
## O selo do porto fica SEMPRE na coordenada real de costa do ports.csv. Quando o
## porto é o da própria cidade (mesmo id OU mesmo nome), ele não repete o nome
## nem a bandeira (uma bandeira por cidade) e o layout o desloca para o lado do
## mar, com distância mínima do rótulo da cidade.
func _build_ports(ports: Array) -> void:
	var city_by_id := {}
	var city_by_name := {}
	for record in _city_nodes:
		if record.get("label") == null:
			continue
		city_by_id[String(record.get("id", ""))] = record
		var city: Dictionary = record.get("city", {})
		city_by_name[String(city.get("name", "")).to_lower()] = record
	for port: Dictionary in ports:
		var port_px := Vector2(float(port["x"]), float(port["y"]))
		var pos := pixel_to_world(port_px)

		var model := PORT_SCENE.instantiate() as Node3D
		_content.add_child(model)
		model.position = pos
		var bounds := _combined_aabb(model)
		var longest := maxf(bounds.size.x, maxf(bounds.size.y, bounds.size.z))
		if longest > 0.0:
			model.scale = Vector3.ONE * (PORT_SIZE / longest)

		var port_id := String(port["id"])
		var port_name := String(port["name"])
		var city_record: Dictionary = city_by_id.get(port_id, {})
		if city_record.is_empty():
			city_record = city_by_name.get(port_name.to_lower(), {})
		var same_city := false
		if not city_record.is_empty():
			var city: Dictionary = city_record.get("city", {})
			var city_name := String(city.get("name", "")).to_lower()
			var distance := pos.distance_to(city_record["position"])
			same_city = city_name == port_name.to_lower() \
					or distance <= PORT_ICON_MERGE_DISTANCE

		# Porto: âncora pequena no ponto de costa, SEM nome e SEM bandeira.
		# Quando coincide com a própria cidade, vira um badge sobre o ícone dela.
		var fused := same_city and not city_record.is_empty() \
				and pos.distance_to(city_record["position"]) <= PORT_ICON_MERGE_DISTANCE
		var marker := _add_port_marker(PORT_ICON)

		# Lado do mar: da cidade para o ponto de costa; se coincidirem, procura
		# a água mais próxima na máscara terra/água.
		var sea_dir := Vector2(1.0, 0.0)
		if not city_record.is_empty():
			var city: Dictionary = city_record.get("city", {})
			var city_px := Vector2(float(city.get("x", port_px.x)),
					float(city.get("y", port_px.y)))
			var delta := port_px - city_px
			sea_dir = delta.normalized() if delta.length() >= 0.75 \
					else _coast_direction_map(city_px)
		else:
			sea_dir = _coast_direction_map(port_px)

		_port_nodes.append({
			"id": port_id,
			"kind": "port",
			"name": port_name,
			"nation": String(port["nation"]),
			"position": pos,
			"anchor": pos + Vector3(0.0, 0.8, 0.0),
			"marker_base": pos + Vector3(0.0, 0.8, 0.0),
			"ground": pos,
			"priority": 0.0,
			"same_city": same_city,
			"fused": fused,
			"city_id": String(city_record.get("id", "")),
			"sea_dir_world": Vector3(sea_dir.x, 0.0, sea_dir.y),
			"label": null,
			"label_alpha": 1.0,
			"pixel_size": LABEL_PIXEL_PER_UNIT * 52.0,
			"icon": marker["icon"],
			"ring": marker["ring"],
			"flag": null,
		})


## Direção (em px do mapa, x/lon e y/lat) da água mais próxima de um ponto.
## Usa a máscara terra/água já existente; serve para deslocar o selo de um porto
## para o lado do mar quando o ponto de costa coincide com o centro da cidade.
func _coast_direction_map(point_px: Vector2) -> Vector2:
	if _nav_image == null:
		var texture := load(NAV_MASK_PATH) as Texture2D
		if texture != null:
			_nav_image = texture.get_image()
	if _nav_image == null:
		return Vector2(1.0, 0.0)
	var scale := Vector2(
			float(_nav_image.get_width()) / MAP_SIZE_PX.x,
			float(_nav_image.get_height()) / MAP_SIZE_PX.y)
	var center := point_px * scale
	for radius_px in [3.0, 6.0, 12.0, 24.0, 48.0]:
		var radius: float = float(radius_px) * scale.x
		for step in 32:
			var angle := TAU * float(step) / 32.0
			var dir := Vector2(cos(angle), sin(angle))
			var sample: Vector2 = center + dir * radius
			var x := int(round(sample.x))
			var y := int(round(sample.y))
			if x < 0 or y < 0 or x >= _nav_image.get_width() \
					or y >= _nav_image.get_height():
				continue
			if _nav_image.get_pixel(x, y).r < 0.5:
				return dir
	return Vector2(1.0, 0.0)


## Marcador de lugar (cidade): símbolo dentro de um selo escuro com aro de
## latão (+ bandeira só na capital), ancorado no ponto da cidade.
func _add_place_icon(render_priority: int, texture: Texture2D, nation: String,
		with_flag: bool) -> Dictionary:
	var icon := Sprite3D.new()
	icon.texture = texture
	icon.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	icon.transparent = true
	icon.modulate = ICON_COLOR
	icon.no_depth_test = true
	icon.render_priority = render_priority - 1
	_content.add_child(icon)

	var ring := Sprite3D.new()
	ring.texture = BADGE_TEXTURE
	ring.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	ring.transparent = true
	ring.no_depth_test = true
	ring.render_priority = render_priority - 2
	_content.add_child(ring)

	var flag: Sprite3D = _new_flag(nation) if with_flag else null

	return {"icon": icon, "ring": ring, "flag": flag}


## Âncora de porto: só o ícone (sem nome, sem bandeira). O aro fino pode ser
## escondido quando a âncora vira badge do ícone da cidade.
func _add_port_marker(texture: Texture2D) -> Dictionary:
	var icon := Sprite3D.new()
	icon.texture = texture
	icon.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	icon.transparent = true
	icon.modulate = ICON_COLOR
	icon.no_depth_test = true
	icon.render_priority = 11
	_content.add_child(icon)

	var ring := Sprite3D.new()
	ring.texture = BADGE_TEXTURE
	ring.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	ring.transparent = true
	ring.no_depth_test = true
	ring.render_priority = 10
	_content.add_child(ring)

	return {"icon": icon, "ring": ring}


func _new_flag(nation: String) -> Sprite3D:
	var texture := Economy.flag_texture(nation)
	if texture == null:
		return null
	var flag := Sprite3D.new()
	flag.texture = texture
	flag.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	flag.no_depth_test = true
	flag.render_priority = 10
	_content.add_child(flag)
	return flag


## Dispõe o marcador de cidade: [bandeira só capital] [aro+ícone] [nome].
## O ícone fica exatamente na âncora da cidade; o nome cresce para a direita.
func _layout_city_marker(record: Dictionary) -> void:
	var label: Label3D = record.get("label")
	if label == null:
		return
	var anchor: Vector3 = record["anchor"]
	var em := float(label.font_size) * label.pixel_size
	var icon_h := em * PLACE_ICON_HEIGHT
	var ring_h := icon_h * RING_SCALE
	var gap := em * PLACE_ICON_GAP

	var icon: Sprite3D = record.get("icon")
	if icon != null:
		icon.pixel_size = icon_h / PLACE_ICON_SOURCE
		icon.position = anchor

	var ring: Sprite3D = record.get("ring")
	if ring != null:
		ring.pixel_size = ring_h / PLACE_ICON_SOURCE
		ring.position = anchor

	var flag: Sprite3D = record.get("flag")
	if flag != null:
		var flag_h := icon_h * FLAG_SCALE
		flag.pixel_size = flag_h / 20.0  # bandeira 40×20 (2:1)
		flag.position = anchor + Vector3(-ring_h * 0.5 - gap - flag_h, 0.0, 0.0)

	var text_w := label.font.get_string_size(
			label.text, HORIZONTAL_ALIGNMENT_LEFT, -1, label.font_size).x * label.pixel_size
	label.position = anchor + Vector3(ring_h * 0.5 + gap + text_w * 0.5, 0.0, 0.0)


## Dispõe o ícone de porto no ponto de costa. O badge (porto que coincide com a
## própria cidade) é menor e não desenha aro, para não competir com o selo dela.
func _layout_port_marker(record: Dictionary) -> void:
	var icon: Sprite3D = record.get("icon")
	if icon == null:
		return
	var pixel_size := float(record.get("pixel_size", LABEL_PIXEL_PER_UNIT * 52.0))
	var fused := bool(record.get("fused", false))
	var em := LABEL_SIZE_STANDARD * pixel_size
	var icon_h := em * (PORT_BADGE_SCALE if fused else PORT_ICON_HEIGHT)
	icon.pixel_size = icon_h / PLACE_ICON_SOURCE
	icon.position = record["marker_base"]

	var ring: Sprite3D = record.get("ring")
	if ring != null:
		if fused:
			ring.visible = false
		else:
			ring.pixel_size = icon_h * RING_SCALE / PLACE_ICON_SOURCE
			ring.position = icon.position


## Recursos PRÓPRIOS da cidade para a legenda: jazidas minerais que pertencem
## a ela (regra A1 — cada jazida tem um único dono) + as culturas da sua bacia
## acima do limiar do investidor (0,30), ordenadas por aptidão decrescente.
## Antes mostrava as jazidas da NAÇÃO inteira: toda cidade da mesma nação
## listava os mesmos minerais ("acesso aos mesmos depósitos"). Sem duplicar:
## minerais e culturas vêm de catálogos distintos.
func _resource_entries(city_id: String, nation: String, center: Vector2) -> Array:
	if _geo == null:
		_geo = GeographyScript.new()
		_geo.load_data()
	_ensure_catchment()
	var code := String(_geo.NATION_CODE.get(nation, ""))
	var minerals: Dictionary = _geo.city_mineral_reserves(city_id) if code != "" \
			else _legacy_territory_reserves(city_id, center)

	var entries: Array = []
	for key in MINERAL_KEYS:
		if minerals.has(key):
			entries.append({
				"kind": key,
				"count": int(minerals[key]["count"]),
				"reserve_kt": float(minerals[key]["reserve_kt"]),
			})
	var crops: Array = []
	for crop in RENEWABLE_CROPS:
		var value := float(_renewable_value(crop, code, city_id, center))
		if value >= CROP_ACCESS_MIN:
			crops.append({"kind": crop, "count": 1, "value": value})
	crops.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		var av := float(a["value"])
		var bv := float(b["value"])
		if not is_equal_approx(av, bv):
			return av > bv
		return RENEWABLE_CROPS.find(String(a["kind"])) < RENEWABLE_CROPS.find(String(b["kind"])))
	entries.append_array(crops)
	return entries


## Bacia de captação por cidade para a ficha (uma vez só). A ficha mostra as
## culturas DA CIDADE (média ponderada dos seus territórios) e os minerais que
## ela POSSUI (regra A1) — nada de máximo/acesso nacional.
func _ensure_catchment() -> void:
	if _geo == null or _geo_catchment_ready:
		return
	var city_list: Array = []
	for record in _city_nodes:
		var city: Dictionary = record["city"]
		city_list.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
			"landmass": String(city.get("landmass", "")),
		})
	_geo.assign_provinces(city_list)
	_geo.assign_deposits(city_list)
	_geo_catchment_ready = true


## Fallback legado (cidade sem nação conhecida): reservas do território. Não é
## usado pelas cidades do cenário — todas têm nação —, mas mantém a ficha
## funcional para ids de demonstração.
func _legacy_territory_reserves(city_id: String, center: Vector2) -> Dictionary:
	var out := {}
	for deposit in _geo.deposits_for(city_id, center):
		var resource := String(deposit.get("resource", ""))
		var qty := float(deposit.get("quantity_kt", 0.0))
		if resource == "" or qty <= 0.0:
			continue
		if not out.has(resource):
			out[resource] = {"count": 0, "reserve_kt": 0.0}
		out[resource]["count"] = int(out[resource]["count"]) + 1
		out[resource]["reserve_kt"] = float(out[resource]["reserve_kt"]) + qty
	return out


## Aptidão de uma cultura renovável PARA A CIDADE (bacia de captação). A madeira
## usa a cobertura florestal da bacia, não a aptidão agrícola. Cidade sem nação
## conhecida mantém o fallback do território mais próximo (legado).
func _renewable_value(crop: String, code: String, city_id: String, center: Vector2) -> float:
	if _geo == null:
		return 0.0
	if crop == "wood":
		if code != "":
			return _geo.city_forest_cover(city_id)
		return _geo.forest_cover(city_id, center)
	if code != "":
		return _geo.city_crop_suitability(city_id, crop)
	return _geo.crop_suitability(city_id, center, crop)



## AABB combinada das malhas do modelo, em espaço local da raiz.
static func _combined_aabb(root: Node3D) -> AABB:
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


## Marcador estratégico em camadas; devolve os discos para o controle de LOD.
func _spawn_city_marker(position: Vector3, _city_name: String, color: Color) -> Array:
	var discs: Array = []
	discs.append(_add_disc(position + Vector3(0.0, 0.14, 0.0), 0.85, 0.10, MARKER_COLOR))
	discs.append(_add_disc(position + Vector3(0.0, 0.20, 0.0), 0.57, 0.12, color))
	discs.append(_add_disc(position + Vector3(0.0, 0.27, 0.0), 0.19, 0.10, MARKER_DOT))
	return discs


## Disco plano usado para desenhar o marcador da cidade em camadas.
func _add_disc(position: Vector3, radius: float, height: float, color: Color) -> MeshInstance3D:
	var mesh := CylinderMesh.new()
	mesh.top_radius = radius
	mesh.bottom_radius = radius
	mesh.height = height
	mesh.radial_segments = 24

	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED

	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.material_override = material
	instance.position = position
	_content.add_child(instance)
	return instance


## Nome da cidade em caixa alta, como legenda de carta militar.
## Hierarquia por produção: capitais maiores, cidades menores (estilo HOI3).
func _add_city_label_hierarchical(position: Vector3, city_name: String, color: Color,
		capital: bool, with_label: bool) -> Label3D:
	if not with_label:
		return null

	# Tamanho padronizado; só capital (CSV) ganha um pouco mais.
	var font_size := LABEL_SIZE_CAPITAL if capital else LABEL_SIZE_STANDARD
	
	var label := Label3D.new()
	label.text = city_name.to_upper()
	label.font = LABEL_FONT
	label.font_size = font_size
	label.pixel_size = 0.019
	# Texto claro com halo escuro: lê sobre qualquer preenchimento de nação e
	# não se confunde com a cor do país. `color` fica só como dica de identidade.
	label.modulate = Color("#E8E2D0")
	label.outline_modulate = Color(0.04, 0.055, 0.055, 0.95)
	label.outline_size = int(font_size * 0.4)  # Outline proporcional
	label.no_depth_test = true
	label.render_priority = 10
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.position = position + Vector3(0.0, 1.0, 0.0)
	_content.add_child(label)
	return label


## LOD por distância: rótulos de cidade/porto, marcador e cidade modular.
func _process(_delta: float) -> void:
	if _city_nodes.is_empty() and _port_nodes.is_empty():
		return
	var camera := get_viewport().get_camera_3d()
	if camera == null:
		return

	var camera_position := camera.global_position
	_update_nation_labels(camera_position)
	# Portos: âncora pequena, sem nome, que some cedo no zoom out.
	for record in _port_nodes:
		var port_position: Vector3 = to_global(record["position"])
		_apply_port_lod(record, camera_position.distance_to(port_position))
	for record in _city_nodes:
		var world_position: Vector3 = to_global(record["position"])
		var distance := camera_position.distance_to(world_position)
		_apply_city_lod(record, distance, world_position)
	# Layout simples: âncora fixa, sem linhas-guia. Nomes que ainda colidem dão
	# lugar ao ícone da cidade menor (prioridade por população).
	_layout_place_labels(camera)


## Layout dos rótulos na tela, SEM deslocamento de grupos e SEM linhas-guia:
##  - cidades em ordem de prioridade (capitais, depois população); se o nome
##    ainda colidir com um rótulo/ícone já aceito, a cidade menor fica só com o
##    ícone (o nome reaparece ao aproximar ou no hover);
##  - portos são ícones de âncora no ponto de costa, com ajuste local de no
##    máximo `PORT_MAX_OFFSET_PX` para não entrar no rótulo da própria cidade.
func _layout_place_labels(camera: Camera3D) -> void:
	_label_rects.clear()
	var entries: Array = []
	for record in _city_nodes:
		if record.get("label") == null:
			continue
		if float(record.get("label_alpha", 0.0)) <= 0.01:
			continue
		_layout_city_marker(record)
		var metrics := _city_metrics(record, camera)
		if metrics.is_empty():
			continue
		entries.append({
			"record": record,
			"metrics": metrics,
			"capital": bool(record.get("capital", false)),
			"priority": float(record.get("priority", 0.0)),
		})
	entries.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		if bool(a["capital"]) != bool(b["capital"]):
			return bool(a["capital"])
		return float(a["priority"]) > float(b["priority"])
	)

	var kept: Array[Rect2] = []
	for entry in entries:
		var record: Dictionary = entry["record"]
		var metrics: Dictionary = entry["metrics"]
		var label: Label3D = record["label"]
		var text_allowed := true
		if not bool(entry["capital"]) and String(record["id"]) != _hovered_id:
			var text_rect: Rect2 = metrics["text_rect"]
			for other in kept:
				if text_rect.grow(LABEL_COLLIDE_PAD_PX).intersects(other):
					text_allowed = false
					break
		label.visible = text_allowed
		var block: Rect2 = metrics["icon_rect"]
		if text_allowed:
			block = block.merge(metrics["text_rect"])
		kept.append(block)
		var icon: Sprite3D = record.get("icon")
		var flag: Sprite3D = record.get("flag")
		_label_rects.append({
			"id": String(record.get("id", "")),
			"kind": "city",
			"rect": block,
			"text_rect": metrics["text_rect"],
			"icon_rect": metrics["icon_rect"],
			"text_visible": text_allowed,
			"marker_visible": icon != null and icon.visible,
			"flag_visible": flag != null and flag.visible,
			"capital": bool(record.get("capital", false)),
			"population": float(record.get("priority", 0.0)),
			"same_city": false,
			"fused": false,
			"city_id": "",
			"offset": Vector2.ZERO,
		})

	# Portos por último: nunca deslocam rótulos, só o próprio ícone.
	var placed_ports: Array[Rect2] = []
	for record in _port_nodes:
		_layout_port_marker(record)
		var icon: Sprite3D = record.get("icon")
		if icon == null:
			continue
		if float(record.get("label_alpha", 0.0)) <= 0.01:
			icon.visible = false
			_append_port_report(record, Rect2(), false, Vector2.ZERO)
			continue
		var metrics := _port_metrics(record, camera)
		if metrics.is_empty():
			_append_port_report(record, Rect2(), false, Vector2.ZERO)
			continue
		var chosen := Vector2.ZERO
		if not bool(record.get("fused", false)):
			var found := false
			for candidate in _port_offset_candidates(record, camera):
				var rect: Rect2 = metrics["rect"]
				var shifted := Rect2(rect.position + candidate, rect.size)
				var ok := true
				for block in kept:
					if shifted.grow(1.0).intersects(block):
						ok = false
						break
				if ok:
					for other in placed_ports:
						if shifted.intersects(other):
							ok = false
							break
				if ok:
					chosen = candidate
					found = true
					break
			if not found:
				# Sem espaço local: fica só o modelo 3D do porto na costa e o
				# ícone da cidade; nada de linha.
				icon.visible = false
				var ring: Sprite3D = record.get("ring")
				if ring != null:
					ring.visible = false
				_append_port_report(record, Rect2(), false, Vector2.ZERO)
				continue
			if not chosen.is_zero_approx():
				_apply_port_offset(record, chosen, camera)
				metrics = _port_metrics(record, camera)
		var final_rect: Rect2 = metrics["rect"]
		placed_ports.append(final_rect)
		_append_port_report(record, final_rect, true, chosen)


func _append_port_report(record: Dictionary, rect: Rect2, visible: bool,
		offset: Vector2) -> void:
	_label_rects.append({
		"id": String(record.get("id", "")),
		"kind": "port",
		"rect": rect,
		"text_rect": Rect2(),
		"icon_rect": rect,
		"text_visible": false,
		"marker_visible": visible,
		"flag_visible": false,
		"capital": false,
		"population": 0.0,
		"same_city": bool(record.get("same_city", false)),
		"fused": bool(record.get("fused", false)),
		"city_id": String(record.get("city_id", "")),
		"offset": offset,
	})


## Retângulos de tela do último layout (cidade/porto), para o teste de
## sobreposição rodar sem renderizar a cena. Não há linhas-guia para reportar.
func label_layout_report() -> Array:
	return _label_rects.duplicate(true)


## Retângulo de tela de um bloco centrado no mundo (meia-largura/meia-altura em
## unidades de mundo), no plano da câmera.
func _world_rect_to_screen(camera: Camera3D, center: Vector3, half_x: float,
		half_y: float) -> Rect2:
	var basis := camera.global_transform.basis
	var a := camera.unproject_position(center - basis.x * half_x + basis.y * half_y)
	var b := camera.unproject_position(center + basis.x * half_x - basis.y * half_y)
	return Rect2(
		Vector2(minf(a.x, b.x), minf(a.y, b.y)),
		Vector2(absf(b.x - a.x), absf(b.y - a.y)))


## Métricas do rótulo de cidade no frame: retângulo do ícone e do texto.
func _city_metrics(record: Dictionary, camera: Camera3D) -> Dictionary:
	var label: Label3D = record.get("label")
	if label == null:
		return {}
	var anchor_world: Vector3 = to_global(record["anchor"])
	if camera.is_position_behind(anchor_world):
		return {}
	var em := float(label.font_size) * label.pixel_size
	var icon_h := em * PLACE_ICON_HEIGHT
	var icon_rect := _world_rect_to_screen(camera, anchor_world,
			icon_h * 0.5, icon_h * 0.5)
	var label_world: Vector3 = to_global(label.position)
	if camera.is_position_behind(label_world):
		return {}
	var text_w := label.font.get_string_size(label.text,
			HORIZONTAL_ALIGNMENT_LEFT, -1, label.font_size).x * label.pixel_size
	var text_rect := _world_rect_to_screen(camera, label_world,
			text_w * 0.5, em * 0.62)
	return {"icon_rect": icon_rect, "text_rect": text_rect}


## Métricas do ícone de porto no frame.
func _port_metrics(record: Dictionary, camera: Camera3D) -> Dictionary:
	var icon: Sprite3D = record.get("icon")
	if icon == null:
		return {}
	var marker_world: Vector3 = to_global(icon.position)
	if camera.is_position_behind(marker_world):
		return {}
	var size := PLACE_ICON_SOURCE * icon.pixel_size
	return {"rect": _world_rect_to_screen(camera, marker_world,
			size * 0.5, size * 0.5)}


## Candidatos de ajuste local do porto, em pixels de tela: o ponto de costa e
## deslocamentos curtos para o lado do mar (e perpendiculares), até 12 px.
func _port_offset_candidates(record: Dictionary, camera: Camera3D) -> Array[Vector2]:
	var candidates: Array[Vector2] = [Vector2.ZERO]
	var sea := Vector2(0.0, -1.0)
	var base_world: Vector3 = to_global(record["marker_base"])
	if not camera.is_position_behind(base_world):
		var sea_world: Vector3 = record.get("sea_dir_world", Vector3(1.0, 0.0, 0.0))
		var a := camera.unproject_position(base_world)
		var b := camera.unproject_position(base_world + sea_world)
		if a.distance_to(b) > 0.01:
			sea = (b - a).normalized()
	var normal := Vector2(-sea.y, sea.x)
	for distance in [4.0, 8.0, 12.0]:
		candidates.append(sea * distance)
		candidates.append(normal * distance)
		candidates.append(-normal * distance)
	return candidates


## Aplica o deslocamento em pixels de tela como translação no plano da câmera.
func _apply_port_offset(record: Dictionary, offset: Vector2, camera: Camera3D) -> void:
	var icon: Sprite3D = record.get("icon")
	if icon == null:
		return
	var base_world: Vector3 = to_global(record["marker_base"])
	var basis := camera.global_transform.basis
	var px_x := _screen_px_per_world(camera, base_world, basis.x)
	var px_y := _screen_px_per_world(camera, base_world, basis.y)
	if px_x <= 0.0 or px_y <= 0.0:
		return
	var target := base_world + basis.x * (offset.x / px_x) \
			- basis.y * (offset.y / px_y)
	var parent := icon.get_parent() as Node3D
	icon.position = parent.to_local(target) if parent != null else target
	var ring: Sprite3D = record.get("ring")
	if ring != null:
		ring.position = icon.position


## Pixels de tela por unidade de mundo numa direção, na profundidade do ponto.
func _screen_px_per_world(camera: Camera3D, point: Vector3, axis: Vector3) -> float:
	var a := camera.unproject_position(point)
	var b := camera.unproject_position(point + axis)
	return a.distance_to(b)


## Escala e fade do rótulo da cidade. O tamanho em tela é constante (pixel_size
## proporcional à distância); o que muda no zoom out é a faixa de fade por
## categoria: capitais duram mais, cidades pequenas somem antes.
func _apply_place_label_lod(record: Dictionary, distance: float) -> void:
	var label: Label3D = record.get("label")
	if label == null:
		return
	label.pixel_size = clampf(LABEL_PIXEL_PER_UNIT * distance,
			LABEL_PIXEL_MIN, LABEL_PIXEL_MAX)
	var range := _label_fade_range(record)
	var label_alpha := 1.0 - _fade_in(distance, range.x, range.y)
	record["label_alpha"] = label_alpha
	label.modulate.a = label_alpha
	label.visible = label_alpha > 0.01
	_layout_city_marker(record)

	var icon: Sprite3D = record.get("icon")
	if icon != null:
		icon.transparency = 1.0 - label_alpha
		icon.visible = label_alpha > 0.01
	var ring: Sprite3D = record.get("ring")
	if ring != null:
		ring.transparency = 1.0 - label_alpha
		ring.visible = label_alpha > 0.01
	var flag: Sprite3D = record.get("flag")
	if flag != null:
		flag.transparency = 1.0 - label_alpha
		flag.visible = label_alpha > 0.01


## Faixa (início, fim) do fade por distância, conforme a categoria do lugar.
func _label_fade_range(record: Dictionary) -> Vector2:
	if String(record.get("kind", "")) == "port":
		return Vector2(LABEL_FADE_SMALL_START, LABEL_FADE_SMALL_END)
	if bool(record.get("capital", false)):
		return Vector2(LABEL_FADE_CAPITAL_START, LABEL_FADE_CAPITAL_END)
	var population := float(record.get("priority", 0.0))
	if population >= LABEL_LARGE_POPULATION:
		return Vector2(LABEL_FADE_LARGE_START, LABEL_FADE_LARGE_END)
	if population >= LABEL_MEDIUM_POPULATION:
		return Vector2(LABEL_FADE_MEDIUM_START, LABEL_FADE_MEDIUM_END)
	return Vector2(LABEL_FADE_SMALL_START, LABEL_FADE_SMALL_END)


## LOD da âncora de porto: mesma escala de tela do texto de cidade, mas menor e
## com fade de cidade pequena (some no zoom out).
func _apply_port_lod(record: Dictionary, distance: float) -> void:
	var pixel_size := clampf(LABEL_PIXEL_PER_UNIT * distance,
			LABEL_PIXEL_MIN, LABEL_PIXEL_MAX)
	record["pixel_size"] = pixel_size
	var range := _label_fade_range(record)
	var alpha := 1.0 - _fade_in(distance, range.x, range.y)
	record["label_alpha"] = alpha
	var icon: Sprite3D = record.get("icon")
	if icon != null:
		icon.transparency = 1.0 - alpha
		icon.visible = alpha > 0.01
	var ring: Sprite3D = record.get("ring")
	if ring != null:
		ring.transparency = 1.0 - alpha
		ring.visible = alpha > 0.01 and not bool(record.get("fused", false))
	_layout_port_marker(record)


func _apply_city_lod(record: Dictionary, distance: float, world_position: Vector3) -> void:
	_apply_place_label_lod(record, distance)

	# Marcador discreto (continental): aparece só quando a cidade já sumiu.
	var marker_alpha := _fade_in(distance, MARKER_FADE_IN_START, MARKER_FADE_IN_END)
	for disc in record["discs"]:
		var mesh := disc as MeshInstance3D
		mesh.visible = marker_alpha > 0.01
		mesh.transparency = 1.0 - marker_alpha

	_update_urban(record, distance, world_position)


## Rampa de entrada: 0 antes de `start`, 1 após `end`.
func _fade_in(distance: float, start: float, end: float) -> float:
	return clampf((distance - start) / (end - start), 0.0, 1.0)


## Constrói a cidade modular apenas quando a câmera se aproxima e a libera ao
## afastar. Histerese evita reconstrução em vaivém.
func _update_urban(record: Dictionary, distance: float, world_position: Vector3) -> void:
	var urban: Node3D = record.get("urban")
	if urban == null:
		if distance <= URBAN_BUILD_DISTANCE:
			record["urban"] = _build_urban(record, world_position)
		return
	# Constrói uma vez e só alterna a visibilidade: sem hitch a cada zoom.
	urban.visible = distance <= CITY_VISIBLE_DISTANCE


func _build_urban(record: Dictionary, world_position: Vector3) -> Node3D:
	var city: Dictionary = record["city"]
	var manager := UrbanGrowthScript.new() as Node3D
	manager.name = "Urban_" + String(record["id"])
	manager.position = to_local(world_position) + Vector3(0.0, 0.16, 0.0)
	manager.scale = Vector3.ONE * URBAN_SCALE
	_content.add_child(manager)

	manager.setup(String(record["id"]))
	# Crescimento orgânico só em terra firme e dentro do território da nação.
	manager.buildable_filter = _make_urban_filter(
			to_local(world_position), String(city.get("nation", "")))
	manager.apply_indicators({
		"population": float(city.get("population", 0.0)),
		"industrialization": float(city.get("industrialization", 0.0)),
		"income": float(city.get("income", 0.5)),
		"jobs": float(city.get("jobs", 0.0)),
	})
	manager.apply_industry(IndustryProfileScript.derive(city))
	return manager


## Filtro orgânico do crescimento: só constrói em terra firme, dentro do
## território da nação e sem encostar nos portos.
func _make_urban_filter(city_local: Vector3, nation: String) -> Callable:
	var keepout: Array = []
	for p in _port_nodes:
		var pp: Vector3 = p.get("position", Vector3.ZERO)
		keepout.append(Vector2(pp.x, pp.z))
	return func(lot_local: Vector3) -> bool:
		if _territory == null:
			return true
		var local := city_local + Vector3(lot_local.x * URBAN_SCALE, 0.0,
				lot_local.z * URBAN_SCALE)
		var px := Vector2(
			local.x / PIXEL_TO_WORLD + MAP_SIZE_PX.x * 0.5,
			local.z / PIXEL_TO_WORLD + MAP_SIZE_PX.y * 0.5)
		var uv := Vector2(px.x / MAP_SIZE_PX.x, px.y / MAP_SIZE_PX.y)
		if uv.x < 0.0 or uv.x >= 1.0 or uv.y < 0.0 or uv.y >= 1.0:
			return false
		if not _territory.land_at_uv(uv):
			return false
		if _territory.owner_at_uv(uv) != nation:
			return false
		var flat := Vector2(local.x, local.z)
		for pp in keepout:
			if flat.distance_to(pp) < URBAN_PORT_KEEPOUT:
				return false
		return true
