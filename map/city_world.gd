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
const MapPickScript: Script = preload("res://map/map_pick.gd")
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
## Rótulos não dependem mais do LOD 3D: qualquer cidade pode receber nome quando
## a câmera chega perto. O pool de Label3D é reutilizado por prioridade (capital >
## população), então o custo é o do pool, não o do número de cidades.
const MAX_LABELED_CITIES := 160
## Grade espacial do layout de rótulos/ícones, em pixels de tela. Substitui a
## comparação O(n²): portos e cidades só testam as células vizinhas.
const LABEL_GRID_PX := 64.0
## Orçamento por frame para construir cidades detalhadas/portos sob demanda.
## Sem isso, 40 cidades próximas construídas no mesmo frame davam picos >500 ms.
const LOD_BUILD_BUDGET_USEC := 1500

## Layout de rótulos (projeção + colisão de tela): refeito quando a câmera se
## move — no máximo a cada `LAYOUT_FRAMES_MOVING` frames — e, com a câmera
## parada, só a cada `LAYOUT_FRAMES_IDLE` frames. As posições no wrap, o fade e
## os tamanhos continuam por frame em `_apply_wrap_positions()`.
const LAYOUT_FRAMES_MOVING := 2
const LAYOUT_FRAMES_IDLE := 24
const CAMERA_MOVE_EPSILON := 0.01

## Grade do LOD: célula (u de mundo) da grade XZ que guarda os registros. A cada
## frame só as células em volta da câmera são visitadas — não os 650+650
## lugares. O eixo X é modular (o mundo dá a volta).
const LOD_CELL := 62.5
## Acima deste deslocamento da câmera no espaço do mapa (zoom/teleporte) a
## varredura é completa: nenhum registro pode ficar com faixa de fade antiga.
const LOD_SCAN_JUMP := LOD_CELL * 0.5
## Raio (u) dentro do qual um registro pode ter rótulo/ícone/modelo aceso. É o
## maior `lod_end` elegível à grade; acima disso o registro vai para `_wide_cities`.
const LOD_NEAR_RADIUS := 128.0
## Porto: o modelo `port.glb` só nasce quando a câmera chega perto (o ícone é
## MultiMesh). `MAX_PORT_MODELS` limita os modelos vivos ao viajar pelo mundo.
const PORT_MODEL_DISTANCE := 110.0
const PORT_MODEL_VISIBLE_DISTANCE := 120.0
const MAX_PORT_MODELS := 96
## Layout de rótulos: sem linhas-guia. O nome fica ao lado do ícone, ancorado na
## cidade; quando dois nomes ainda colidem na tela, a cidade de menor população
## mostra só o ícone (o nome reaparece ao aproximar ou no hover). Capital só cede
## a outro rótulo de capital (ou ao hover): com 66 nações, Roma e Vaticano ficam a
## menos de 1 px e os Bálcãs têm 5 capitais no mesmo punhado de pixels — sem
## linhas-guia, alguém tem de ceder, e cede quem tem menos população. Portos são
## um ícone de âncora sem nome, no ponto de costa do ports.csv.
const LABEL_COLLIDE_PAD_PX := 3.0
## Alfa mínimo para o TEXTO entrar no layout. Abaixo disso o nome é um fantasma
## (7% de opacidade) que ainda ocupa uma vaga do pool e um bloco da grade; o
## ícone e o marcador continuam com o alfa do fade. Sem isso, cidades médias
## apareciam com nome no zoom out (PAL, 7%) contra a regra "só capitais e
## cidades grandes no longe".
const LABEL_TEXT_MIN_ALPHA := 0.15
## Margem (px) além da viewport em que a âncora de um rótulo ainda conta como "na
## tela": o texto de uma capital encostada na borda continua legível, então ela
## não pode ser descartada só porque a âncora saiu 10–20 px do retângulo.
const LABEL_SCREEN_MARGIN_PX := 24.0
## Deslocamento máximo, em pixels de tela, do ícone de porto para escapar do
## rótulo da própria cidade. O porto nunca "voa" para longe da costa.
const PORT_MAX_OFFSET_PX := 12.0
## Distância (px de tela) em que o porto da própria cidade deixa de ser um selo
## na costa e vira badge pequeno encostado no ícone da cidade. Sem isso, o
## porto de Amsterdã/Hamburgo/Copenhagen ficava por cima do ícone, do nome e da
## área urbana — ou empurrava o selo para dentro do mar.
const PORT_BADGE_MERGE_PX := 28.0
## Distância (u de mundo) do porto à própria cidade até a qual o selo é sempre
## badge: os portos do ports.csv com o mesmo id/nome ficam a até 4,5 u do centro
## (Hamburgo 3,7; Antuérpia 3,4), dentro do tecido urbano visto de perto.
const PORT_OWN_BADGE_DISTANCE := 5.0
## Respiro (px) entre o badge do porto e o ícone/nome da cidade.
const PORT_BADGE_GAP_PX := 2.0

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
## Fim do fade da cidade média em 100 (era 110): com a câmera inclinada, uma
## cidade do lado próximo da tela fica a ~103 u no zoom out de 120 e ainda
## aparecia com nome (alpha 0,18 > o corte de 0,15). O zoom out deve manter só
## capitais e cidades grandes.
const LABEL_FADE_MEDIUM_END := 100.0
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

## Fallback dos marcadores enquanto nations.csv não abre. A fonte real é a
## coluna `color` do cenário (231 nações), lida em `_marker_colors` no build.
const NATION_COLORS := {
	"GBR": Color("#8C3A3C"),
	"FRA": Color("#3C5A86"),
	"DEU": Color("#5E6668"),
	"NED": Color("#A1653A"),
	"BEL": Color("#A39244"),
}

var _content: Node3D
## Cidades, portos e rótulos ficam numa cópia ÚNICA. O wrap do mundo (cópias
## laterais) só duplica as superfícies baratas do `_content`; cada lugar é
## desenhado na cópia mais próxima da câmera pelo próprio `wrap_x`.
var _places: Node3D
var _gps_mesh: MeshInstance3D
var _atlas_mesh: MeshInstance3D
var _political_layer: MeshInstance3D
var _atlas_material: ShaderMaterial
var _political_material: ShaderMaterial
var _mode_layer: Node3D
var _city_nodes: Array[Dictionary] = []
## tag -> cor da nação (nations.csv), usada nos marcadores e nos discos.
var _marker_colors: Dictionary = {}
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
## `true` quando `_geo` é a instância do Economy (compartilhada, já carregada e
## com a atribuição A1 feita para o mesmo roster do cenário).
var _geo_shared := false
## Carregamento adiado da geografia (sem Economy): a thread parseia os JSON
## naturais enquanto o mapa já está jogável; o resultado chega em `_poll_geo_load`.
var _geo_thread: Thread
var _geo_result: RefCounted
var _capital_by_id := {}
## Filtro de recursos da legenda: {} = todos; senão kind -> bool.
var _resource_filter: Dictionary = {}
var _last_cities: Array = []
var _minimap_texture: ImageTexture
## Layout de rótulos: métricas de tela do último frame (relatório do teste).
var _label_rects: Array[Dictionary] = []
var _nav_image: Image
## Pool de rótulos e índice de prioridade (capital > população), independentes
## do LOD 3D. `_label_free` guarda os Label3D disponíveis; `_active_labels` os
## que estão com texto neste frame.
var _label_pool: Array[Label3D] = []
var _label_free: Array[Label3D] = []
var _active_labels: Array[Dictionary] = []
var _label_grid: Dictionary = {}
## Ícones distantes por instancing (cidade/porto) e modelos sob demanda.
var _city_icon_mm: MultiMeshInstance3D
var _city_star_mm: MultiMeshInstance3D
var _city_ring_mm: MultiMeshInstance3D
var _city_icon_plain_records: Array = []
var _city_icon_star_records: Array = []
var _port_icon_mm: MultiMeshInstance3D
var _port_ring_mm: MultiMeshInstance3D
var _urban_queue: Array[Dictionary] = []
var _port_model_queue: Array[Dictionary] = []
## Modelos de porto vivos (record["model"] != null). O trim só precisa varrer
## os 650 registros quando o total passa de `MAX_PORT_MODELS`.
var _live_port_models := 0
var _records_by_id: Dictionary = {}
## Listas de VISÍVEIS montadas no LOD de cada frame: o trabalho de layout/ícones
## percorre só elas, não os 650 lugares.
var _visible_city_records: Array = []
## Portos com selo aceso neste frame (alpha > 0,01). O layout percorre esta
## lista em vez dos 650 portos: a consulta da grade já filtrou por distância.
var _visible_port_records: Array = []
var _detailed_records: Array = []
var _drawn_ports: Array = []
## Grade espacial do LOD (XZ) e as cidades de raio longo (capitais/grandes), que
## são poucas e ficam fora da grade. `_grid_cells` conta as células em X.
var _city_grid: Dictionary = {}
var _port_grid: Dictionary = {}
var _wide_cities: Array = []
var _grid_cells := 16
## Raio de consulta de cada grade: o maior `lod_end` dos registros nela. Fora
## dele o registro está "assentado" (alfa 0); teleportes forçam varredura
## completa via `LOD_SCAN_JUMP`.
var _city_grid_radius := LOD_NEAR_RADIUS
var _port_grid_radius := LOD_NEAR_RADIUS
## Câmera (espaço local) no frame anterior, para detectar pulos grandes.
var _last_camera_local := Vector3(INF, INF, INF)
## Offset de wrap da câmera no eixo X (múltiplo de PLANE_SIZE.x).
var _camera_x := 0.0
## Estado da câmera no último layout (throttle do trabalho de tela) e espaço
## até o próximo layout obrigatório. `_layout_dirty` força um layout já no
## próximo frame (hover, seleção, idioma, filtro).
var _layout_camera_position := Vector3(INF, INF, INF)
var _layout_camera_basis := Basis()
var _layout_camera_fov := -1.0
var _layout_viewport := Vector2.ZERO
var _layout_cooldown := 0
var _layout_dirty := true
## Um movimento que caiu no cooldown (teleporte/recentralização de um frame)
## precisa de um layout posterior mesmo que a câmera já esteja parada.
var _layout_pending := false
## Cópias laterais do mundo (wrap em X): o rótulo clicável é o da cópia que
## está de fato na tela, não necessariamente a central. Com o pool de rótulos,
## o texto já é posicionado na cópia mais próxima — o pick testa só a atual.
var _wrap_offsets: Array[float] = [0.0]

