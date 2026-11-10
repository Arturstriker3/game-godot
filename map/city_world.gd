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
## Ícone de lugar: pendurado logo acima do nome, com a mesma tinta e altura
## proporcional ao texto — ícone + nome leem como um selo único.
const PLACE_ICON_SOURCE := 512.0
const PLACE_ICON_HEIGHT := 1.0  # altura do ícone em ems do rótulo
const PLACE_ICON_GAP := 0.3     # respiro acima do contorno do texto, em ems
const BADGE_TEXTURE: Texture2D = preload("res://assets/icons/places/badge.png")
const RING_SCALE := 1.45        # selo ~45% maior que o ícone
const FLAG_SCALE := 0.6         # bandeira ~60% da altura do ícone
## Abaixo desta distância (u) o porto coincide com a cidade: sem ícone próprio.
const PORT_ICON_MERGE_DISTANCE := 0.5

## Raio de captação de recursos (só no modo recursos): o anel no chão mostra
## até onde a cidade alcança; o detalhe fica na legenda ao clicar na cidade.
const MINERAL_KEYS := ["coal", "copper", "gas", "iron", "oil"]
const WHEAT_STRIP_MIN := 0.40

const MARKER_COLOR := Color("#0A1212")
const MARKER_DOT := Color("#E8E2D0")

## Orçamento de renderização das cidades.
const MAX_DETAILED_CITIES := 40
const MAX_CITY_LABELS := 24

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

## Hierarquia de rótulos: padronizada. Capital vem do CSV (coluna `capital`) e
## fica um pouco maior; todo o resto usa o mesmo tamanho.
const LABEL_SIZE_CAPITAL := 34
const LABEL_SIZE_STANDARD := 28
const LABEL_COLOR := Color("#E8E2D0")
const LABEL_HOVER_COLOR := Color("#C9A24A")  # latão: hover/indica clicável
const ICON_COLOR := Color("#E8E2D0")          # marfim: alto contraste no selo escuro
## Fade de rótulo por distância (estilo HOI3).
const LABEL_FADE_START := 80.0
const LABEL_FADE_END := 120.0

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
var _capital_by_id := {}
## Filtro de recursos da legenda: {} = todos; senão kind -> bool.
var _resource_filter: Dictionary = {}
var _last_cities: Array = []
var _minimap_texture: ImageTexture

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