## Emitido quando o conjunto de donos muda (o minimapa precisa se atualizar).
signal territory_changed


## Monta o mundo: chão de satélite, camada geopolítica e modelos de cidade.
func build(cities: Array, ports: Array) -> void:
	_load_city_flags()
	_marker_colors = TerritoryMapScript.scenario_colors()
	# Geografia: se o Economy (autoload) já carregou os JSON naturais, o mapa
	# reutiliza a MESMA instância. Sem ele, o parse de deposits.json (3,3 MB,
	# ~2 s) sai do caminho crítico e vem por thread (`_poll_geo_load`).
	_geo = _shared_geography()
	_geo_shared = _geo != null
	if _geo == null:
		_start_geo_load()
	_content = Node3D.new()
	_content.name = "Content"
	add_child(_content)
	_places = Node3D.new()
	_places.name = "Places"
	add_child(_places)

	_build_gps_plane()
	_build_political_layer()
	_build_mode_layer(cities)
	_build_cities(cities)
	_build_ports(ports)
	_build_city_labels()
	_build_nation_labels()
	_init_territory(cities)
	_build_lod_grid()
	set_mode(_mode)

	# Cópias laterais: o mundo dá a volta em X (cilíndrico), então quem sai por
	# um lado reaparece no outro. Só as superfícies (baratas) são duplicadas;
	# cidades, portos e rótulos são posicionados na cópia mais próxima por frame.
	for offset in [-PLANE_SIZE.x, PLANE_SIZE.x]:
		var copy := _content.duplicate() as Node3D
		copy.name = "ContentWrap%s" % ("Neg" if offset < 0.0 else "Pos")
		copy.position = Vector3(offset, 0.0, 0.0)
		add_child(copy)
	_layout_dirty = true


## GeographyService já carregado pelo Economy (autoload). O EconomicAI guarda a
## instância em `geo` e ele roda antes da cena principal, então o mapa não paga
## o parse dos JSON naturais de novo. `null` em testes sem o autoload — nesse
## caso o mapa carrega a própria cópia, na primeira vez que alguém pedir a ficha
## de recursos (nunca no build).
static func _shared_geography() -> RefCounted:
	# Busca o autoload pelo nome no root (não pela variável global): funciona
	# também em funções estáticas e em execuções onde o autoload não existe.
	var tree := Engine.get_main_loop() as SceneTree
	if tree == null or tree.root == null:
		return null
	var economy := tree.root.get_node_or_null("Economy")
	if economy == null:
		return null
	var ai: Object = economy.get("_ai")
	if ai == null:
		return null
	var geo: RefCounted = ai.get("geo")
	return geo


## Sem o Economy não há serviço carregado: o parse roda numa thread para o
## build não esperar. O mapa é jogável enquanto isso (os modos de calor não
## dependem de geografia); a ficha de recursos espera o join em `_ensure_geo`.
func _start_geo_load() -> void:
	if _geo_thread != null or _geo != null:
		return
	_geo_thread = Thread.new()
	if _geo_thread.start(_load_geo_worker) != OK:
		_geo_thread = null
		_geo = GeographyScript.new()
		_geo.load_data()


## Trabalho da thread: cria e carrega o serviço; nada toca a árvore de cena.
func _load_geo_worker() -> void:
	var geo: RefCounted = GeographyScript.new()
	geo.load_data()
	_geo_result = geo


## Entrega o resultado da thread quando ela termina (chamado por frame). Não
## bloqueia: se ainda está viva, tenta de novo no próximo frame.
func _poll_geo_load() -> void:
	if _geo_thread == null or _geo_thread.is_alive():
		return
	_geo_thread.wait_to_finish()
	_geo_thread = null
	if _geo_result == null:
		return
	_geo = _geo_result
	_geo_result = null
	if _mode_layer != null:
		_mode_layer.set_geography(_geo)


func _exit_tree() -> void:
	if _geo_thread != null:
		_geo_thread.wait_to_finish()
		_geo_thread = null



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
## Sem física: projeta o centro e um ponto na borda (footprint) para obter o
## raio em pixels. O hover e o clique compartilham `place_pick_radius_px()`.
func pick_city(screen_position: Vector2, camera: Camera3D) -> String:
	return _pick_place(_city_nodes, screen_position, camera)


## Raio de detecção em pixels de TELA de qualquer lugar clicável (cidade ou
## porto/construção): o footprint desenhado com o piso comum de MapPick.
## É a fonte única do hover e do clique — não há valor por tipo.
func place_pick_radius_px(place_id: String, camera: Camera3D) -> float:
	return MapPickScript.pick_radius_px(camera, _place_position(place_id),
			_place_footprint(place_id))


## Melhor lugar da lista sob o cursor (score < 1 dentro do raio de tela).
func _pick_place(nodes: Array, screen_position: Vector2, camera: Camera3D) -> String:
	if camera == null:
		return ""
	var best_id := ""
	var best_score := 1.0
	var right := camera.global_transform.basis.x
	for record in nodes:
		var world: Vector3 = to_global(record["position"] \
				+ Vector3(_wrap_offset(float(record["position"].x)), 0.0, 0.0))
		if camera.is_position_behind(world):
			continue
		var center := camera.unproject_position(world)
		var radius: float = MapPickScript.pick_radius_px(camera, world,
				_place_footprint(String(record["id"])))
		if radius <= 0.0:
			continue
		var score: float = center.distance_to(screen_position) / radius
		score = minf(score, _label_pick_score(record, screen_position, camera, right))
		if score <= 1.0 and (best_id.is_empty() or score < best_score):
			best_score = score
			best_id = String(record["id"])
	return best_id


## Score normalizado do retângulo do rótulo (nome + bandeira/selo). INF se não
## houver rótulo VISÍVEL na tela. Clicar no nome seleciona a cidade/porto.
##
## Regras de culling (o bug que fazia Caracas roubar o clique em Paris): um
## rótulo atrás da câmera, fora da viewport, oculto por LOD/colisão ou com alfa
## ~0 nunca é escolhido. A projeção de um ponto no plano da câmera gera
## coordenadas gigantes e um "raio" absurdo; exigir o centro dentro da tela
## elimina esse falso positivo. Testamos também as cópias de wrap do mundo.
func _label_pick_score(record: Dictionary, screen_position: Vector2,
		camera: Camera3D, right: Vector3) -> float:
	if camera == null:
		return INF
	var viewport := camera.get_viewport()
	if viewport == null:
		return INF
	var label: Label3D = record.get("label")
	if label == null or not is_instance_valid(label) \
			or not label.is_visible_in_tree():
		return INF
	var font := label.font
	if font == null:
		return INF
	if float(record.get("label_alpha", 1.0)) <= 0.01 or label.modulate.a <= 0.01:
		return INF
	var em := float(label.font_size) * label.pixel_size
	var text_w := font.get_string_size(label.text,
			HORIZONTAL_ALIGNMENT_LEFT, -1, label.font_size).x * label.pixel_size
	var half := right * (text_w * 0.5 + em * 1.6)
	var viewport_rect := Rect2(Vector2.ZERO, viewport.get_visible_rect().size)
	var base := label.global_position
	var best := INF
	for offset in _wrap_offsets:
		var label_world := base + Vector3(offset, 0.0, 0.0)
		if camera.is_position_behind(label_world):
			continue
		var center := camera.unproject_position(label_world)
		if not viewport_rect.has_point(center):
			continue
		var a := camera.unproject_position(label_world - half)
		var b := camera.unproject_position(label_world + half)
		var top := camera.unproject_position(label_world + Vector3.UP * em * 0.7)
		var radius := maxf(20.0, a.distance_to(top))
		best = minf(best, _segment_distance(screen_position, a, b) / radius)
	return best


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


## Posição global da cidade (para centralizar a câmera), já na cópia do wrap
## mais próxima da câmera — no outro lado da emenda ela está a ±1000 u.
func city_world_position(city_id: String) -> Vector3:
	for record in _city_nodes:
		if String(record.get("id", "")) == city_id:
			var position: Vector3 = record.get("position", Vector3.ZERO)
			return to_global(position + Vector3(_wrap_offset(position.x), 0.0, 0.0))
	return Vector3.ZERO


## Porto (construção) sob o cursor. Mesmo raio de tela da cidade, derivado do
## aro desenhado do porto (`_port_footprint`) com o piso comum de MapPick.
func pick_port(screen_position: Vector2, camera: Camera3D) -> String:
	return _pick_place(_port_nodes, screen_position, camera)


## Posição global do porto (para centralizar a câmera), na mesma cópia de wrap
## usada para desenhar — com registro ausente devolve a origem em vez de erro.
func port_world_position(port_id: String) -> Vector3:
	for record in _port_nodes:
		if String(record.get("id", "")) == port_id:
			var position: Vector3 = record.get("position", Vector3.ZERO)
			return to_global(position + Vector3(_wrap_offset(position.x), 0.0, 0.0))
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
	_invalidate_layout()
	if _selection == null:
		_create_indicators()
	if _selection == null:
		return
	var active := not port_id.is_empty()
	_selection.visible = active
	_selection_dot.visible = active
	if active:
		var ring_radius := _ring_world_radius(port_id)
		var base: Vector3 = port_world_position(port_id) + Vector3(0.0, 0.15, 0.0)
		_selection.scale = Vector3(ring_radius, 1.0, ring_radius)
		_selection.global_position = base
		_selection_dot.global_position = base + Vector3(0.0, 0.03, 0.0)


func selected_city() -> String:
	return _selected_id


## Id do lugar selecionado (cidade ou porto); "" quando nada está selecionado.
func selected_place_id() -> String:
	return _selected_id


## Tipo de um lugar pelo id: "city", "port" ou "".
func place_kind(place_id: String) -> String:
	if place_id.is_empty():
		return ""
	for record in _city_nodes:
		if String(record["id"]) == place_id:
			return "city"
	for record in _port_nodes:
		if String(record["id"]) == place_id:
			return "port"
	return ""


## Depósito selecionado no mapa de recursos (-1 = nenhum).
func selected_deposit() -> int:
	if _mode_layer == null:
		return -1
	return _mode_layer.selected_deposit()


## Há qualquer seleção no mapa (cidade, porto/construção ou jazida)?
func has_selection() -> bool:
	return not _selected_id.is_empty() or selected_deposit() >= 0


## Remove TODA a seleção do mapa: anel de cidade/porto, jazida e hover.
func clear_selection() -> void:
	if not _selected_id.is_empty():
		select_city("")
	set_deposit_selected(-1)
	set_hover("")


## Seleção discreta: um anel fino rente ao chão + um ponto central pequeno.
## Não esconde edifícios nem atravessa o terreno (fica a ~0,15 u de altura).
func select_city(city_id: String) -> void:
	_selected_id = city_id
	_invalidate_layout()
	_set_territory_highlight(city_id)
	if _selection == null:
		_create_indicators()
	if _selection == null:
		return
	var active := not city_id.is_empty()
	_selection.visible = active
	_selection_dot.visible = active
	if active:
		var ring_radius := _ring_world_radius(city_id)
		var base: Vector3 = city_world_position(city_id) + Vector3(0.0, 0.15, 0.0)
		_selection.scale = Vector3(ring_radius, 1.0, ring_radius)
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
		_invalidate_layout()

	if _selection == null:
		_create_indicators()
	if _hover == null:
		return
	if place_id == _selected_id:
		_hover.visible = false
		return
	_hover.visible = not place_id.is_empty()
	if _hover.visible:
		var ring_radius := _ring_world_radius(place_id)
		_hover.scale = Vector3(ring_radius, 1.0, ring_radius)
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
	record["hovered"] = hovered
	var label: Label3D = record.get("label")
	if label != null:
		label.modulate = Color(label_color.r, label_color.g, label_color.b,
				label.modulate.a)
	var icon: Sprite3D = record.get("icon")
	if icon != null:
		icon.modulate = Color(icon_color.r, icon_color.g, icon_color.b, 1.0)


## Posição global de um lugar (cidade ou porto) pelo id, na cópia do wrap que
## está sendo desenhada.
func _place_position(place_id: String) -> Vector3:
	var record: Dictionary = _records_by_id.get(place_id, {})
	if record.is_empty():
		return Vector3.ZERO
	var position: Vector3 = record["position"]
	return to_global(position + Vector3(_wrap_offset(position.x), 0.0, 0.0))


## Raio da cidade em unidades de mundo, para o anel de seleção abraçar o tecido.
func _city_footprint(city_id: String) -> float:
	var record: Dictionary = _records_by_id.get(city_id, {})
	var urban = record.get("urban")
	if urban != null and urban.has_method("extent"):
		return maxf(0.6, float(urban.extent()) * URBAN_SCALE + 0.18)
	return 1.8


## Raio em unidades de mundo do aro desenhado do porto (o mesmo badge de
## `_layout_port_marker`). É a referência visual do hover/seleção e do pick.
func _port_footprint(record: Dictionary) -> float:
	var pixel_size := float(record.get("pixel_size", LABEL_PIXEL_PER_UNIT * 52.0))
	var em := LABEL_SIZE_STANDARD * pixel_size
	var badge := _port_is_badge(record)
	var icon_h := em * (PORT_BADGE_SCALE if badge else PORT_ICON_HEIGHT)
	return icon_h * RING_SCALE * 0.5


## Footprint ÚNICO de um lugar (cidade ou porto/construção), usado tanto pelo
## anel de hover/seleção quanto pelo raio de detecção em tela.
func _place_footprint(place_id: String) -> float:
	var record: Dictionary = _records_by_id.get(place_id, {})
	if record.is_empty():
		return 1.8
	if String(record.get("kind", "")) == "city":
		return _city_footprint(place_id)
	return _port_footprint(record)


## Raio em unidades de mundo do anel de hover/seleção. É o raio de pick em
## PIXELS convertido para o mundo na profundidade do lugar, então o anel mostra
## a área clicável real: cidade e porto ficam com o mesmo raio em tela (piso de
## 26 px) e, no zoom aproximado, o anel volta a abraçar o ícone desenhado.
func _ring_world_radius(place_id: String) -> float:
	var footprint := _place_footprint(place_id)
	var camera := get_viewport().get_camera_3d()
	if camera == null:
		return footprint
	var position := _place_position(place_id)
	var raw: float = MapPickScript.icon_radius_px(camera, position, footprint)
	if raw <= 0.0:
		return footprint
	return footprint * place_pick_radius_px(place_id, camera) / raw


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
	# Nomes das cidades: o pool é reconfigurado no próximo layout; aqui só
	# invalidamos o texto/largura em cache.
	for record in _city_nodes:
		record["text"] = ""
		record["text_width"] = 0.0
	_invalidate_layout()


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


## Cor da nação do marcador: nations.csv (231) com o fallback histórico das 5
## jogáveis. Depois de o TerritoryMap existir, ele é a fonte (inclui a paleta
## de desempate de vizinhos).
func _marker_color(tag: String) -> Color:
	if _territory != null:
		var live: Color = _territory.nation_color(tag)
		if live.a > 0.0:
			return live
	return _marker_colors.get(tag, NATION_COLORS.get(tag, Color.WHITE))


## Quando o dono de uma cidade muda (override de território), o marcador passa
## a usar a cor da nação efetiva, não a original. O rótulo já é neutro (#E8E2D0).
func _update_city_nation_colors(cities: Array) -> void:
	for city in cities:
		var city_id := String(city.get("id", ""))
		var tag := String(_territory_overrides.get(city_id, city.get("nation", "")))
		for node in _city_nodes:
			if String(node.get("id", "")) != city_id:
				continue
			var color: Color = _marker_color(tag)
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
	# Passa a geografia já carregada; o ModeLayer só cria a própria instância
	# quando chamado fora do CityWorld (sem Economy).
	_mode_layer.setup(cities, _geo)


func _build_cities(cities: Array) -> void:
	# Só as maiores ganham o modelo 3D detalhado. As demais ficam apenas com o
	# ícone e o rótulo do pool: sem disco raso no chão (a "mancha" clara que
	# aparecia sob Alexandria/Tanta/Suez) e sem tecido urbano próprio.
	var ordered := cities.duplicate()
	ordered.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("population", 0.0)) > float(b.get("population", 0.0))
	)
	var detailed := mini(MAX_DETAILED_CITIES, ordered.size())
	var detailed_ids := {}
	for index in detailed:
		detailed_ids[String(ordered[index].get("id", ""))] = true
	for city: Dictionary in cities:
		var city_id := String(city.get("id", ""))
		var position: Vector3 = pixel_to_world(Vector2(city["x"], city["y"]))
		var color: Color = _marker_color(String(city["nation"]))
		var capital := bool(_capital_by_id.get(city_id, false))
		var discs: Array = []
		var icon: Sprite3D = null
		var ring: Sprite3D = null
		var flag: Sprite3D = null
		if detailed_ids.has(city_id):
			discs = _spawn_city_marker(position, String(city.get("name", "")), color)
			var icon_texture := STAR_ICON if capital else CITY_ICON
			# Bandeira só na capital: nas cidades comuns ela alargava o rótulo
			# e forçava o nome a sair de cima das vizinhas.
			var marker := _add_place_icon(10, icon_texture, String(city["nation"]), capital)
			icon = marker["icon"]
			ring = marker["ring"]
			flag = marker["flag"]
		var record := {
			"id": city_id,
			"kind": "city",
			"city": city,
			"position": position,
			"anchor": position + Vector3(0.0, 1.0, 0.0),
			"ground": position,
			"priority": float(city.get("population", 0.0)),
			"capital": capital,
			"detailed": detailed_ids.has(city_id),
			"discs": discs,
			"disc_local": _disc_local_positions(discs),
			"label": null,
			"label_alpha": 1.0,
			"pixel_size": LABEL_PIXEL_PER_UNIT * 52.0,
			"font_size": LABEL_SIZE_CAPITAL if capital else LABEL_SIZE_STANDARD,
			"text_width": 0.0,
			"wrap_x": 0.0,
			"icon": icon,
			"ring": ring,
			"flag": flag,
			"urban": null,
			"urban_queued": false,
		}
		_city_nodes.append(record)
		_records_by_id[city_id] = record
		# Faixa de fade e raio de "assentamento" do LOD, calculados uma vez: a
		# categoria (capital/população) não muda, então o frame só compara
		# distâncias — sem alocar Vector2 nem reavaliar a categoria.
		var fade := _label_fade_range(record)
		record["fade_start"] = fade.x
		record["fade_end"] = fade.y
		record["lod_end"] = maxf(fade.y, MARKER_FADE_IN_END) \
				if bool(record["detailed"]) else fade.y
		if bool(record["detailed"]):
			_detailed_records.append(record)


## Pool de rótulos e ícones distantes (MultiMesh). Criado uma vez no build; o
## conteúdo é reutilizado a cada frame pelo layout.
func _build_city_labels() -> void:
	for _index in MAX_LABELED_CITIES:
		var label := Label3D.new()
		label.text = ""
		label.font = LABEL_FONT
		label.font_size = LABEL_SIZE_STANDARD
		label.pixel_size = 0.019
		label.modulate = LABEL_COLOR
		label.outline_modulate = Color(0.04, 0.055, 0.055, 0.95)
		label.outline_size = int(LABEL_SIZE_STANDARD * 0.4)
		label.no_depth_test = true
		label.render_priority = 10
		label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		label.visible = false
		_places.add_child(label)
		_label_pool.append(label)
		_label_free.append(label)

	# Ícones de cidade fora do LOD detalhado: prédios (comum) e estrela
	# (capital), cada um com o selo escuro, desenhados por instancing.
	var plain: Array = []
	var stars: Array = []
	for record in _city_nodes:
		if bool(record["detailed"]):
			continue
		if bool(record["capital"]):
			stars.append(record)
		else:
			plain.append(record)
	_city_icon_mm = _make_billboard_multimesh(CITY_ICON, 9, plain.size())
	_city_star_mm = _make_billboard_multimesh(STAR_ICON, 9, stars.size())
	_city_ring_mm = _make_billboard_multimesh(BADGE_TEXTURE, 8,
			plain.size() + stars.size())
	_city_icon_plain_records = plain
	_city_icon_star_records = stars


## Ícones de porto: âncora e aro no ponto de costa, por instancing.
func _build_port_icons(ports: Array) -> void:
	_port_icon_mm = _make_billboard_multimesh(PORT_ICON, 11, ports.size())
	_port_ring_mm = _make_billboard_multimesh(BADGE_TEXTURE, 10, ports.size())


func _make_billboard_multimesh(texture: Texture2D, priority: int,
		count: int) -> MultiMeshInstance3D:
	if count <= 0:
		return null
	var quad := QuadMesh.new()
	quad.size = Vector2.ONE
	var material := StandardMaterial3D.new()
	material.albedo_texture = texture
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	material.billboard_keep_scale = true
	material.no_depth_test = true
	material.render_priority = priority
	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.mesh = quad
	multimesh.instance_count = count
	multimesh.visible_instance_count = 0
	var instance := MultiMeshInstance3D.new()
	instance.multimesh = multimesh
	instance.material_override = material
	instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_places.add_child(instance)
	return instance