## Recursos que a cidade alcança (para a legenda): minérios no raio + trigo.
func city_resource_entries(city_id: String) -> Array:
	for record in _city_nodes:
		if String(record["id"]) == city_id:
			var city: Dictionary = record["city"]
			return _resource_entries(city_id, Vector2(
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
				capital, index < MAX_CITY_LABELS)
		var icon: Sprite3D = null
		var ring: Sprite3D = null
		var flag: Sprite3D = null
		if label != null:
			# Guarda a chave/fallback para atualizar o texto ao trocar idioma.
			label.set_meta("city_id", city_id)
			label.set_meta("city_fallback", city_name)
			var icon_texture := STAR_ICON if capital else CITY_ICON
			var marker := _add_place_icon(label, icon_texture, String(city["nation"]))
			icon = marker["icon"]
			ring = marker["ring"]
			flag = marker["flag"]
		_city_nodes.append({
			"id": city_id,
			"city": city,
			"position": position,
			"discs": discs,
			"label": label,
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
func _build_ports(ports: Array) -> void:
	# Porto que divide o id/nome com uma cidade (Amsterdam, Caiena) não repete a
	# legenda: o nome já está no mapa. Se o porto estiver praticamente em cima da
	# cidade, também dispensa o ícone (dois ícones ficariam sobrepostos); o modelo
	# 3D no cais continua marcando o porto.
	var city_positions := {}
	var city_names := {}
	for record in _city_nodes:
		if record.get("label") == null:
			continue
		city_positions[String(record.get("id", ""))] = record["position"]
		var city: Dictionary = record.get("city", {})
		city_names[String(city.get("name", "")).to_lower()] = true
	for port: Dictionary in ports:
		var pos := pixel_to_world(Vector2(float(port["x"]), float(port["y"])))

		var model := PORT_SCENE.instantiate() as Node3D
		_content.add_child(model)
		model.position = pos
		var bounds := _combined_aabb(model)
		var longest := maxf(bounds.size.x, maxf(bounds.size.y, bounds.size.z))
		if longest > 0.0:
			model.scale = Vector3.ONE * (PORT_SIZE / longest)

		# Nome + ícone do porto, na mesma composição vertical das cidades.
		var port_id := String(port["id"])
		var port_name := String(port["name"])
		var label: Label3D = null
		var icon: Sprite3D = null
		var ring: Sprite3D = null
		var flag: Sprite3D = null
		var city_position: Variant = city_positions.get(port_id)
		var on_city := false
		if city_position != null:
			on_city = pos.distance_to(city_position) <= PORT_ICON_MERGE_DISTANCE
		if not on_city:
			label = _add_port_label(pos, I18n.port_name(port_id, port_name))
			# Só omite o nome se ele for exatamente o da cidade; um porto com nome
			# próprio (Mana) mantém a legenda mesmo usando o id da cidade.
			label.set_meta("hide_text", city_names.has(port_name.to_lower()))
			label.set_meta("port_id", port_id)
			label.set_meta("port_fallback", port_name)
			var marker := _add_place_icon(label, PORT_ICON, String(port["nation"]))
			icon = marker["icon"]
			ring = marker["ring"]
			flag = marker["flag"]

		_port_nodes.append({
			"id": String(port["id"]),
			"name": String(port["name"]),
			"nation": String(port["nation"]),
			"position": pos,
			"label": label,
			"icon": icon,
			"ring": ring,
			"flag": flag,
		})


## Marcador de lugar (cidade/porto): símbolo dentro de um selo escuro com aro de
## latão + bandeira, dispostos horizontalmente à esquerda do nome.
func _add_place_icon(label: Label3D, texture: Texture2D, nation: String) -> Dictionary:
	var icon := Sprite3D.new()
	icon.texture = texture
	icon.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	icon.transparent = true
	icon.modulate = ICON_COLOR
	icon.no_depth_test = true
	icon.render_priority = label.render_priority - 1
	_content.add_child(icon)

	var ring := Sprite3D.new()
	ring.texture = BADGE_TEXTURE
	ring.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	ring.transparent = true
	ring.no_depth_test = true
	ring.render_priority = label.render_priority - 2
	_content.add_child(ring)

	var flag := _new_flag(nation)

	_layout_place_icon(label, icon, ring, flag)
	return {"icon": icon, "ring": ring, "flag": flag}


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


## Layout horizontal: [bandeira] [círculo+ícone] [nome]. O nome fica em
## `label.position`; o círculo e a bandeira ficam à esquerda (X do mundo).
func _layout_place_icon(label: Label3D, icon: Sprite3D, ring: Sprite3D, flag: Sprite3D) -> void:
	if label == null or icon == null:
		return
	var em := float(label.font_size) * label.pixel_size
	var icon_h := em * PLACE_ICON_HEIGHT
	var text_w := label.font.get_string_size(
			label.text, HORIZONTAL_ALIGNMENT_LEFT, -1, label.font_size).x * label.pixel_size
	var gap := em * PLACE_ICON_GAP

	icon.pixel_size = icon_h / PLACE_ICON_SOURCE
	var icon_x := label.position.x - text_w * 0.5 - gap - icon_h * 0.5
	icon.position = Vector3(icon_x, label.position.y, label.position.z)

	if ring != null:
		ring.pixel_size = icon_h * RING_SCALE / PLACE_ICON_SOURCE
		ring.position = icon.position

	if flag != null:
		var flag_h := icon_h * FLAG_SCALE
		flag.pixel_size = flag_h / 20.0  # bandeira 40×20 (2:1)
		flag.position = Vector3(
				icon_x - icon_h * RING_SCALE * 0.5 - gap - flag_h,
				label.position.y, label.position.z)


## Recursos que a cidade acessa: depósitos **onshore** cuja província pertence
## à cidade + trigo do território. Offshore fica decorativo por enquanto.
func _resource_entries(city_id: String, center: Vector2) -> Array:
	if _geo == null:
		_geo = GeographyScript.new()
		_geo.load_data()
	var counts := {}
	var reserves := {}
	if _mode_layer != null and _territory != null:
		for point in _mode_layer.mineral_points():
			if point.size() < 10:
				continue
			if int(point[9]) == 1:
				continue  # offshore: sem acesso até a mecânica de extração
			var province_id := int(point[8])
			if province_id < 0 or _territory.city_for_province(province_id) != city_id:
				continue
			var key := String(MINERAL_KEYS[clampi(int(point[2]), 0,
					MINERAL_KEYS.size() - 1)])
			counts[key] = int(counts.get(key, 0)) + 1
			reserves[key] = float(reserves.get(key, 0.0)) + float(point[3])

	var entries: Array = []
	for key in MINERAL_KEYS:
		if counts.has(key):
			entries.append({
				"kind": key,
				"count": int(counts[key]),
				"reserve_kt": float(reserves.get(key, 0.0)),
			})
	var wheat := float(_geo.crop_suitability(city_id, center, "wheat"))
	if wheat >= WHEAT_STRIP_MIN:
		entries.append({"kind": "wheat", "count": 1, "value": wheat})
	return entries



func _add_port_label(position: Vector3, port_name: String) -> Label3D:
	var label := Label3D.new()
	label.text = port_name.to_upper()
	label.font = LABEL_FONT
	label.font_size = LABEL_SIZE_STANDARD
	label.pixel_size = 0.019
	label.modulate = Color("#E8E2D0")
	label.outline_modulate = Color(0.04, 0.055, 0.055, 0.95)
	label.outline_size = int(LABEL_SIZE_STANDARD * 0.4)
	label.no_depth_test = true
	label.render_priority = 10
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.position = position + Vector3(0.0, 1.0, 0.0)
	_content.add_child(label)
	return label


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
	# Portos seguem o mesmo LOD dos nomes de cidade: sem tamanho fixo, o rótulo
	# do porto não fica gigante no zoom próximo nem minúsculo no distante.
	for record in _port_nodes:
		var port_position: Vector3 = to_global(record["position"])
		_apply_place_label_lod(record, camera_position.distance_to(port_position))
	for record in _city_nodes:
		var world_position: Vector3 = to_global(record["position"])
		var distance := camera_position.distance_to(world_position)
		_apply_city_lod(record, distance, world_position)


## Escala e fade compartilhados pelos rótulos de lugar (cidade e porto).
func _apply_place_label_lod(record: Dictionary, distance: float) -> void:
	var label: Label3D = record.get("label")
	if label == null:
		return
	label.pixel_size = clampf(LABEL_PIXEL_PER_UNIT * distance,
			LABEL_PIXEL_MIN, LABEL_PIXEL_MAX)
	# Fade de rótulo por distância (estilo HOI3): desaparece gradualmente longe.
	var label_alpha := 1.0 - _fade_in(distance, LABEL_FADE_START, LABEL_FADE_END)
	label.modulate.a = label_alpha
	var hide_text := label.has_meta("hide_text") and bool(label.get_meta("hide_text"))
	label.visible = not hide_text and label_alpha > 0.01

	var icon: Sprite3D = record.get("icon")
	if icon != null:
		_layout_place_icon(label, icon, record.get("ring"), record.get("flag"))
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