## Portos como entidades 3D: modelo port.glb (sem disco de fundo, como a cidade).
## O modelo 3D fica SEMPRE na coordenada real de costa do ports.csv. Quando o
## porto é o da própria cidade (mesmo id OU mesmo nome), o selo de tela não
## repete nome/bandeira e, se encostar no selo da cidade, é desenhado como badge
## pequeno ao lado do ícone (PORT_BADGE_SCALE), nunca por cima do nome nem da
## área urbana.
func _build_ports(ports: Array) -> void:
	var city_by_id := {}
	var city_by_name := {}
	for record in _city_nodes:
		city_by_id[String(record.get("id", ""))] = record
		var city: Dictionary = record.get("city", {})
		city_by_name[String(city.get("name", "")).to_lower()] = record
	for port: Dictionary in ports:
		var port_px := Vector2(float(port["x"]), float(port["y"]))
		var pos := pixel_to_world(port_px)

		var port_id := String(port["id"])
		var port_name := String(port["name"])
		# `port_of` (ports.csv) é a cidade de origem declarada pelo cenário —
		# inclusive para os portos de costa que não compartilham id/nome com a
		# cidade. Sem ele, cai no id e depois no nome (contrato antigo).
		var port_of := String(port.get("port_of", ""))
		var city_record: Dictionary = city_by_id.get(port_of, {}) \
				if port_of != "" else city_by_id.get(port_id, {})
		if city_record.is_empty():
			city_record = city_by_name.get(port_name.to_lower(), {})
		var same_city := false
		if not city_record.is_empty():
			var city: Dictionary = city_record.get("city", {})
			var city_name := String(city.get("name", "")).to_lower()
			var distance := pos.distance_to(city_record["position"])
			same_city = port_of != "" or city_name == port_name.to_lower() \
					or distance <= PORT_ICON_MERGE_DISTANCE

		# Porto: âncora pequena no ponto de costa, SEM nome e SEM bandeira.
		# Quando coincide com a própria cidade, vira um badge sobre o ícone dela.
		var fused := same_city and not city_record.is_empty() \
				and pos.distance_to(city_record["position"]) <= PORT_ICON_MERGE_DISTANCE

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

		var record := {
			"id": port_id,
			"kind": "port",
			"name": port_name,
			"nation": String(port["nation"]),
			"position": pos,
			"anchor": pos + Vector3(0.0, 0.8, 0.0),
			"marker_base": pos + Vector3(0.0, 0.8, 0.0),
			"marker_world": pos + Vector3(0.0, 0.8, 0.0),
			"ground": pos,
			"priority": 0.0,
			"same_city": same_city,
			"fused": fused,
			"badge": false,
			"city_id": String(city_record.get("id", "")),
			"sea_dir_world": Vector3(sea_dir.x, 0.0, sea_dir.y),
			"label": null,
			"label_alpha": 1.0,
			"pixel_size": LABEL_PIXEL_PER_UNIT * 52.0,
			"wrap_x": 0.0,
			"icon": null,
			"ring": null,
			"flag": null,
			"model": null,
			"model_queued": false,
		}
		_port_nodes.append(record)
		_records_by_id[port_id] = record
		# Faixa de fade do selo e raio de assentamento do LOD (modelo 3D).
		record["fade_start"] = LABEL_FADE_SMALL_START
		record["fade_end"] = LABEL_FADE_SMALL_END
		record["lod_end"] = maxf(LABEL_FADE_SMALL_END, PORT_MODEL_VISIBLE_DISTANCE)
	_build_port_icons(ports)



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
	_places.add_child(icon)

	var ring := Sprite3D.new()
	ring.texture = BADGE_TEXTURE
	ring.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	ring.transparent = true
	ring.no_depth_test = true
	ring.render_priority = render_priority - 2
	_places.add_child(ring)

	var flag: Sprite3D = _new_flag(nation) if with_flag else null

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
	_places.add_child(flag)
	return flag


## Nome atual da cidade (i18n), calculado uma vez e invalidado na troca de idioma.
func _city_text(record: Dictionary) -> String:
	var text := String(record.get("text", ""))
	if text.is_empty():
		var city: Dictionary = record.get("city", {})
		text = I18n.city_name(String(record["id"]),
				String(city.get("name", ""))).to_upper()
		record["text"] = text
	return text


## Largura do nome em unidades de mundo na escala atual do frame.
func _city_text_width(record: Dictionary) -> float:
	var width := float(record.get("text_width", 0.0))
	if width <= 0.0:
		width = LABEL_FONT.get_string_size(_city_text(record),
				HORIZONTAL_ALIGNMENT_LEFT, -1, int(record["font_size"])).x
		record["text_width"] = width
	return width * float(record["pixel_size"])


## Dispõe o marcador de cidade: [bandeira só capital] [aro+ícone] [nome].
## O ícone fica exatamente na âncora; o nome cresce para a direita. A posição
## inclui o wrap (cópia lateral mais próxima da câmera). Roda TODO frame para
## os lugares visíveis — é só aritmética e escrita de transform, sem nó novo —
## enquanto a colisão de rótulos (`_layout_place_labels`) é fatiada.
func _place_city_marker(record: Dictionary) -> void:
	var wrap := float(record.get("wrap_x", 0.0))
	# Passo quantizado (= distância arredondada; pixel_size é proporcional a
	# ela). Se nem o wrap nem o passo mudaram, não há o que reescrever: só o
	# alfa do nome, que é contínuo.
	var step := roundi(float(record["pixel_size"]) / LABEL_PIXEL_PER_UNIT)
	if int(record.get("placed_step", -1)) == step \
			and is_equal_approx(float(record.get("placed_wrap", 0.0)), wrap):
		var current: Label3D = record.get("label")
		if current != null:
			current.modulate.a = float(record.get("label_alpha", 1.0))
		return
	record["placed_step"] = step
	record["placed_wrap"] = wrap
	var anchor: Vector3 = record["anchor"] + Vector3(wrap, 0.0, 0.0)
	var em := float(record["font_size"]) * float(record["pixel_size"])
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

	var text_w := _city_text_width(record)
	var text_center := anchor + Vector3(ring_h * 0.5 + gap + text_w * 0.5,
			0.0, 0.0)
	record["text_center"] = text_center

	# Marcador em camadas (só cidades detalhadas): mesma cópia de wrap.
	var discs: Array = record.get("discs", [])
	var bases: Array = record.get("disc_local", [])
	for index in discs.size():
		(discs[index] as MeshInstance3D).position = bases[index] \
				+ Vector3(wrap, 0.0, 0.0)

	# Nome do pool já atribuído: acompanha o wrap e o fade sem refazer a
	# colisão de tela (a cor base vem do layout e é preservada).
	var label: Label3D = record.get("label")
	if label != null:
		label.position = text_center
		label.modulate.a = float(record.get("label_alpha", 1.0))



## Recursos PRÓPRIOS da cidade para a legenda: jazidas minerais que pertencem
## a ela (regra A1 — cada jazida tem um único dono) + as culturas da sua bacia
## acima do limiar do investidor (0,30), ordenadas por aptidão decrescente.
## Antes mostrava as jazidas da NAÇÃO inteira: toda cidade da mesma nação
## listava os mesmos minerais ("acesso aos mesmos depósitos"). Sem duplicar:
## minerais e culturas vêm de catálogos distintos.
func _resource_entries(city_id: String, nation: String, center: Vector2) -> Array:
	_ensure_geo()
	if _geo == null:
		return []
	_ensure_catchment()
	var code := String(_geo.nation_code(nation))
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


## Garante um GeographyService carregado para a ficha/legenda. Idempotente:
## com o Economy presente é a mesma instância já carregada (custo zero); só em
## contexto sem Economy o mapa carrega a própria cópia.
func _ensure_geo() -> void:
	if _geo == null:
		_geo = _shared_geography()
		_geo_shared = _geo != null
	if _geo == null and _geo_thread != null:
		_geo_thread.wait_to_finish()
		_geo_thread = null
		_geo = _geo_result
		_geo_result = null
		if _mode_layer != null:
			_mode_layer.set_geography(_geo)
	if _geo == null:
		_geo = GeographyScript.new()
	_geo.load_data()


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
	# Instância do Economy: `apply_to_sim` já atribuiu províncias e depósitos com
	# o MESMO roster do cenário. Reatribuir aqui só repetiria o trabalho e, com
	# ids sintéticos (benchmark), poderia divergir da economia — então só
	# garantimos a posse A1, que é idempotente.
	if _geo_shared:
		_geo.ensure_deposit_ownership()
	else:
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


static func _disc_local_positions(discs: Array) -> Array[Vector3]:
	var result: Array[Vector3] = []
	for disc in discs:
		result.append((disc as MeshInstance3D).position)
	return result


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
	_places.add_child(instance)
	return instance



## LOD por distância: rótulos de cidade/porto, marcador e cidade modular.
## O loop percorre todas as cidades/portos (barato: distância + wrap), mas só
## projeta/monta o que está dentro da faixa de fade do frame. O layout de tela
## (projeção + colisão de rótulos) é fatiado: câmera em movimento, um a cada
## `LAYOUT_FRAMES_MOVING` frames; parada, um a cada `LAYOUT_FRAMES_IDLE`.
func _process(_delta: float) -> void:
	if _city_nodes.is_empty() and _port_nodes.is_empty():
		return
	var camera := get_viewport().get_camera_3d()
	if camera == null:
		return

	var camera_position := camera.global_position
	var camera_local := to_local(camera_position)
	_camera_x = camera_local.x
	_poll_geo_load()
	# Mudar o transform do PRÓPRIO mapa (recentralizar o mundo, trocar o nó de
	# lugar) move a câmera no espaço local sem mexer na câmera global; o layout
	# precisa acompanhar os dois.
	var local_delta := _last_camera_local.distance_to(camera_local)
	var moved := _camera_changed(camera) or local_delta > CAMERA_MOVE_EPSILON
	# Câmera parada, nada marcado como sujo e nenhum modelo na fila: nada na
	# tela muda. O frame custa só a comparação da câmera. Com modelo na fila,
	# só a fila roda (construir um modelo por frame já é o pico do frame).
	if not moved and not _layout_dirty and not _layout_pending:
		if _urban_queue.is_empty() and _port_model_queue.is_empty():
			return
		_process_lod_queues(camera)
		return
	_update_nation_labels(camera_position)
	_visible_city_records.clear()
	_visible_port_records.clear()
	# Pulo grande da câmera (clique no HUD, limite, teleporte): uma varredura
	# completa assenta/apaga o que ficou fora das células — senão um rótulo ou
	# ícone processado perto ficaria aceso no lugar errado.
	var jump := local_delta > LOD_SCAN_JUMP
	_last_camera_local = camera_local
	if jump:
		for record in _port_nodes:
			_apply_lod_port(record, camera_local, camera)
		for record in _city_nodes:
			_apply_lod_city(record, camera_local, camera)
	else:
		# Portos e cidades só nas células da grade em volta da câmera; as
		# cidades de raio longo (capitais/grandes) ficam fora da grade.
		for record in _lod_records(_port_grid, camera_local, _port_grid_radius):
			_apply_lod_port(record, camera_local, camera)
		for record in _wide_cities:
			_apply_lod_city(record, camera_local, camera)
		for record in _lod_records(_city_grid, camera_local, _city_grid_radius):
			_apply_lod_city(record, camera_local, camera)
	# Posição no wrap + escala de tela dos marcadores visíveis: não depende do
	# layout (que é fatiado) e mantém discos/ícones/nomes colados na cópia mais
	# próxima da câmera.
	_apply_wrap_positions()
	# Layout simples (projeção + colisão): âncora fixa, sem linhas-guia. Nomes
	# que ainda colidem dão lugar ao ícone da cidade menor (prioridade).
	if _should_relayout(moved):
		_layout_place_labels(camera)
	# Ícones distantes (MultiMesh): compacta o buffer visível a cada frame para
	# o zoom/pan não deixar o instancing um frame atrás do layout fatiado.
	_apply_icon_transforms(camera)
	# Modelos detalhados entram fatiados, com orçamento por frame.
	_process_lod_queues(camera)


## Offset da cópia do mundo (wrap cilíndrico) mais próxima da câmera.
func _wrap_offset(x: float) -> float:
	return roundf((_camera_x - x) / PLANE_SIZE.x) * PLANE_SIZE.x


## Força um layout no próximo frame (hover, seleção, idioma, filtro, modo).
func _invalidate_layout() -> void:
	_layout_dirty = true


## Decide se o layout de tela roda neste frame. Quando a câmera está parada e
## nada mudou, o trabalho é pulado — o que sobra por frame é só o LOD barato.
func _should_relayout(moved: bool) -> bool:
	# Câmera em movimento usa o ritmo de pan; se ela vinha parada (cooldown
	# longo) e se mexeu, encurta o cooldown para o próximo frame.
	if moved and _layout_cooldown > LAYOUT_FRAMES_MOVING:
		_layout_cooldown = LAYOUT_FRAMES_MOVING
	if _layout_cooldown > 0:
		_layout_cooldown -= 1
		if not _layout_dirty and _layout_cooldown > 0:
			if moved:
				_layout_pending = true
			return false
	_layout_dirty = false
	_layout_pending = false
	_layout_cooldown = LAYOUT_FRAMES_MOVING if moved else LAYOUT_FRAMES_IDLE
	return true


## Compara a câmera (posição, rotação, fov e viewport) com o último layout e
## guarda o estado novo. Verdadeiro se algo mudou além da tolerância.
func _camera_changed(camera: Camera3D) -> bool:
	var transform := camera.global_transform
	var viewport := camera.get_viewport()
	var viewport_size := viewport.get_visible_rect().size if viewport != null \
			else Vector2.ZERO
	var moved := _layout_camera_position.distance_to(transform.origin) \
			> CAMERA_MOVE_EPSILON \
			or not _layout_camera_basis.is_equal_approx(transform.basis) \
			or not is_equal_approx(_layout_camera_fov, camera.fov) \
			or _layout_viewport != viewport_size
	_layout_camera_position = transform.origin
	_layout_camera_basis = transform.basis
	_layout_camera_fov = camera.fov
	_layout_viewport = viewport_size
	return moved


## Reaplica posição, escala de tela e alfa de cada marcador na cópia de wrap
## mais próxima. Percorre só as cidades com rótulo aceso — a lista é montada no
## LOD do frame —, então não custa nada além do que está na tela.
func _apply_wrap_positions() -> void:
	for record in _visible_city_records:
		_place_city_marker(record)
	for record in _detailed_records:
		# Discos do LOD continental continuam visíveis bem depois do nome.
		if float(record.get("label_alpha", 0.0)) <= 0.01:
			_place_city_marker(record)


## A posição do registro (já com o wrap) está dentro do frustum da câmera?
## Usado só para não construir modelo 3D de cidade/porto fora da tela.
func _in_frustum(record: Dictionary, camera: Camera3D) -> bool:
	var local: Vector3 = record["position"] \
			+ Vector3(float(record.get("wrap_x", 0.0)), 0.0, 0.0)
	return camera.is_position_in_frustum(to_global(local))


## Monta a grade do LOD uma vez (as posições dos lugares são estáticas).
func _build_lod_grid() -> void:
	_grid_cells = maxi(1, roundi(PLANE_SIZE.x / LOD_CELL))
	_city_grid.clear()
	_port_grid.clear()
	_wide_cities.clear()
	_city_grid_radius = 0.0
	_port_grid_radius = 0.0
	for record in _city_nodes:
		if float(record["lod_end"]) > LOD_NEAR_RADIUS:
			_wide_cities.append(record)
		else:
			_add_to_grid(_city_grid, record)
			_city_grid_radius = maxf(_city_grid_radius, float(record["lod_end"]))
	for record in _port_nodes:
		_add_to_grid(_port_grid, record)
		_port_grid_radius = maxf(_port_grid_radius, float(record["lod_end"]))


## Célula da grade (X modular; Z preso ao plano) de um registro.
func _lod_grid_key(record: Dictionary) -> Vector2i:
	var position: Vector3 = record["position"]
	var cell := PLANE_SIZE.x / float(maxi(_grid_cells, 1))
	var cells_z := maxi(1, roundi(PLANE_SIZE.y / cell))
	return Vector2i(
		wrapi(floori((position.x + PLANE_SIZE.x * 0.5) / cell), 0, _grid_cells),
		clampi(floori((position.z + PLANE_SIZE.y * 0.5) / cell), 0,
				cells_z - 1))


func _add_to_grid(grid: Dictionary, record: Dictionary) -> void:
	var key := _lod_grid_key(record)
	if not grid.has(key):
		grid[key] = []
	(grid[key] as Array).append(record)


## Registros das células que cobrem `radius` em volta da câmera (em espaço
## local). Devolve os arrays das células, sem copiar registro nenhum.
func _lod_records(grid: Dictionary, camera_local: Vector3,
		radius: float) -> Array:
	if grid.is_empty():
		return []
	var cell := PLANE_SIZE.x / float(maxi(_grid_cells, 1))
	var cells_z := maxi(1, roundi(PLANE_SIZE.y / cell))
	var span := maxi(1, ceili(radius / cell))
	var cx := floori((camera_local.x + PLANE_SIZE.x * 0.5) / cell)
	var cz := floori((camera_local.z + PLANE_SIZE.y * 0.5) / cell)
	var out: Array = []
	for iz in range(cz - span, cz + span + 1):
		if iz < 0 or iz >= cells_z:
			continue
		for ix in range(cx - span, cx + span + 1):
			var bucket: Variant = grid.get(Vector2i(wrapi(ix, 0, _grid_cells), iz))
			if bucket != null:
				out.append_array(bucket)
	return out




## Layout dos rótulos na tela, SEM deslocamento de grupos e SEM linhas-guia:
##  - cidades em ordem de prioridade (hover, capitais, população); o pool de
##    Label3D é reutilizado, então qualquer cidade pode ter nome;
##  - se o texto colidir com um bloco já aceito (grade espacial), a cidade menor
##    fica só com o ícone (o nome reaparece ao aproximar ou no hover);
##  - portos são ícones de âncora no ponto de costa, com ajuste local de no
##    máximo `PORT_MAX_OFFSET_PX` para não entrar no rótulo da própria cidade;
##    o porto da própria cidade que encosta no selo dela vira badge pequeno
##    encostado no ícone (fora do nome e da área urbana).
func _layout_place_labels(camera: Camera3D) -> void:
	_label_rects.clear()
	for record in _active_labels:
		record["label"] = null
	_active_labels.clear()
	_label_free.clear()
	for label in _label_pool:
		_label_free.append(label)
	_label_grid.clear()
	var viewport := Rect2(Vector2.ZERO,
			camera.get_viewport().get_visible_rect().size)

	var entries: Array = []
	# Bloco de tela de cada cidade desenhada, para o porto da própria cidade
	# saber se está por cima do selo/nome e virar badge.
	var city_blocks: Dictionary = {}
	# Só as cidades com rótulo aceso neste frame (lista montada no LOD): o
	# layout não varre os 650 registros nem projeta quem está apagado.
	for record in _visible_city_records:
		var metrics := _city_metrics(record, camera)
		if metrics.is_empty():
			continue
		# Guarda os retângulos já no dicionário da entrada: o segundo laço não
		# toca mais no dicionário de métricas (que pode vir vazio/incompleto se a
		# projeção mudar no meio do frame).
		var icon_rect := _metrics_rect(metrics, "icon_rect")
		var text_rect := _metrics_rect(metrics, "text_rect")
		if icon_rect.size.x <= 0.0 or text_rect.size.x <= 0.0:
			continue
		entries.append({
			"record": record,
			"icon_rect": icon_rect,
			"text_rect": text_rect,
			"capital": bool(record.get("capital", false)),
			"hovered": String(record["id"]) == _hovered_id,
			"priority": float(record.get("priority", 0.0)),
		})
	entries.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		if bool(a["hovered"]) != bool(b["hovered"]):
			return bool(a["hovered"])
		if bool(a["capital"]) != bool(b["capital"]):
			return bool(a["capital"])
		return float(a["priority"]) > float(b["priority"])
	)

	for entry in entries:
		var record: Dictionary = entry["record"]
		var icon_rect: Rect2 = entry["icon_rect"]
		var text_rect: Rect2 = entry["text_rect"]
		var text_allowed := true
		# Texto quase invisível (fim do fade) não entra: o ícone fica, o nome não.
		if float(record.get("label_alpha", 1.0)) < LABEL_TEXT_MIN_ALPHA:
			text_allowed = false
		# Capitais são aceitas primeiro (ordem acima); a partir daí TODA etiqueta
		# testa colisão — só o hover fura a fila. Antes a capital ignorava a
		# colisão e Roma/Vaticano e as capitais dos Bálcãs desenhavam nomes um
		# sobre o outro com o cenário de 66 nações.
		if text_allowed and not bool(entry["hovered"]):
			text_allowed = not _grid_hits(_label_grid, text_rect,
					LABEL_COLLIDE_PAD_PX)
		var label: Label3D = null
		if text_allowed:
			label = _take_label()
			if label == null:
				# Pool esgotado: as cidades de menor prioridade ficam só com o
				# ícone, como na colisão.
				text_allowed = false
		if label != null:
			_configure_city_label(label, record)
			_active_labels.append(record)
		var block: Rect2 = icon_rect
		if text_allowed:
			block = block.merge(text_rect)
		_grid_insert(_label_grid, block)
		var icon: Sprite3D = record.get("icon")
		var flag: Sprite3D = record.get("flag")
		_label_rects.append({
			"id": String(record.get("id", "")),
			"kind": "city",
			"rect": block,
			"text_rect": text_rect,
			"icon_rect": icon_rect,
			"text_visible": text_allowed,
			"marker_visible": icon == null or icon.visible,
			"flag_visible": flag != null and flag.visible,
			"capital": bool(record.get("capital", false)),
			"population": float(record.get("priority", 0.0)),
			"same_city": false,
			"fused": false,
			"badge": false,
			"city_id": "",
			"offset": Vector2.ZERO,
		})
		city_blocks[String(record.get("id", ""))] = {
			"block": block,
			"icon": icon_rect,
		}

	# Portos por último: nunca deslocam rótulos, só o próprio ícone. A lista já
	# vem filtrada pelo LOD do frame (selo aceso), não os 650 registros.
	_drawn_ports.clear()
	for record in _visible_port_records:
		record["draw"] = false
		record["badge"] = false
		if float(record.get("label_alpha", 0.0)) <= 0.01:
			# Fora da faixa de fade: não entra no relatório nem gasta projeção.
			continue
		var metrics := _port_metrics(record, camera)
		if _metrics_rect(metrics).size.x <= 0.0:
			# Porto atrás da câmera/fora do frustum: não desenha neste frame.
			_append_port_report(record, Rect2(), false, Vector2.ZERO)
			continue
		var city_info: Dictionary = city_blocks.get(
				String(record.get("city_id", "")), {})
		# Porto da própria cidade encostado no selo dela: badge pequeno ao lado do
		# ícone (o nome fica à direita). Nunca sobre o ícone, o nome ou a área
		# urbana; o modelo 3D continua na coordenada real de costa.
		if bool(record.get("fused", false)) \
				or _port_badge_needed(record, metrics, city_info):
			record["badge"] = true
			if not city_info.is_empty() \
					and _place_port_badge(record, city_info, camera):
				var badge_rect := _metrics_rect(_port_metrics(record, camera))
				if badge_rect.size.x > 0.0:
					record["draw"] = true
					_drawn_ports.append(record)
					_grid_insert(_label_grid, badge_rect)
					_append_port_report(record, badge_rect, true, Vector2.ZERO)
					continue
			# Sem selo da cidade na tela ou sem vaga ao lado dele: o modelo 3D
			# fica na costa e o ícone não é desenhado neste frame.
			_append_port_report(record, Rect2(), false, Vector2.ZERO)
			continue
		var chosen := Vector2.ZERO
		# A posição de costa é o candidato preferido; só projeta o lado do
		# mar (e os deslocamentos) quando ela de fato colide.
		var coast_rect := _metrics_rect(metrics)
		if _grid_hits(_label_grid, coast_rect, 1.0):
			var found := false
			for candidate in _port_offset_candidates(record, camera):
				var shifted := Rect2(coast_rect.position + candidate,
						coast_rect.size)
				if not _grid_hits(_label_grid, shifted, 1.0):
					chosen = candidate
					found = true
					break
			if not found:
				# Sem espaço local: fica só o modelo 3D do porto na costa e o
				# ícone da cidade; nada de linha.
				_append_port_report(record, Rect2(), false, Vector2.ZERO)
				continue
			if not chosen.is_zero_approx():
				_apply_port_offset(record, chosen, camera)
				metrics = _port_metrics(record, camera)
				# O deslocamento pode tirar o porto do frustum (borda da tela,
				# encosta atrás da câmera): sem retângulo, nada de desenhar.
				if _metrics_rect(metrics).size.x <= 0.0:
					_append_port_report(record, Rect2(), false, Vector2.ZERO)
					continue
		var final_rect := _metrics_rect(metrics)
		if final_rect.size.x <= 0.0:
			_append_port_report(record, Rect2(), false, Vector2.ZERO)
			continue
		record["draw"] = true
		_drawn_ports.append(record)
		_grid_insert(_label_grid, final_rect)
		_append_port_report(record, final_rect, true, chosen)

	# Rótulos que voltaram ao pool não podem ficar na tela com o texto da cidade
	# anterior (fantasmas ao reduzir o conjunto visível). Esconde só os livres.
	for label in _label_free:
		if label.visible:
			label.visible = false


## Pega um Label3D livre do pool.
func _take_label() -> Label3D:
	if _label_free.is_empty():
		return null
	return _label_free.pop_back()


## Escreve o nome/estilo da cidade no rótulo do pool.
func _configure_city_label(label: Label3D, record: Dictionary) -> void:
	label.text = _city_text(record)
	label.font_size = int(record["font_size"])
	label.pixel_size = float(record["pixel_size"])
	label.outline_size = int(record["font_size"] * 0.4)
	var base := LABEL_HOVER_COLOR if String(record["id"]) == _hovered_id \
			else LABEL_COLOR
	label.modulate = Color(base.r, base.g, base.b,
			float(record.get("label_alpha", 1.0)))
	label.position = record["text_center"]
	label.visible = true
	record["label"] = label


## Célula da grade de ocupação de tela.
static func _grid_key(point: Vector2) -> Vector2i:
	return Vector2i(floori(point.x / LABEL_GRID_PX),
			floori(point.y / LABEL_GRID_PX))


## Verdadeiro se `rect` (com folga) encosta em algum bloco já aceito.
func _grid_hits(grid: Dictionary, rect: Rect2, pad: float) -> bool:
	var r := rect.grow(pad)
	var first := _grid_key(r.position)
	var last := _grid_key(r.end)
	for x in range(first.x, last.x + 1):
		for y in range(first.y, last.y + 1):
			var bucket: Array = grid.get(Vector2i(x, y), [])
			for other in bucket:
				if r.intersects(other):
					return true
	return false


## Insere um bloco ocupado nas células que ele cobre.
func _grid_insert(grid: Dictionary, rect: Rect2) -> void:
	var first := _grid_key(rect.position)
	var last := _grid_key(rect.end)
	for x in range(first.x, last.x + 1):
		for y in range(first.y, last.y + 1):
			var key := Vector2i(x, y)
			if not grid.has(key):
				grid[key] = []
			(grid[key] as Array).append(rect)


## Posição de mundo de um ponto da cidade, já na cópia do wrap mais próxima.
func _display_world(record: Dictionary) -> Vector3:
	return to_global(record["position"] + Vector3(record.get("wrap_x", 0.0), 0.0, 0.0))


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
		"fused": _port_is_badge(record),
		"badge": bool(record.get("badge", false)),
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


## Métricas do rótulo de cidade no frame: retângulo do ícone e do texto. Não
## depende do Label3D estar atribuído (o layout decide a colisão antes de pegar
## um rótulo do pool).
func _city_metrics(record: Dictionary, camera: Camera3D) -> Dictionary:
	var wrap := float(record.get("wrap_x", 0.0))
	var anchor_world: Vector3 = to_global(record["anchor"] + Vector3(wrap, 0.0, 0.0))
	if camera.is_position_behind(anchor_world):
		return {}
	var em := float(record["font_size"]) * float(record["pixel_size"])
	var icon_h := em * PLACE_ICON_HEIGHT
	var icon_rect := _world_rect_to_screen(camera, anchor_world,
			icon_h * 0.5, icon_h * 0.5)
	var text_world: Vector3 = to_global(record["text_center"])
	if camera.is_position_behind(text_world):
		return {}
	var text_w := _city_text_width(record)
	var text_rect := _world_rect_to_screen(camera, text_world,
			text_w * 0.5, em * 0.62)
	# Só descarta o rótulo quando NEM o ícone NEM o texto aparecem na tela. A
	# checagem anterior era a âncora dentro do frustum: uma capital com o nome
	# encostado na borda (ou o ícone 1 u acima do ponto do mapa) sumia mesmo
	# tendo pixels visíveis, e o layout ficava sem a capital "na tela".
	var screen := Rect2(Vector2.ZERO,
			camera.get_viewport().get_visible_rect().size).grow(LABEL_SCREEN_MARGIN_PX)
	if not screen.intersects(icon_rect) and not screen.intersects(text_rect):
		return {}
	return {"icon_rect": icon_rect, "text_rect": text_rect}


## Retângulo de tela de um dicionário de métricas. Dicionário vazio ou sem a
## chave (porto atrás da câmera, fora do frustum ou depois de um deslocamento)
## devolve um `Rect2` vazio — o layout trata isso como "não desenha neste frame"
## em vez de estourar acesso a chave inexistente.
static func _metrics_rect(metrics: Dictionary, key: String = "rect") -> Rect2:
	var value: Variant = metrics.get(key)
	return value if typeof(value) == TYPE_RECT2 else Rect2()


## O selo do porto é um badge da própria cidade (estático ou deste frame)?
static func _port_is_badge(record: Dictionary) -> bool:
	return bool(record.get("fused", false)) or bool(record.get("badge", false))


## O porto da própria cidade encosta no selo/nome dela na tela? Os portos do
## ports.csv com o mesmo id/nome ficam dentro do tecido urbano (até ~4,5 u):
## esses viram badge independente do zoom.
func _port_badge_needed(record: Dictionary, metrics: Dictionary,
		city_info: Dictionary) -> bool:
	if String(record.get("city_id", "")) == "" or city_info.is_empty():
		return false
	if bool(record.get("same_city", false)):
		var city: Dictionary = _records_by_id.get(
				String(record.get("city_id", "")), {})
		if not city.is_empty():
			var port_local: Vector3 = record.get("position", Vector3.ZERO)
			var city_local: Vector3 = city.get("position", Vector3.ZERO)
			if port_local.distance_to(city_local) <= PORT_OWN_BADGE_DISTANCE:
				return true
	var icon: Rect2 = city_info.get("icon", Rect2())
	var block: Rect2 = city_info.get("block", Rect2())
	if icon.size.x <= 0.0 or block.size.x <= 0.0:
		return false
	var rect := _metrics_rect(metrics)
	if rect.size.x <= 0.0:
		return false
	if rect.grow(PORT_BADGE_GAP_PX).intersects(block):
		return true
	return rect.get_center().distance_to(icon.get_center()) <= PORT_BADGE_MERGE_PX


## Posiciona o badge do porto no canto inferior esquerdo do ícone da cidade
## (o nome fica à direita), tentando os quatro lados e aceitando só uma vaga
## livre na grade. Devolve false se nenhum lado cabe — aí o ícone não é
## desenhado neste frame, mas o modelo 3D do porto continua na costa.
func _place_port_badge(record: Dictionary, city_info: Dictionary,
		camera: Camera3D) -> bool:
	var icon: Rect2 = city_info.get("icon", Rect2())
	if icon.size.x <= 0.0:
		return false
	var city: Dictionary = _records_by_id.get(
			String(record.get("city_id", "")), {})
	if city.is_empty():
		return false
	var badge_h := icon.size.y * PORT_BADGE_SCALE
	var step := icon.size.y * 0.5 + badge_h * 0.5 + PORT_BADGE_GAP_PX
	var anchor: Vector3 = city["anchor"] \
			+ Vector3(float(city.get("wrap_x", 0.0)), 0.0, 0.0)
	for direction: Vector2 in [Vector2(-1.0, 1.0), Vector2(-1.0, -1.0),
			Vector2(0.0, 1.0), Vector2(0.0, -1.0)]:
		record["marker_world"] = anchor
		_apply_port_offset(record, direction * step, camera)
		var rect := _metrics_rect(_port_metrics(record, camera))
		if rect.size.x <= 0.0:
			continue
		if not _grid_hits(_label_grid, rect, 1.0):
			# Guarda a posição sem o wrap: o LOD reaplica a cópia mais próxima
			# em todo frame, inclusive nos frames sem layout.
			record["badge_local"] = record["marker_world"] \
					- Vector3(float(record.get("wrap_x", 0.0)), 0.0, 0.0)
			return true
	return false


## Métricas do ícone de porto no frame (posição já com wrap e deslocamento).
func _port_metrics(record: Dictionary, camera: Camera3D) -> Dictionary:
	var marker_world: Vector3 = to_global(record["marker_world"])
	if camera.is_position_behind(marker_world) \
			or not camera.is_position_in_frustum(marker_world):
		return {}
	var icon_h := _port_icon_height(record)
	return {"rect": _world_rect_to_screen(camera, marker_world,
			icon_h * 0.5, icon_h * 0.5)}


## Altura do ícone de porto em unidades de mundo no frame.
func _port_icon_height(record: Dictionary) -> float:
	var em := LABEL_SIZE_STANDARD * float(record.get("pixel_size",
			LABEL_PIXEL_PER_UNIT * 52.0))
	return em * (PORT_BADGE_SCALE if _port_is_badge(record) \
			else PORT_ICON_HEIGHT)



## Candidatos de ajuste local do porto, em pixels de tela: o ponto de costa e
## deslocamentos curtos para o lado do mar (e perpendiculares), até 12 px.
func _port_offset_candidates(record: Dictionary, camera: Camera3D) -> Array[Vector2]:
	var candidates: Array[Vector2] = [Vector2.ZERO]
	var sea := Vector2(0.0, -1.0)
	var base_world: Vector3 = to_global(record["marker_world"])
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
	var base_world: Vector3 = to_global(record["marker_world"])
	var basis := camera.global_transform.basis
	var px_x := _screen_px_per_world(camera, base_world, basis.x)
	var px_y := _screen_px_per_world(camera, base_world, basis.y)
	if px_x <= 0.0 or px_y <= 0.0:
		return
	var delta: Vector3 = basis.x * (offset.x / px_x) - basis.y * (offset.y / px_y)
	record["marker_world"] = record["marker_world"] + delta


## Pixels de tela por unidade de mundo numa direção, na profundidade do ponto.
func _screen_px_per_world(camera: Camera3D, point: Vector3, axis: Vector3) -> float:
	var a := camera.unproject_position(point)
	var b := camera.unproject_position(point + axis)
	return a.distance_to(b)


## Escala e fade do rótulo da cidade. O tamanho em tela é constante (pixel_size
## proporcional à distância); o que muda no zoom out é a faixa de fade por
## categoria: capitais duram mais, cidades pequenas somem antes. A faixa já vem
## calculada no build (`fade_start`/`fade_end`).
func _apply_place_label_lod(record: Dictionary, distance: float) -> void:
	record["pixel_size"] = clampf(LABEL_PIXEL_PER_UNIT * distance,
			LABEL_PIXEL_MIN, LABEL_PIXEL_MAX)
	var label_alpha := 1.0 - _fade_in(distance, float(record["fade_start"]),
			float(record["fade_end"]))
	record["label_alpha"] = label_alpha

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
## com fade de cidade pequena (some no zoom out). O ícone é MultiMesh; o modelo
## 3D nasce sob demanda quando a câmera chega perto E o porto está na tela.
## Longe de tudo (`lod_end`), o registro "assenta": nenhum campo é reescrito até
## a câmera voltar para o raio.
func _apply_port_lod(record: Dictionary, distance: float,
		on_screen: bool) -> void:
	if distance > float(record["lod_end"]):
		if bool(record.get("lod_settled", false)):
			return
		record["lod_settled"] = true
	else:
		record["lod_settled"] = false
	record["pixel_size"] = clampf(LABEL_PIXEL_PER_UNIT * distance,
			LABEL_PIXEL_MIN, LABEL_PIXEL_MAX)
	var alpha := 1.0 - _fade_in(distance, float(record["fade_start"]),
			float(record["fade_end"]))
	record["label_alpha"] = alpha
	record["marker_world"] = record["marker_base"] \
			+ Vector3(record["wrap_x"], 0.0, 0.0)
	# Badge da própria cidade: usa a posição ao lado do ícone calculada no
	# layout, para o selo não voltar para cima da cidade nos frames sem layout.
	if _port_is_badge(record) and record.has("badge_local"):
		record["marker_world"] = record["badge_local"] \
				+ Vector3(record["wrap_x"], 0.0, 0.0)
	if alpha > 0.01:
		_visible_port_records.append(record)
	_update_port_model(record, distance, on_screen)


func _apply_city_lod(record: Dictionary, distance: float,
		on_screen: bool) -> void:
	if distance > float(record["lod_end"]):
		if bool(record.get("lod_settled", false)):
			return
		record["lod_settled"] = true
	else:
		record["lod_settled"] = false
	_apply_place_label_lod(record, distance)
	if float(record["label_alpha"]) > 0.01:
		_visible_city_records.append(record)

	# Marcador discreto (continental): aparece só quando a cidade já sumiu.
	var discs: Array = record["discs"]
	if not discs.is_empty():
		var marker_alpha := _fade_in(distance, MARKER_FADE_IN_START,
				MARKER_FADE_IN_END)
		for disc in discs:
			var mesh := disc as MeshInstance3D
			mesh.visible = marker_alpha > 0.01
			mesh.transparency = 1.0 - marker_alpha

	_update_urban(record, distance, on_screen)



## Rampa de entrada: 0 antes de `start`, 1 após `end`.
func _fade_in(distance: float, start: float, end: float) -> float:
	return clampf((distance - start) / (end - start), 0.0, 1.0)


## Pede a construção da cidade modular quando a câmera se aproxima E a cidade
## está na tela (construir fora do frustum só gerava picos de 5 ms por frame à
## toa). Depois de construída, só alterna a visibilidade (histerese evita
## reconstrução em vaivém).
##
## Só as cidades detalhadas (as 40 maiores) têm modelo 3D. As demais mostram
## apenas ícone + nome em qualquer zoom: sem tecido urbano, sem solo orgânico e
## sem o disco raso que virava uma mancha clara sob o marcador.
func _update_urban(record: Dictionary, distance: float,
		on_screen: bool) -> void:
	var urban: Node3D = record.get("urban")
	if not bool(record.get("detailed", false)):
		if urban != null:
			urban.visible = false
		return
	if urban == null:
		if on_screen and distance <= URBAN_BUILD_DISTANCE \
				and not bool(record.get("urban_queued", false)):
			record["urban_queued"] = true
			_urban_queue.append(record)
		return
	var want_visible := distance <= CITY_VISIBLE_DISTANCE
	if urban.visible != want_visible:
		urban.visible = want_visible
	var target: Vector3 = record["position"] \
			+ Vector3(record["wrap_x"], 0.0, 0.0) + Vector3(0.0, 0.16, 0.0)
	if not urban.position.is_equal_approx(target):
		urban.position = target


## Consome as filas de construção detalhada (cidade e porto) com orçamento de
## 1,5 ms por frame. Cada modelo entra no máximo um por frame.
func _process_lod_queues(camera: Camera3D) -> void:
	if _urban_queue.is_empty() and _port_model_queue.is_empty():
		return
	var camera_local := to_local(camera.global_position)
	var start := Time.get_ticks_usec()
	while not _urban_queue.is_empty():
		var record: Dictionary = _urban_queue.pop_front()
		record["urban_queued"] = false
		if record.get("urban") != null:
			continue
		if _camera_distance(record, camera_local) > URBAN_BUILD_DISTANCE:
			continue
		if not _in_frustum(record, camera):
			continue
		record["urban"] = _build_urban(record)
		if Time.get_ticks_usec() - start >= LOD_BUILD_BUDGET_USEC:
			break
	while not _port_model_queue.is_empty():
		var record: Dictionary = _port_model_queue.pop_front()
		record["model_queued"] = false
		if record.get("model") != null:
			continue
		if _camera_distance(record, camera_local) > PORT_MODEL_DISTANCE:
			continue
		if not _in_frustum(record, camera):
			continue
		_build_port_model(record)
		if Time.get_ticks_usec() - start >= LOD_BUILD_BUDGET_USEC:
			break
	_trim_port_models(camera_local)


## Distância da câmera ao registro, já resolvendo o wrap em X e incluindo a
## altura da câmera — a mesma referência do LOD e das faixas de fade.
static func _camera_distance(record: Dictionary, camera_local: Vector3) -> float:
	var position: Vector3 = record["position"]
	var dx := position.x - camera_local.x
	dx -= roundf(dx / PLANE_SIZE.x) * PLANE_SIZE.x
	var dy := position.y - camera_local.y
	var dz := position.z - camera_local.z
	return sqrt(dx * dx + dy * dy + dz * dz)


## LOD de um porto: wrap, distância e (só perto) checagem de frustum.
func _apply_lod_port(record: Dictionary, camera_local: Vector3,
		camera: Camera3D) -> void:
	var position: Vector3 = record["position"]
	var dx := position.x - camera_local.x
	var wrap := -roundf(dx / PLANE_SIZE.x) * PLANE_SIZE.x
	record["wrap_x"] = wrap
	dx += wrap
	var dz := position.z - camera_local.z
	# Distância 3D (inclui a altura da câmera): é a mesma referência das faixas
	# de fade aprovadas no mapa — a planar mantinha nomes pequenos acesos no
	# zoom out.
	var dy := position.y - camera_local.y
	var distance := sqrt(dx * dx + dy * dy + dz * dz)
	# O teste de frustum só decide se um modelo AINDA INEXISTENTE entra na fila;
	# com o modelo já construído, `_update_port_model` só usa a distância.
	var on_screen := distance <= PORT_MODEL_DISTANCE \
			and record.get("model") == null and _in_frustum(record, camera)
	_apply_port_lod(record, distance, on_screen)


## LOD de uma cidade: wrap, distância e (só perto) checagem de frustum.
func _apply_lod_city(record: Dictionary, camera_local: Vector3,
		camera: Camera3D) -> void:
	var position: Vector3 = record["position"]
	var dx := position.x - camera_local.x
	var wrap := -roundf(dx / PLANE_SIZE.x) * PLANE_SIZE.x
	record["wrap_x"] = wrap
	dx += wrap
	var dz := position.z - camera_local.z
	# Mesma distância 3D do porto/cidade (ver `_apply_lod_port`).
	var dy := position.y - camera_local.y
	var distance := sqrt(dx * dx + dy * dy + dz * dz)
	# Só testa o frustum para decidir a entrada de uma cidade ainda não
	# construída; com `urban` pronto, `_update_urban` ignora `on_screen`.
	# Cidade sem modelo detalhado nunca entra na fila: nem projeta frustum.
	var on_screen := bool(record.get("detailed", false)) \
			and distance <= URBAN_BUILD_DISTANCE \
			and record.get("urban") == null and _in_frustum(record, camera)
	_apply_city_lod(record, distance, on_screen)


func _build_urban(record: Dictionary) -> Node3D:
	var city: Dictionary = record["city"]
	var manager := UrbanGrowthScript.new() as Node3D
	manager.name = "Urban_" + String(record["id"])
	manager.position = record["position"] \
			+ Vector3(record["wrap_x"], 0.0, 0.0) + Vector3(0.0, 0.16, 0.0)
	manager.scale = Vector3.ONE * URBAN_SCALE
	_places.add_child(manager)

	manager.setup(String(record["id"]))
	# Crescimento orgânico só em terra firme e dentro do território da nação.
	manager.buildable_filter = _make_urban_filter(
			manager.position, String(city.get("nation", "")))
	manager.apply_indicators({
		"population": float(city.get("population", 0.0)),
		"industrialization": float(city.get("industrialization", 0.0)),
		"income": float(city.get("income", 0.5)),
		"jobs": float(city.get("jobs", 0.0)),
	})
	manager.apply_industry(IndustryProfileScript.derive(city))
	return manager


## Modelo 3D do porto: criado uma vez, sob demanda, e reutilizado perto da costa.
func _update_port_model(record: Dictionary, distance: float,
		on_screen: bool) -> void:
	var model: Node3D = record.get("model")
	if model == null:
		if on_screen and distance <= PORT_MODEL_DISTANCE \
				and not bool(record.get("model_queued", false)):
			record["model_queued"] = true
			_port_model_queue.append(record)
		return
	model.visible = distance <= PORT_MODEL_VISIBLE_DISTANCE
	model.position = record["position"] + Vector3(record["wrap_x"], 0.0, 0.0)


func _build_port_model(record: Dictionary) -> void:
	var model := PORT_SCENE.instantiate() as Node3D
	_places.add_child(model)
	model.position = record["position"] + Vector3(record["wrap_x"], 0.0, 0.0)
	var bounds := _combined_aabb(model)
	var longest := maxf(bounds.size.x, maxf(bounds.size.y, bounds.size.z))
	if longest > 0.0:
		model.scale = Vector3.ONE * (PORT_SIZE / longest)
	record["model"] = model
	_live_port_models += 1


## Mantém no máximo `MAX_PORT_MODELS` modelos vivos: libera os mais distantes
## quando a câmera viaja pelo mundo. `camera_local` é a câmera no espaço do mapa
## (mesma referência de `record["position"]`).
func _trim_port_models(camera_local: Vector3) -> void:
	# Contador evita varrer os 650 registros em todo frame de fila ativa (era o
	# maior custo fixo do pan, mesmo com poucos modelos vivos).
	if _live_port_models <= MAX_PORT_MODELS:
		return
	var live: Array[Dictionary] = []
	for record in _port_nodes:
		if record.get("model") != null:
			live.append(record)
	if live.size() <= MAX_PORT_MODELS:
		_live_port_models = live.size()
		return
	live.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return _camera_distance(a, camera_local) > _camera_distance(b, camera_local)
	)
	# Histerese: ao passar do teto, libera até 16 abaixo dele. Sem isso, o trim
	# varria e ordenava os 650 portos em praticamente todo frame de pan.
	var target := maxi(1, MAX_PORT_MODELS - 16)
	var excess := live.size() - target
	if excess <= 0:
		_live_port_models = live.size()
		return
	for index in excess:
		var model: Node3D = live[index]["model"]
		model.queue_free()
		live[index]["model"] = null
	_live_port_models = live.size() - excess


## Atualiza os MultiMesh de ícones distantes: só os visíveis entram no buffer,
## que é compactado (`visible_instance_count`). Nada de nó por porto/cidade — e
## a varredura é só sobre as listas de visíveis do frame, não sobre os 650.
func _apply_icon_transforms(camera: Camera3D) -> void:
	if camera == null:
		return
	# Sem registros (ou sem MultiMesh) não há nada a compactar; evita escrever
	# em buffer nulo quando o build não criou algum dos grupos.
	if _city_icon_mm == null and _city_star_mm == null \
			and _city_ring_mm == null and _port_icon_mm == null \
			and _port_ring_mm == null:
		return
	var plain_index := 0
	var star_index := 0
	var ring_index := 0
	for record in _visible_city_records:
		if bool(record.get("detailed", false)):
			continue
		var world := _city_icon_world(record)
		if camera.is_position_behind(world) \
				or not camera.is_position_in_frustum(world):
			continue
		var size := _city_icon_size(record)
		if bool(record.get("capital", false)):
			_set_mm_transform(_city_star_mm, star_index, world, size)
			star_index += 1
		else:
			_set_mm_transform(_city_icon_mm, plain_index, world, size)
			plain_index += 1
		_set_mm_transform(_city_ring_mm, ring_index, world, size * RING_SCALE)
		ring_index += 1
	_set_mm_count(_city_icon_mm, plain_index)
	_set_mm_count(_city_star_mm, star_index)
	_set_mm_count(_city_ring_mm, ring_index)

	var port_index := 0
	var port_ring := 0
	for record in _drawn_ports:
		var world: Vector3 = to_global(record["marker_world"])
		var size := _port_icon_height(record)
		_set_mm_transform(_port_icon_mm, port_index, world, size)
		port_index += 1
		if not _port_is_badge(record):
			_set_mm_transform(_port_ring_mm, port_ring, world, size * RING_SCALE)
			port_ring += 1
	_set_mm_count(_port_icon_mm, port_index)
	_set_mm_count(_port_ring_mm, port_ring)


func _city_icon_size(record: Dictionary) -> float:
	if float(record.get("label_alpha", 0.0)) <= 0.01:
		return 0.0
	var em := float(record["font_size"]) * float(record["pixel_size"])
	return em * PLACE_ICON_HEIGHT


func _city_icon_world(record: Dictionary) -> Vector3:
	return to_global(record["anchor"] + Vector3(record["wrap_x"], 0.0, 0.0))


func _set_mm_transform(instance: MultiMeshInstance3D, index: int,
		world: Vector3, size: float) -> void:
	if instance == null or instance.multimesh == null or index < 0:
		return
	# Buffer curto (ou vazio): instância fora do intervalo é ignorada em vez de
	# derrubar o frame com erro da engine.
	if index >= instance.multimesh.instance_count:
		return
	var local := instance.to_local(world)
	instance.multimesh.set_instance_transform(index,
			Transform3D(Basis().scaled(Vector3(size, size, size)), local))


func _set_mm_count(instance: MultiMeshInstance3D, count: int) -> void:
	if instance == null or instance.multimesh == null:
		return
	instance.multimesh.visible_instance_count = clampi(count, 0,
			instance.multimesh.instance_count)



## Filtro orgânico do crescimento: só constrói em terra firme, dentro do
## território da nação e sem encostar nos portos. Com o mundo cilíndrico, o
## lote e a posição do porto entram pela cópia MAIS PRÓXIMA (fposmod em X):
## antes, uma cidade na emenda via o porto do outro lado do mapa como vizinho e
## o lote saía do plano (uv fora de [0,1)) e era descartado.
func _make_urban_filter(city_local: Vector3, nation: String) -> Callable:
	var keepout: Array = []
	for p in _port_nodes:
		var pp: Vector3 = p.get("position", Vector3.ZERO)
		var wrap := roundf((city_local.x - pp.x) / PLANE_SIZE.x) * PLANE_SIZE.x
		keepout.append(Vector2(pp.x + wrap, pp.z))
	return func(lot_local: Vector3) -> bool:
		if _territory == null:
			return true
		var local := city_local + Vector3(lot_local.x * URBAN_SCALE, 0.0,
				lot_local.z * URBAN_SCALE)
		var px := Vector2(
			fposmod(local.x / PIXEL_TO_WORLD + MAP_SIZE_PX.x * 0.5,
					MAP_SIZE_PX.x),
			local.z / PIXEL_TO_WORLD + MAP_SIZE_PX.y * 0.5)
		var uv := Vector2(px.x / MAP_SIZE_PX.x, px.y / MAP_SIZE_PX.y)
		if uv.y < 0.0 or uv.y >= 1.0:
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
