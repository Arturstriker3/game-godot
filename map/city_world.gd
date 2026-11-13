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
const FLAG_SHADER: Shader = preload("res://map/map_shaders/flag_atlas.gdshader")
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
## Raio do anel de hover/seleção em pixels de TELA, em qualquer zoom. É um
## alvo visual fixo: antes o aro crescia com o footprint (1,8 u de fallback) e
## no zoom aproximado virava um anel de centenas de pixels, deslocado do ícone.
const PLACE_RING_RADIUS_PX := 26.0
## O aro fica levemente acima do ponto do chão (z-fighting), mas o centro
## projetado continua no ícone (±2 px até d013).
const PLACE_RING_LIFT := 0.02
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
## move além de `CAMERA_MOVE_EPSILON`, com throttle de ~12 Hz — durante pan/zoom
## rápido o layout é congelado e refeito quando a câmera para. O trabalho é
## fatiado por `LAYOUT_BUDGET_USEC` para não estourar o frame.
const LAYOUT_MIN_INTERVAL := 1.0 / 12.0
const LAYOUT_BUDGET_USEC := 1200
## Varredura do LOD que passou deste custo já gastou o frame: o layout do ciclo
## entra no frame seguinte (continua fatiado), em vez de somar dois picos.
const SCAN_DEFER_USEC := 1400
const CAMERA_MOVE_EPSILON := 0.06
## Deslocamento (u) num único frame que conta como teleporte/recentralização:
## roda varredura e layout completos já neste frame (o teste e o clique no HUD
## esperam a tela pronta). Pan normal anda bem menos que isso por frame.
const LAYOUT_JUMP := 4.0
## No nível LONGE os ícones mudam pouco: o buffer do MultiMesh é reescrito a
## ~4 Hz durante o pan e imediatamente quando a câmera para.
const FAR_ICON_INTERVAL := 0.25

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
## Tecido urbano vale para TODAS as cidades (858) e é construído sob demanda,
## pela mesma fila com orçamento. O teto de modelos vivos evita memória crescendo
## sem limite em pans longos; a histerese segura os que foram liberados (sem
## reconstruir em vaivém enquanto a câmera não se afastar).
const MAX_URBAN_MODELS := 96
const URBAN_TRIM_HYSTERESIS := 16
## Distância (u) que a câmera precisa variar para um tecido liberado poder
## voltar a ser construído: evita o vaivém quando a fila está no teto.
const URBAN_HOLD_RELEASE := 16.0
## Base do modelo urbano acima do plano do mapa (o tecido se funde ao terreno).
const URBAN_GROUND_LIFT := 0.05
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
## LOD de zoom em 3 níveis, com histerese (não pisca na fronteira) e fade curto
## de 150 ms. A distância é a da câmera ao ponto do chão que ela mira — a mesma
## referência do enquadramento (d=52 perto, 120 médio, 260 longe).
##   LONGE  (estratégico) > 178 u: só as CAPITAIS (estrela, sem selo), sem nomes,
##           bandeiras, portos, tecido urbano ou veículos; as demais cidades
##           não têm marcador nenhum.
##   MÉDIO  (66..168)      : só capitais (bandeira + estrela + nome) e cidades da
##           nação do jogador (ícone normal + nome); as demais não têm marcador
##           nenhum.
##   PERTO  (<= 74)        : todos os nomes, ícones normais, portos, construções,
##           tecido e veículos.
## A histerese separa entrada e saída: 66/74 no perto e 168/178 no médio.
const ZOOM_NEAR_ENTER := 66.0
const ZOOM_NEAR_EXIT := 74.0
const ZOOM_MID_ENTER := 168.0
const ZOOM_MID_EXIT := 178.0
const LEVEL_FADE_SECONDS := 0.15

## Declutter por célula de tela: no máximo N nomes por célula de 64 px (além da
## colisão retângulo a retângulo). Capital > nação do jogador > população.
const LABEL_MAX_PER_CELL := 4

## No LONGE a estrela da capital fica ~20% maior; não há selo atrás dela.
const FAR_CAPITAL_ICON_SCALE := 1.2

## Bandeiras: atlas único + MultiMesh (uma draw call). Cada célula copia a
## textura da nação (40×27, proporção real das PNGs) e o shader escolhe a célula
## por `INSTANCE_CUSTOM`.
const FLAG_CELL := Vector2i(40, 27)
const FLAG_ATLAS_COLS := 16

## Nomes de nação gravados no mapa. A posição vem do polo de inacessibilidade do
## maior pedaço contíguo (TerritoryMap) e o tamanho das dimensões úteis do
## território; o encaixe é validado contra o raster de dono. A visibilidade é por
## TAMANHO NA TELA (px) com histerese e fade de 150 ms. O layout de cidade
## (capitais/bandeiras) tem prioridade: o nome cede quando colide.
const NATION_LABEL_COLOR := Color(0.07, 0.08, 0.09, 0.68)
const NATION_LABEL_MIN_SCREEN_PX := 40.0
const NATION_LABEL_FADE_SCREEN_PX := 14.0
const NATION_LABEL_SCREEN_PAD := 64.0
const NATION_LABEL_CITY_PAD := 3.0
## Tamanho mínimo/máximo de `pixel_size` do Label3D (unidades de mundo por px da
## fonte). O máximo mantém a carta discreta; o mínimo descarta nomes ilegíveis.
const NATION_LABEL_PIXEL_MIN := 0.006
const NATION_LABEL_PIXEL_MAX := 0.055
## Fade de 150 ms e margens de histerese (entra em 1,1×, sai em 0,9×).
const NATION_LABEL_FADE_TIME := 0.15
const NATION_LABEL_HYST_IN := 1.10
const NATION_LABEL_HYST_OUT := 0.90
## No LONGE, nomes no chão só para países grandes; os pequenos mostram o nome
## em caixa alta Barlow sob a estrela da capital.
const NATION_LABEL_FAR_FLOOR_MIN_PX := 90.0
const NATION_LABEL_FAR_CAPITAL_MIN_PX := 96.0
const NATION_CAPITAL_LABEL_SIZE := 16
const NATION_CAPITAL_LABEL_SCALE := 0.62

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
var _port_nodes: Array[Dictionary] = []
var _hovered_id := ""
var _mode := "political"
var _selection: MeshInstance3D
var _selection_dot: MeshInstance3D
var _hover: MeshInstance3D
var _selected_id := ""
var _nation_labels: Array[Label3D] = []
var _nation_anchors: Dictionary = {}
## Rótulos de país sob a estrela da capital no LONGE (um por nação com capital).
var _nation_capital_labels: Dictionary = {}
## Capital (registro de cidade) de cada nação, para o rótulo do LONGE.
var _capital_city_by_nation: Dictionary = {}
## Labels com fade em andamento (150 ms) e relatório do último layout.
var _nation_fade_active: Array[Label3D] = []
var _nation_label_report_cache: Array[Dictionary] = []

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
## Modelos urbanos vivos (record["urban"] != null), para o teto de memória.
var _live_urban_models := 0
var _records_by_id: Dictionary = {}
## Listas de VISÍVEIS montadas no LOD de cada frame: o trabalho de layout/ícones
## percorre só elas, não os 650 lugares.
var _visible_city_records: Array = []
## A primeira varredura já rodou? Antes dela as listas estão vazias e os buffers
## de ícone não podem ser "aplicados" (perderiam o alvo e ficariam limpos).
var _scanned_once := false
## Portos com selo aceso neste frame (alpha > 0,01). O layout percorre esta
## lista em vez dos 650 portos: a consulta da grade já filtrou por distância.
var _visible_port_records: Array = []
var _detailed_records: Array = []
var _drawn_ports: Array = []
## Grade espacial do LOD (XZ) e as cidades de raio longo (capitais/grandes), que
## são poucas e ficam fora da grade. `_grid_cells` conta as células em X.
var _city_grid: Dictionary = {}
var _port_grid: Dictionary = {}
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
var _layout_dirty := true
## Nomes de nação só são reavaliados quando a câmera para (ou troca de modo/
## zoom/idioma): durante o pan o custo seria pago a ~12 Hz sem ganho visível.
var _nation_labels_dirty := true
## Um movimento que caiu no throttle precisa de um layout posterior mesmo que a
## câmera já esteja parada (refazer ao parar).
var _layout_pending := false
## A varredura deste ciclo já rodou e o layout ficou para o próximo frame: a
## varredura sozinha já estourou o orçamento, empilhar o layout no mesmo frame
## criava o pico de p95/p99 do pan.
var _layout_after_scan := false
## Acumulador do throttle do layout (segundos) e orçamento do frame em curso.
var _layout_accum := 0.0

## Nível de zoom vigente (enum `ZoomLevel`) e distância da câmera ao chão.
## `_zoom_level = -1` no build: o primeiro frame assenta o nível sem fade.
var _zoom_level := -1
var _zoom_distance := 0.0
## Registros cujo alpha está animando entre níveis (fade de 150 ms).
var _level_fade_queue: Array = []
## Nação do jogador: no nível MÉDIO, só as cidades dela mostram nome além das
## capitais. Vazio = nenhuma nação jogável declarada ainda.
var _player_nation := ""

## Bandeiras: atlas único + MultiMesh. `_flag_records` são as capitais com
## bandeira carregada; o buffer só é reescrito quando algo muda.
var _flag_atlas: ImageTexture
var _flag_uv: Dictionary = {}
var _flag_mm: MultiMeshInstance3D
var _flag_records: Array[Dictionary] = []
var _icons_dirty := true
var _flags_dirty := true
var _icon_accum := 0.0

## Buffers reutilizados pela varredura do LOD (evitam alocar Array por frame).
var _scan_city_records: Array = []
var _scan_port_records: Array = []

## Layout fatiado: entradas de cidade já projetadas, índice de aceitação e
## contagem de nomes por célula de tela.
var _label_cell_counts: Dictionary = {}
var _layout_job_active := false
var _layout_job_phase := 0
var _layout_job_entries: Array = []
var _layout_job_index := 0
var _layout_job_city_blocks: Dictionary = {}
var _layout_job_ports: Array = []
var _layout_job_cities: Array = []
var _layout_job_camera: Camera3D

## Render dos comboios (child criado por `main_game`): visibilidade por nível e
## frustum, checada a ~6 Hz — a posição lógica do tween continua.
var _convoy_renderer: Node = null
var _convoy_timer := 0.0

## Cópias laterais do mundo (wrap em X): o rótulo clicável é o da cópia que
## está de fato na tela, não necessariamente a central. Com o pool de rótulos,
## o texto já é posicionado na cópia mais próxima — o pick testa só a atual.
var _wrap_offsets: Array[float] = [0.0]

## Emitido quando o conjunto de donos muda (o minimapa precisa se atualizar).
signal territory_changed

## Níveis do LOD de zoom (ver constantes ZOOM_*).
enum ZoomLevel { FAR, MID, NEAR }


## Monta o mundo: chão de satélite, camada geopolítica e modelos de cidade.
func build(cities: Array, ports: Array) -> void:
	_load_city_flags()
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
	_build_flag_renderer()
	_init_territory(cities)
	# Os nomes de nação precisam do território já atribuído (âncora/extensão de
	# TODAS as nações, não só a lista legada de nations.json).
	_build_nation_labels()
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
	_nation_labels_dirty = true


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
	var record: Dictionary = _records_by_id.get(place_id, {})
	if not record.is_empty() and not _place_pickable(record):
		return 0.0
	return MapPickScript.pick_radius_px(camera, _place_position(place_id),
			_place_footprint(place_id))


## O lugar responde a hover/clique no nível atual? Fora do PERTO só responde o
## que tem marcador desenhado: no MÉDIO, capitais e cidades da nação do jogador;
## no LONGE, apenas as capitais. Portos só no PERTO. Sem isso, o clique pegaria
## cidades invisíveis (o mesmo lugar das antigas "bolinhas").
func _place_pickable(record: Dictionary) -> bool:
	if _zoom_level == ZoomLevel.NEAR:
		return true
	if String(record.get("kind", "")) != "city":
		return false
	return _city_marker_wanted(record)


## Melhor lugar da lista sob o cursor (score < 1 dentro do raio de tela).
func _pick_place(nodes: Array, screen_position: Vector2, camera: Camera3D) -> String:
	if camera == null:
		return ""
	var best_id := ""
	var best_score := 1.0
	var right := camera.global_transform.basis.x
	for record in nodes:
		if not _place_pickable(record):
			continue
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
		_place_indicator(_selection, _selection_dot, port_id)


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
		_place_indicator(_selection, _selection_dot, city_id)


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
## No LONGE, só a capital tem marcador: hover de outra cidade/porto é ignorado
## (o pick já devolve vazio; o guarda evita anel sobre o que não está desenhado).
func set_hover(place_id: String) -> void:
	if not place_id.is_empty():
		var record: Dictionary = _records_by_id.get(place_id, {})
		if not record.is_empty() and not _place_pickable(record):
			place_id = ""
	# Cor do nome + ícone: destaca a construção sob o cursor (indica clicável).
	if _hovered_id != place_id:
		_set_label_color(_hovered_id, false)
		_hovered_id = place_id
		_set_label_color(place_id, true)
		# Hover só reordena o layout de cidade/porto. NÃO invalida os nomes de
		# nação: passar o mouse sobre uma cidade não pode fazer os nomes dos
		# países sumirem/voltarem (era o "pisca" ao mexer o mouse).
		_invalidate_layout(false)

	if _selection == null:
		_create_indicators()
	if _hover == null:
		return
	if place_id == _selected_id:
		_hover.visible = false
		return
	_hover.visible = not place_id.is_empty()
	if _hover.visible:
		_place_indicator(_hover, null, place_id)


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


## Posição do centro visual do lugar: o próprio ícone (âncora da cidade ou o
## marcador desenhado do porto), já na cópia do wrap que está na tela. É a
## fonte ÚNICA do anel de hover/seleção e do ponto central do selecionado.
func _indicator_position(place_id: String) -> Vector3:
	var record: Dictionary = _records_by_id.get(place_id, {})
	if record.is_empty():
		return Vector3.ZERO
	if String(record.get("kind", "")) == "city":
		return to_global(record["anchor"]
				+ Vector3(float(record.get("wrap_x", 0.0)), 0.0, 0.0))
	var marker: Vector3 = record.get("marker_world", record["position"])
	return to_global(marker)


## Raio em unidades de mundo de um anel de `PLACE_RING_RADIUS_PX` pixels de
## tela na profundidade do ícone. Fixo em qualquer zoom: o aro acompanha o
## ícone, não o footprint da cidade (que no zoom próximo é muito maior que o
## símbolo desenhado).
func _ring_world_radius(place_id: String) -> float:
	var camera := get_viewport().get_camera_3d()
	if camera == null:
		return _place_footprint(place_id)
	var position := _indicator_position(place_id)
	if camera.is_position_behind(position):
		return 0.5
	var px_per_world := _screen_px_per_world(camera, position,
			camera.global_transform.basis.x)
	if px_per_world <= 0.0:
		return 0.5
	return PLACE_RING_RADIUS_PX / px_per_world


## Coloca anel (e o ponto central opcional) no ícone do lugar, com o raio fixo
## de 26 px. Roda também a cada frame do LOD: o aro continua centrado e do
## mesmo tamanho em tela durante pan/zoom, em vez de congelar na escala em que
## o hover começou.
func _place_indicator(ring: MeshInstance3D, dot: MeshInstance3D,
		place_id: String) -> void:
	if ring == null or place_id.is_empty():
		return
	var radius := _ring_world_radius(place_id)
	var base := _indicator_position(place_id) + Vector3(0.0, PLACE_RING_LIFT, 0.0)
	ring.scale = Vector3(radius, 1.0, radius)
	ring.global_position = base
	if dot != null:
		dot.global_position = base + Vector3(0.0, 0.03, 0.0)


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


## Nomes de TODAS as nações com território, gravados no chão, como numa carta.
## A posição/tamanho vêm do modelo territorial (maior pedaço contíguo + polo de
## inacessibilidade + PCA), com nome completo → curto → oculto.
func _build_nation_labels() -> void:
	# Conecta antes de qualquer retorno: os rótulos de cidade também dependem
	# deste sinal para acompanhar o idioma.
	if not I18n.locale_changed.is_connected(_on_locale_changed):
		I18n.locale_changed.connect(_on_locale_changed)
	_nation_anchors.clear()
	if FileAccess.file_exists(ATLAS_NATIONS_PATH):
		var anchors: Variant = JSON.parse_string(
				FileAccess.get_file_as_string(ATLAS_NATIONS_PATH))
		if typeof(anchors) == TYPE_DICTIONARY:
			_nation_anchors = anchors

	var tags := _nation_label_tags()
	if tags.is_empty():
		return

	var holder := Node3D.new()
	holder.name = "NationLabels"
	_content.add_child(holder)
	_nation_labels.clear()
	for tag: String in tags:
		var label := Label3D.new()
		label.name = "Nation_" + tag
		label.set_meta("tag", tag)
		label.font_size = 64
		label.outline_size = 0
		label.modulate = Color(NATION_LABEL_COLOR.r, NATION_LABEL_COLOR.g,
				NATION_LABEL_COLOR.b, 0.0)
		label.billboard = BaseMaterial3D.BILLBOARD_DISABLED
		label.double_sided = false
		label.alpha_cut = Label3D.ALPHA_CUT_DISABLED
		label.render_priority = 2
		label.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
		label.visible = false
		label.set_meta("alpha", 0.0)
		label.set_meta("alpha_target", 0.0)
		holder.add_child(label)
		_nation_labels.append(label)
		_place_nation_label(label)
	_build_nation_capital_labels()


## Tags com nome que devem existir no mapa: as nações do cenário presentes no
## território atual + as âncoras históricas. Tags sem tradução (territórios sem
## nome no i18n, ex. ATA/CLP/CSI/IOA/KAS) ficam de fora — nada de chave crua.
func _nation_label_tags() -> Array[String]:
	var found: Dictionary = {}
	for tag: Variant in _nation_anchors:
		found[String(tag)] = true
	if _territory != null:
		for tag: String in _territory.nation_tags():
			found[tag] = true
	var out: Array[String] = []
	for tag: Variant in found:
		var text := I18n.nation_name(String(tag))
		if text.begins_with("nation."):
			continue
		out.append(String(tag))
	out.sort()
	return out


## Rótulo pequeno Barlow caixa alta sob a estrela da capital. Só aparece no
## LONGE, para nações cujo nome no chão não cabe (Bálcãs, Caribe, Luxemburgo).
func _build_nation_capital_labels() -> void:
	_capital_city_by_nation.clear()
	for record: Dictionary in _city_nodes:
		if not bool(record.get("capital", false)):
			continue
		var nation := String(record.get("nation", ""))
		if nation.is_empty() or _capital_city_by_nation.has(nation):
			continue
		_capital_city_by_nation[nation] = record
	if _capital_city_by_nation.is_empty():
		return

	var holder := Node3D.new()
	holder.name = "NationCapitalLabels"
	_content.add_child(holder)
	_nation_capital_labels.clear()
	for nation: Variant in _capital_city_by_nation:
		var tag := String(nation)
		var label := Label3D.new()
		label.name = "NationCapital_" + tag
		label.set_meta("tag", tag)
		label.font = LABEL_FONT
		label.font_size = NATION_CAPITAL_LABEL_SIZE
		label.pixel_size = 0.02
		label.text = I18n.nation_name(tag).to_upper()
		label.modulate = Color(NATION_LABEL_COLOR.r, NATION_LABEL_COLOR.g,
				NATION_LABEL_COLOR.b, 0.0)
		label.outline_modulate = Color(0.04, 0.055, 0.055, 0.95)
		label.outline_size = 3
		label.no_depth_test = true
		label.render_priority = 11
		label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		label.visible = false
		label.set_meta("alpha", 0.0)
		label.set_meta("alpha_target", 0.0)
		holder.add_child(label)
		_nation_capital_labels[tag] = label


func _on_locale_changed(_locale: String) -> void:
	for root in _content_roots():
		var holder := root.get_node_or_null("NationLabels")
		if holder != null:
			for child in holder.get_children():
				_place_nation_label(child as Label3D)
		var capitals := root.get_node_or_null("NationCapitalLabels")
		if capitals != null:
			for child in capitals.get_children():
				var label := child as Label3D
				if label != null:
					label.text = I18n.nation_name(
							String(label.get_meta("tag"))).to_upper()
	# Nomes das cidades: o pool é reconfigurado no próximo layout; aqui só
	# invalidamos o texto/largura em cache.
	for record in _city_nodes:
		record["text"] = ""
		record["text_width"] = 0.0
		# Força a reancoragem do rótulo: a largura do nome mudou, mas
		# `_place_city_marker` só recalcula quando o passo/wrap mudam.
		record["placed_step"] = -1
	_invalidate_layout()


## Testa um texto do rótulo contra o modelo territorial. Devolve métricas e se
## ele cabe (≥95% do retângulo de amostragem dentro do dono, com tamanho útil).
func _fit_nation_text(label: Label3D, model: Dictionary, text: String) -> Dictionary:
	var width_px := float(model.get("width_px", float(model.get("extent_px", 40.0))))
	var height_px := float(model.get("height_px", width_px * 0.6))
	var font: Font = label.font
	var font_size := label.font_size
	var text_px := font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT,
			-1, font_size).x
	var font_h := font.get_height(font_size)
	var avail_w := width_px * PIXEL_TO_WORLD * 0.85
	var avail_h := height_px * PIXEL_TO_WORLD * 0.80
	var pixel_size := minf(avail_w / maxf(text_px, 1.0),
			avail_h / maxf(font_h, 1.0))
	pixel_size = minf(pixel_size, NATION_LABEL_PIXEL_MAX)
	var angle := float(model.get("angle", 0.0))
	var center_px := Vector2(float(model.get("x", 0.0)), float(model.get("y", 0.0)))
	var inside := 1.0
	if _territory != null:
		# O polo é o centro do maior círculo inscrito, mas um texto comprido
		# pode passar das bordas. Encolhe até 95% do retângulo caber (ou até o
		# piso legível); se nem o piso couber, o nome não é mostrado.
		for _attempt in 10:
			var tw := text_px * pixel_size
			var th := font_h * pixel_size
			inside = _nation_inside_ratio(String(label.get_meta("tag")), center_px,
					angle, tw / PIXEL_TO_WORLD * 0.5,
					th / PIXEL_TO_WORLD * 0.5)
			if inside >= 0.95:
				break
			pixel_size *= 0.85
			if pixel_size < NATION_LABEL_PIXEL_MIN:
				break
	var fits := pixel_size >= NATION_LABEL_PIXEL_MIN and inside >= 0.95
	var text_world := text_px * pixel_size
	var height_world := font_h * pixel_size
	return {
		"text": text,
		"fits": fits,
		"inside": inside,
		"pixel_size": pixel_size,
		"text_width_world": text_world,
		"text_height_world": height_world,
		"short": false,
	}


## Fração de pontos do retângulo do rótulo (5×5) que caem no território da
## própria nação, no raster de dono. >5% fora reprova o encaixe.
func _nation_inside_ratio(tag: String, center_px: Vector2, angle: float,
		half_w_px: float, half_h_px: float) -> float:
	if _territory == null:
		return 1.0
	var axis := Vector2(cos(angle), sin(angle))
	var perp := Vector2(-axis.y, axis.x)
	var samples := 5
	var inside := 0
	for iy in samples:
		var v := (float(iy) + 0.5) / float(samples) - 0.5
		for ix in samples:
			var u := (float(ix) + 0.5) / float(samples) - 0.5
			var point := center_px + axis * (u * 2.0 * half_w_px) \
					+ perp * (v * 2.0 * half_h_px)
			var uv := Vector2(point.x / MAP_SIZE_PX.x, point.y / MAP_SIZE_PX.y)
			uv.x = fposmod(uv.x, 1.0)
			if uv.y < 0.0 or uv.y > 1.0:
				continue
			if _territory.canonical_owner_at_uv(uv) == tag:
				inside += 1
	return float(inside) / float(samples * samples)


## Posiciona/redimensiona um rótulo de nação no polo de inacessibilidade do
## maior pedaço contíguo, alinhado ao eixo principal (±35°). Sem território,
## esconde (meta "has_territory" = false).
func _place_nation_label(label: Label3D) -> void:
	var tag := String(label.get_meta("tag"))
	var model: Dictionary = {}
	if _territory != null:
		model = _territory.nation_label_model(tag)
	if model.is_empty() and _nation_anchors.has(tag):
		var anchor: Dictionary = _nation_anchors[tag]
		var extent := float(anchor.get("extent_px", 40.0))
		model = {
			"x": float(anchor.get("x", 0.0)),
			"y": float(anchor.get("y", 0.0)),
			"width_px": extent,
			"height_px": extent * 0.6,
			"angle": 0.0,
			"area_px": extent * extent,
			"extent_px": extent,
		}
	if model.is_empty():
		label.set_meta("has_territory", false)
		label.visible = false
		return

	label.set_meta("has_territory", true)
	label.set_meta("model", model)
	var center := Vector2(float(model.get("x", 0.0)), float(model.get("y", 0.0)))
	label.position = pixel_to_world(center) + Vector3(0.0, 0.2, 0.0)
	var angle := float(model.get("angle", 0.0))
	label.rotation_degrees = Vector3(-90.0, -rad_to_deg(angle), 0.0)
	_configure_nation_label(label, model)


## Monta fonte (letter-spacing proporcional ao país), tenta nome completo,
## depois o curto (i18n nation.short.* ou ISO3) e guarda o resultado no meta do
## Label3D para o layout de visibilidade.
func _configure_nation_label(label: Label3D, model: Dictionary) -> void:
	var tag := String(label.get_meta("tag"))
	var width_px := float(model.get("width_px", float(model.get("extent_px", 40.0))))
	var font := FontVariation.new()
	font.base_font = NATION_LABEL_FONT
	font.variation_opentype = {"wght": 600}
	# Países grandes ganham mais espaço entre letras (carta à Paradox).
	font.spacing_glyph = clampi(roundi(width_px / 45.0), 2, 14)
	label.font = font

	var full := I18n.nation_name(tag).to_upper()
	var fit := _fit_nation_text(label, model, full)
	if not bool(fit["fits"]):
		var short := I18n.nation_short(tag).to_upper()
		if short != full:
			var short_fit := _fit_nation_text(label, model, short)
			if bool(short_fit["fits"]):
				short_fit["short"] = true
				fit = short_fit
	label.text = String(fit["text"])
	label.pixel_size = float(fit["pixel_size"])
	label.set_meta("fits", bool(fit["fits"]))
	label.set_meta("inside", float(fit["inside"]))
	label.set_meta("short", bool(fit.get("short", false)))
	label.set_meta("text_width", float(fit["text_width_world"]))
	label.set_meta("text_height", float(fit["text_height_world"]))
	label.set_meta("area_px", float(model.get("area_px", 0.0)))
	label.set_meta("angle", float(model.get("angle", 0.0)))
	label.set_meta("extent", width_px * PIXEL_TO_WORLD)


## Reaplica âncora/extensão em todas as cópias laterais (wrap cilíndrico).
func _reposition_nation_labels() -> void:
	for root in _content_roots():
		var holder := root.get_node_or_null("NationLabels") as Node3D
		if holder == null:
			continue
		for child in holder.get_children():
			_place_nation_label(child as Label3D)
	_nation_labels_dirty = true


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
		territory_changed.emit()


## Textura do minimapa (cor do dono por província), gerada e cacheada sob
## demanda. Recalculada quando os donos mudam (territory_changed invalida).
func minimap_texture() -> ImageTexture:
	if _territory == null:
		return null
	if _minimap_texture == null:
		_minimap_texture = _territory.minimap_texture()
	return _minimap_texture


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


## Nomes de nação entram por TAMANHO DO PAÍS NA TELA (px) e pelo ENCAIXE no
## território (≥95% do retângulo dentro do dono). Capitais/bandeiras e nomes de
## cidade já aceitos pelo layout têm prioridade (declutter); o maior país vence
## entre dois nomes que colidem. Histerese + fade de 150 ms evitam o pisca.
func _update_nation_labels(camera: Camera3D) -> void:
	_nation_label_report_cache.clear()
	if camera == null or _mode != "political":
		_hide_nation_labels()
		return
	var viewport := camera.get_viewport()
	if viewport == null:
		return
	var screen := Rect2(Vector2.ZERO,
			viewport.get_visible_rect().size).grow(NATION_LABEL_SCREEN_PAD)
	var chosen_root := _wrap_offset(0.0)
	for root in _content_roots():
		var holder := root.get_node_or_null("NationLabels") as Node3D
		var capitals := root.get_node_or_null("NationCapitalLabels") as Node3D
		var active_root := absf(root.position.x - chosen_root) <= 1.0
		if not active_root:
			if holder != null and holder.visible:
				holder.visible = false
			if capitals != null and capitals.visible:
				capitals.visible = false
			continue
		if holder != null and not holder.visible:
			holder.visible = true
		if capitals != null and not capitals.visible:
			capitals.visible = true

		var floor_by_tag: Dictionary = {}
		var candidates: Array[Dictionary] = []
		if holder != null:
			for child in holder.get_children():
				var label := child as Label3D
				if label == null:
					continue
				var tag := String(label.get_meta("tag"))
				floor_by_tag[tag] = label
				var rect := _nation_label_screen_rect(camera, label)
				label.set_meta("screen_rect", rect)
				if not bool(label.get_meta("fits", false)) \
						or bool(label.get_meta("has_territory", false)) == false:
					_set_nation_label_visible(label, false)
					continue
				if rect.size.x <= 0.0 or not screen.intersects(rect):
					_set_nation_label_visible(label, false)
					continue
				var max_text := maxf(screen.size.x, screen.size.y)
				if rect.size.x > max_text or rect.size.y > max_text:
					_set_nation_label_visible(label, false)
					continue
				var screen_w := _nation_label_screen_width(camera, label)
				var was_on := bool(label.get_meta("floor_on", false))
				var min_px := NATION_LABEL_MIN_SCREEN_PX \
						* (NATION_LABEL_HYST_OUT if was_on else NATION_LABEL_HYST_IN)
				var alpha := clampf((screen_w - min_px) \
						/ NATION_LABEL_FADE_SCREEN_PX, 0.0, 1.0)
				var far := _zoom_level == ZoomLevel.FAR
				var far_min := NATION_LABEL_FAR_FLOOR_MIN_PX \
						* (NATION_LABEL_HYST_OUT if was_on else NATION_LABEL_HYST_IN)
				if alpha <= 0.01 or (far and screen_w < far_min):
					label.set_meta("floor_on", false)
					_set_nation_label_visible(label, false)
					continue
				candidates.append({
					"label": label,
					"tag": tag,
					"rect": rect,
					"alpha": alpha,
					"screen_w": screen_w,
					"weight": float(label.get_meta("area_px", 0.0)),
				})

		# Ocupação de tela: o layout de cidade/porto do frame anterior + os
		# bloqueios de capital/bandeira deste frame (o layout de cidade só roda
		# depois; no LONGE ele nem roda e a grade anterior pode estar velha).
		var grid: Dictionary = {}
		var max_blocker := maxf(screen.size.x, screen.size.y)
		for record in _city_nodes:
			if not bool(record.get("capital", false)):
				continue
			if float(record.get("label_alpha", 0.0)) <= 0.01:
				continue
			var wrap := float(record.get("wrap_x", 0.0))
			var anchor_local: Vector3 = record.get("anchor", Vector3.ZERO)
			var anchor_world := to_global(anchor_local + Vector3(wrap, 0.0, 0.0))
			if camera.is_position_behind(anchor_world):
				continue
			var em := float(record.get("font_size", LABEL_SIZE_STANDARD)) \
					* float(record.get("pixel_size", LABEL_PIXEL_MIN))
			var icon_h := em * PLACE_ICON_HEIGHT
			var icon_rect := _world_rect_to_screen(camera, anchor_world,
					icon_h * 0.5, icon_h * 0.5)
			if icon_rect.size.x > 0.0 and icon_rect.size.x <= max_blocker \
					and icon_rect.size.y <= max_blocker:
				_grid_insert(grid, icon_rect)
			var flag_rect := _flag_rect(record, camera)
			if flag_rect.size.x > 0.0 and flag_rect.size.x <= max_blocker \
					and flag_rect.size.y <= max_blocker:
				_grid_insert(grid, flag_rect)

		candidates.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
			return float(a["weight"]) > float(b["weight"]))
		var accepted: Dictionary = {}
		var deferred: Array[Dictionary] = []
		for candidate in candidates:
			var rect: Rect2 = candidate["rect"]
			# Block de capital/bandeira (estratégico) e outro nome de país
			# bloqueiam; o TEXTO de uma cidade não - o nome do país é tinta de
			# fundo (render_priority 2, alfa 0.68) e, como no mapa antigo, o nome
			# da cidade fica por cima. Sem essa folga a Europa ficava sem nome
			# nenhum no PERTO/MÉDIO, porque a grade de cidades cobre o mapa.
			if _label_text_hits(rect, NATION_LABEL_CITY_PAD) \
					or _grid_hits(grid, rect, NATION_LABEL_CITY_PAD):
				deferred.append(candidate)
				continue
			_accept_nation_candidate(candidate, grid, accepted)
		# Segunda passada: onde não há folga, o nome entra assim mesmo (atrás do
		# nome de cidade), mas nunca sobre capital/bandeira ou outro país.
		for candidate in deferred:
			if _grid_hits(grid, candidate["rect"], NATION_LABEL_CITY_PAD):
				continue
			_accept_nation_candidate(candidate, grid, accepted)

		# LONGE: nomes de país pequenos junto da estrela da capital.
		if capitals != null:
			for child in capitals.get_children():
				var cap_label := child as Label3D
				if cap_label == null:
					continue
				var tag := String(cap_label.get_meta("tag"))
				if _zoom_level != ZoomLevel.FAR or accepted.has(tag):
					_set_nation_label_visible(cap_label, false)
					cap_label.set_meta("screen_rect", Rect2())
					continue
				var floor_label: Label3D = floor_by_tag.get(tag)
				var floor_fits := floor_label != null \
						and bool(floor_label.get_meta("fits", false))
				var floor_w := _nation_label_screen_width(camera, floor_label) \
						if floor_label != null else 0.0
				if floor_fits and floor_w >= NATION_LABEL_FAR_FLOOR_MIN_PX \
						* NATION_LABEL_HYST_OUT:
					# País grande: o nome no chão é o correto; se ele colidiu,
					# não troque por um rótulo de capital.
					_set_nation_label_visible(cap_label, false)
					cap_label.set_meta("screen_rect", Rect2())
					continue
				var record: Dictionary = _capital_city_by_nation.get(tag, {})
				if record.is_empty():
					_set_nation_label_visible(cap_label, false)
					continue
				_place_nation_capital_label(cap_label, camera, record)
				var cap_rect := _nation_capital_screen_rect(camera, cap_label)
				cap_label.set_meta("screen_rect", cap_rect)
				if cap_rect.size.x <= 0.0 or not screen.intersects(cap_rect) \
						or _grid_hits(_label_grid, cap_rect, NATION_LABEL_CITY_PAD) \
						or _grid_hits(grid, cap_rect, NATION_LABEL_CITY_PAD):
					_set_nation_label_visible(cap_label, false)
					continue
				_grid_insert(grid, cap_rect)
				accepted[tag] = true
				_set_nation_label_visible(cap_label, true, 1.0)

		# Relatório do último layout (testes/captura).
		if holder != null:
			for child in holder.get_children():
				var label := child as Label3D
				if label == null:
					continue
				_nation_label_report_cache.append({
					"kind": "nation",
					"tag": String(label.get_meta("tag")),
					"visible": float(label.get_meta("alpha_target", 0.0)) > 0.01,
					"rect": label.get_meta("screen_rect", Rect2()),
					"inside": float(label.get_meta("inside", 1.0)),
					"fits": bool(label.get_meta("fits", false)),
					"short": bool(label.get_meta("short", false)),
					"area_px": float(label.get_meta("area_px", 0.0)),
				})
		if capitals != null:
			for child in capitals.get_children():
				var label := child as Label3D
				if label == null:
					continue
				_nation_label_report_cache.append({
					"kind": "capital",
					"tag": String(label.get_meta("tag")),
					"visible": float(label.get_meta("alpha_target", 0.0)) > 0.01,
					"rect": label.get_meta("screen_rect", Rect2()),
					"inside": 1.0,
					"fits": true,
					"short": false,
					"area_px": 0.0,
				})
		return


## Aceita o candidato: ocupa a grade (para o nome de outro país não repetir a
## área) e liga o alvo de alfa.
func _accept_nation_candidate(candidate: Dictionary, grid: Dictionary,
		accepted: Dictionary) -> void:
	var label: Label3D = candidate["label"]
	_grid_insert(grid, candidate["rect"])
	accepted[String(candidate["tag"])] = true
	label.set_meta("floor_on", true)
	_set_nation_label_visible(label, true, float(candidate["alpha"]))


## Colisão com o TEXTO de uma cidade (não com o bloco ícone+bandeira). Usado
## para dar ao nome de país uma posição limpa quando existe folga; a colisão
## com o ícone/bandeira continua vindo da grade de ocupação.
func _label_text_hits(rect: Rect2, pad: float) -> bool:
	for entry: Dictionary in _label_rects:
		if String(entry.get("kind", "")) != "city":
			continue
		if not bool(entry.get("text_visible", false)):
			continue
		var text_rect: Rect2 = entry.get("text_rect", Rect2())
		if text_rect.size.x <= 0.0 or text_rect.size.y <= 0.0:
			continue
		if text_rect.grow(pad).intersects(rect):
			return true
	return false


## Esmaece/hide todos os nomes de nação (modo não político ou sem câmera).
func _hide_nation_labels() -> void:
	for root in _content_roots():
		var holder := root.get_node_or_null("NationLabels")
		if holder != null:
			for child in holder.get_children():
				_set_nation_label_visible(child as Label3D, false)
		var capitals := root.get_node_or_null("NationCapitalLabels")
		if capitals != null:
			for child in capitals.get_children():
				_set_nation_label_visible(child as Label3D, false)


## Retângulo em tela do texto deitado no chão (4 cantos no espaço do mundo).
func _nation_label_screen_rect(camera: Camera3D, label: Label3D) -> Rect2:
	var text_w := float(label.get_meta("text_width", 0.0))
	var text_h := float(label.get_meta("text_height", 0.0))
	if text_w <= 0.0 or text_h <= 0.0:
		return Rect2()
	var angle := float(label.get_meta("angle", 0.0))
	var axis := Vector3(cos(angle), 0.0, sin(angle))
	var perp := Vector3(-axis.z, 0.0, axis.x)
	var center := label.global_position
	var rect := Rect2()
	var first := true
	for sx: float in [-1.0, 1.0]:
		for sz: float in [-1.0, 1.0]:
			var point := center + axis * (sx * text_w * 0.5) \
					+ perp * (sz * text_h * 0.5)
			if camera.is_position_behind(point):
				return Rect2()
			var projected := camera.unproject_position(point)
			if first:
				rect = Rect2(projected, Vector2.ZERO)
				first = false
			else:
				rect = rect.expand(projected)
	return rect


## Largura em tela do texto deitado (ao longo do eixo principal).
func _nation_label_screen_width(camera: Camera3D, label: Label3D) -> float:
	if label == null:
		return 0.0
	var text_w := float(label.get_meta("text_width", 0.0))
	if text_w <= 0.0:
		return 0.0
	var angle := float(label.get_meta("angle", 0.0))
	var axis := Vector3(cos(angle), 0.0, sin(angle))
	var center := label.global_position
	var a := center - axis * (text_w * 0.5)
	var b := center + axis * (text_w * 0.5)
	if camera.is_position_behind(a) or camera.is_position_behind(b):
		return 0.0
	return camera.unproject_position(a).distance_to(camera.unproject_position(b))


## Posiciona o nome do país sob a estrela da capital, com tamanho de tela
## constante (o `pixel_size` acompanha a distância da câmera).
func _place_nation_capital_label(label: Label3D, camera: Camera3D,
		record: Dictionary) -> void:
	var anchor_local: Vector3 = record.get("anchor", Vector3.ZERO)
	var wrap := float(record.get("wrap_x", _wrap_offset(anchor_local.x)))
	var star := to_global(anchor_local + Vector3(wrap, 0.0, 0.0))
	var distance := camera.global_position.distance_to(star)
	var pixel_size := clampf(LABEL_PIXEL_PER_UNIT * distance,
			LABEL_PIXEL_MIN, LABEL_PIXEL_MAX)
	var font_size := float(record.get("font_size", LABEL_SIZE_CAPITAL))
	var down := -camera.global_transform.basis.y
	label.global_position = star + down * pixel_size * (font_size * 0.72 + 3.0) \
			+ Vector3(0.0, 0.01, 0.0)
	label.pixel_size = maxf(pixel_size * NATION_CAPITAL_LABEL_SCALE, LABEL_PIXEL_MIN)


## Retângulo em tela do rótulo de capital (billboard: largura/altura da fonte
## projetadas na profundidade da estrela).
func _nation_capital_screen_rect(camera: Camera3D, label: Label3D) -> Rect2:
	if camera.is_position_behind(label.global_position):
		return Rect2()
	var px_per_world := _screen_px_per_world(camera, label.global_position,
			camera.global_transform.basis.y)
	if px_per_world <= 0.0:
		return Rect2()
	var text_px := label.font.get_string_size(label.text,
			HORIZONTAL_ALIGNMENT_LEFT, -1, label.font_size).x
	var size := Vector2(text_px * label.pixel_size * px_per_world,
			float(label.font_size) * label.pixel_size * px_per_world)
	var center := camera.unproject_position(label.global_position)
	return Rect2(center - size * 0.5, size)


## Anima o alfa dos rótulos de nação (150 ms) sem piscar ao mover a câmera.
func _advance_nation_label_fades(delta: float) -> void:
	if _nation_fade_active.is_empty():
		return
	var step := delta / maxf(NATION_LABEL_FADE_TIME, 0.001)
	for index in range(_nation_fade_active.size() - 1, -1, -1):
		var label := _nation_fade_active[index]
		if not is_instance_valid(label):
			_nation_fade_active.remove_at(index)
			continue
		var target := float(label.get_meta("alpha_target", 0.0))
		var alpha := move_toward(float(label.get_meta("alpha", 0.0)), target, step)
		label.set_meta("alpha", alpha)
		label.modulate = Color(NATION_LABEL_COLOR.r, NATION_LABEL_COLOR.g,
				NATION_LABEL_COLOR.b, NATION_LABEL_COLOR.a * alpha)
		if alpha > 0.001:
			if not label.visible:
				label.visible = true
		elif label.visible:
			label.visible = false
		if is_equal_approx(alpha, target):
			_nation_fade_active.remove_at(index)


## Relatório do último layout de nomes de nação (teste de encaixe/declutter).
func nation_label_report() -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	for entry in _nation_label_report_cache:
		out.append(entry.duplicate(true))
	return out


## Define o alvo de visibilidade/alfa do rótulo; a transição é animada no fade.
## `alpha_scale` permite o fade de entrada por tamanho de tela.
func _set_nation_label_visible(label: Label3D, value: bool,
		alpha_scale: float = 1.0) -> void:
	if label == null:
		return
	label.set_meta("alpha_target", alpha_scale if value else 0.0)
	if value and not label.visible:
		label.visible = true
	if not _nation_fade_active.has(label):
		_nation_fade_active.append(label)


func _build_mode_layer(cities: Array) -> void:
	_mode_layer = ModeLayerScript.new()
	_mode_layer.name = "ModeLayer"
	add_child(_mode_layer)
	# Passa a geografia já carregada; o ModeLayer só cria a própria instância
	# quando chamado fora do CityWorld (sem Economy).
	_mode_layer.setup(cities, _geo)


func _build_cities(cities: Array) -> void:
	# As maiores começam com o modelo detalhado (estrela/prédios, discos e
	# bandeira próprios); TODAS as cidades recebem o tecido urbano ao aproximar,
	# construído sob demanda pela fila com orçamento. O disco raso que virava
	# mancha clara foi removido de vez — não volta com o tecido.
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
		var capital := bool(_capital_by_id.get(city_id, false))
		var icon: Sprite3D = null
		var ring: Sprite3D = null
		if detailed_ids.has(city_id):
			var icon_texture := STAR_ICON if capital else CITY_ICON
			# Bandeira agora é MultiMesh (atlas único): o marcador detalhado só
			# tem ícone + selo. O glifo aparece quando o nível pede a cidade; no
			# LONGE é só a estrela da capital, sem o selo atrás.
			var marker := _add_place_icon(10, icon_texture)
			icon = marker["icon"]
			ring = marker["ring"]
		var record := {
			"id": city_id,
			"kind": "city",
			"city": city,
			"position": position,
			# Âncora = ponto geográfico da cidade (sem elevação): ícone, rótulo,
			# anel e tecido urbano projetam no MESMO x,y do cities.json. O antigo
			# +1 u parecia inofensivo no zoom out, mas a d013 deslocava o ícone
			# ~1° ao norte (Al Mansurah aparecia na costa).
			"anchor": position,
			"ground": position,
			"priority": float(city.get("population", 0.0)),
			"capital": capital,
			"nation": String(city.get("nation", "")),
			"detailed": detailed_ids.has(city_id),
			"label": null,
			"label_alpha": 1.0,
			"label_target": 1.0,
			"pixel_size": LABEL_PIXEL_PER_UNIT * 52.0,
			"font_size": LABEL_SIZE_CAPITAL if capital else LABEL_SIZE_STANDARD,
			"text_width": 0.0,
			"wrap_x": 0.0,
			"icon": icon,
			"ring": ring,
			"flag": null,
			"urban": null,
			"urban_queued": false,
		}
		_city_nodes.append(record)
		_records_by_id[city_id] = record
		# Marcador detalhado já nasce no lugar: o laço de LOD só varre perto da
		# câmera, então o que fica longe do scan ficaria em (0,0,0) até o mapa
		# se aproximar (marcador visível no golfo da Guiné).
		if icon != null or ring != null:
			_place_city_marker(record)
		# Raio em que o LOD ainda atualiza este registro. Não é mais a faixa de
		# fade do nome (o nível de zoom decide o texto); serve ao tecido urbano
		# e ao assentamento do registro longe da câmera.
		record["lod_end"] = CITY_VISIBLE_DISTANCE
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
		# Porto com o MESMO id da cidade (porto fundido no tecido urbano) não
		# pode sobrescrever o registro da cidade: o id é a chave de pick, do
		# anel e do footprint. Antes o anel de Washington/Taipei/Dongguan era
		# ancorado no porto, ~17 px longe do ícone da cidade.
		var existing: Dictionary = _records_by_id.get(port_id, {})
		if existing.is_empty() or String(existing.get("kind", "")) != "city":
			_records_by_id[port_id] = record
		# Raio de atualização do LOD (modelo 3D do porto). O selo só aparece no
		# nível PERTO; o nível de zoom decide a visibilidade, não o fade.
		record["label_alpha"] = 1.0
		record["label_target"] = 1.0
		record["lod_end"] = PORT_MODEL_VISIBLE_DISTANCE
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
## latão. A bandeira da capital é desenhada pelo MultiMesh do atlas de bandeiras.
func _add_place_icon(render_priority: int, texture: Texture2D) -> Dictionary:
	var icon := Sprite3D.new()
	icon.texture = texture
	icon.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	icon.transparent = true
	icon.modulate = ICON_COLOR
	icon.no_depth_test = true
	icon.render_priority = render_priority - 1
	# Nasce invisível: até `_place_city_marker`/layout posicionar o marcador ele
	# ficaria na origem do mundo (lat 0/lon 0, golfo da Guiné) — a "cidade no
	# meio do mar" que aparecia para quem olhava a África.
	icon.visible = false
	_places.add_child(icon)

	var ring := Sprite3D.new()
	ring.texture = BADGE_TEXTURE
	ring.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	ring.transparent = true
	ring.no_depth_test = true
	ring.render_priority = render_priority - 2
	ring.visible = false
	_places.add_child(ring)

	return {"icon": icon, "ring": ring}


## Atlas único das bandeiras das capitais + MultiMesh (uma draw call). Cada
## instância leva a célula da nação em `INSTANCE_CUSTOM` e o alfa em `COLOR`.
## Sem Economy/flags (bench sintético), não cria nada e o mapa segue sem bandeira.
func _build_flag_renderer() -> void:
	_flag_records.clear()
	_flag_uv.clear()
	if _flag_mm != null:
		_flag_mm.queue_free()
		_flag_mm = null
	var nations: Array[String] = []
	var seen := {}
	for record in _city_nodes:
		if not bool(record.get("capital", false)):
			continue
		var nation := String(record.get("nation", ""))
		if nation.is_empty() or seen.has(nation):
			continue
		seen[nation] = true
		nations.append(nation)
	if nations.is_empty():
		return

	var cell := FLAG_CELL
	var images: Array[Image] = []
	var usable: Array[String] = []
	for nation in nations:
		var texture: Texture2D = Economy.flag_texture(nation)
		if texture == null:
			continue
		var image := texture.get_image()
		if image == null:
			continue
		if image.get_format() != Image.FORMAT_RGBA8:
			image.convert(Image.FORMAT_RGBA8)
		if image.get_width() != cell.x or image.get_height() != cell.y:
			image.resize(cell.x, cell.y, Image.INTERPOLATE_BILINEAR)
		images.append(image)
		usable.append(nation)
	if usable.is_empty():
		return

	var cols := mini(FLAG_ATLAS_COLS, usable.size())
	var rows := ceili(float(usable.size()) / float(cols))
	var atlas := Image.create(cols * cell.x, rows * cell.y, false, Image.FORMAT_RGBA8)
	atlas.fill(Color(0.0, 0.0, 0.0, 0.0))
	for index in usable.size():
		var column := index % cols
		var row := floori(float(index) / float(cols))
		atlas.blit_rect(images[index], Rect2i(0, 0, cell.x, cell.y),
				Vector2i(column * cell.x, row * cell.y))
		# Meio texel de folga: a interpolação linear não puxa o vizinho.
		var u0 := (float(column * cell.x) + 0.5) / float(cols * cell.x)
		var v0 := (float(row * cell.y) + 0.5) / float(rows * cell.y)
		var us := float(cell.x - 1) / float(cols * cell.x)
		var vs := float(cell.y - 1) / float(rows * cell.y)
		_flag_uv[usable[index]] = Rect2(u0, v0, us, vs)
	_flag_atlas = ImageTexture.create_from_image(atlas)

	for record in _city_nodes:
		if not bool(record.get("capital", false)):
			continue
		var nation := String(record.get("nation", ""))
		if not _flag_uv.has(nation):
			continue
		record["flag"] = {
			"nation": nation,
			"texture": _flag_atlas,
			"uv": _flag_uv[nation],
		}
		_flag_records.append(record)
	_flag_mm = _make_flag_multimesh(_flag_records.size())


func _make_flag_multimesh(count: int) -> MultiMeshInstance3D:
	var quad := QuadMesh.new()
	quad.size = Vector2.ONE
	var material := ShaderMaterial.new()
	material.shader = FLAG_SHADER
	material.set_shader_parameter("flags_atlas", _flag_atlas)
	material.render_priority = 12
	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.use_custom_data = true
	multimesh.use_colors = true
	multimesh.mesh = quad
	multimesh.instance_count = count
	multimesh.visible_instance_count = 0
	var instance := MultiMeshInstance3D.new()
	instance.name = "CityFlags"
	instance.multimesh = multimesh
	instance.material_override = material
	instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_places.add_child(instance)
	return instance


## Centro do mundo da bandeira da capital, na cópia de wrap mais próxima.
func _flag_world_center(record: Dictionary) -> Vector3:
	var anchor: Vector3 = record["anchor"] \
			+ Vector3(float(record.get("wrap_x", 0.0)), 0.0, 0.0)
	var em := float(record["font_size"]) * float(record["pixel_size"])
	var icon_h := em * PLACE_ICON_HEIGHT
	var ring_h := icon_h * RING_SCALE
	var gap := em * PLACE_ICON_GAP
	var flag_h := icon_h * FLAG_SCALE
	return to_global(anchor + Vector3(-ring_h * 0.5 - gap - flag_h, 0.0, 0.0))


## Reescreve o buffer das bandeiras só quando algo mudou. Só capitais com alfa
## aceso e dentro do frustum entram; as demais ficam fora de `visible_instance_count`.
func _apply_flag_transforms(camera: Camera3D) -> void:
	_flags_dirty = false
	if _flag_mm == null or _flag_mm.multimesh == null:
		return
	var buffer := _flag_mm.multimesh
	var index := 0
	for record in _flag_records:
		var alpha := float(record.get("label_alpha", 0.0))
		if alpha <= 0.01:
			continue
		var center := _flag_world_center(record)
		if camera.is_position_behind(center) \
				or not camera.is_position_in_frustum(center):
			continue
		if index >= buffer.instance_count:
			break
		var em := float(record["font_size"]) * float(record["pixel_size"])
		var icon_h := em * PLACE_ICON_HEIGHT
		var flag_h := icon_h * FLAG_SCALE
		var size := Vector3(flag_h * 2.0, flag_h * 1.35, 1.0)
		var uv: Rect2 = record["flag"]["uv"]
		buffer.set_instance_transform(index, Transform3D(
				Basis().scaled(size), _flag_mm.to_local(center)))
		buffer.set_instance_custom_data(index,
				Color(uv.size.x, uv.size.y, uv.position.x, uv.position.y))
		buffer.set_instance_color(index, Color(1.0, 1.0, 1.0, alpha))
		index += 1
	buffer.visible_instance_count = index


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
		var icon_scale := FAR_CAPITAL_ICON_SCALE \
				if _zoom_level == ZoomLevel.FAR \
						and bool(record.get("capital", false)) else 1.0
		icon.pixel_size = icon_h * icon_scale / PLACE_ICON_SOURCE
		icon.position = anchor

	var ring: Sprite3D = record.get("ring")
	if ring != null:
		ring.pixel_size = ring_h / PLACE_ICON_SOURCE
		ring.position = anchor

	var text_w := _city_text_width(record)
	var text_center := anchor + Vector3(ring_h * 0.5 + gap + text_w * 0.5,
			0.0, 0.0)
	record["text_center"] = text_center

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


## Frame do mapa: nível de zoom (3 níveis com histerese/fade), varredura do LOD
## só nas células visíveis, layout de tela com throttle (~12 Hz) e fatiado, e
## buffers de ícones/bandeiras reescritos apenas quando algo mudou.
func _process(delta: float) -> void:
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
	var viewport_size := Vector2.ZERO
	var viewport := camera.get_viewport()
	if viewport != null:
		viewport_size = viewport.get_visible_rect().size
	var viewport_changed := _layout_viewport != Vector2.ZERO \
			and viewport_size != _layout_viewport
	var moved := _camera_changed(camera) or local_delta > CAMERA_MOVE_EPSILON
	var first_level := _zoom_level < 0
	if _update_zoom_level(camera):
		_refresh_level_targets(first_level)
		_layout_dirty = true
		_nation_labels_dirty = true
		_convoy_timer = 0.0
	if moved:
		_nation_labels_dirty = true
	var fading := _advance_level_fade(delta)
	_advance_nation_label_fades(delta)
	_last_camera_local = camera_local

	# Buffers de ícone/bandeira ANTES da varredura: usam a lista visível do
	# ciclo anterior (1 frame de atraso, imperceptível) e deixam de somar o
	# rebuild do MultiMesh ao pico da varredura no mesmo frame. No nível LONGE
	# só reescrevem quando o relógio de 4 Hz vence (`_icon_accum`), porque não há
	# nome/bandeira mudando.
	_icon_accum += delta
	var icons_ready := _zoom_level != ZoomLevel.FAR \
			or _icon_accum >= FAR_ICON_INTERVAL
	if _icons_dirty and icons_ready and _scanned_once:
		_icon_accum = 0.0
		_apply_icon_transforms(camera)
	if _flags_dirty:
		_apply_flag_transforms(camera)

	# Nomes de nação: atualiza só com a câmera parada. Durante o pan/zoom a
	# geometria do texto não muda; reavaliar colisão/visibilidade a 12 Hz custava
	# mais que o resto do frame. Com o layout de cidade em fatias, a grade está
	# pela metade: esperar o commit evita o nome de país sobre um nome de cidade
	# que ainda não foi aceito.
	if not moved and _nation_labels_dirty and not _layout_job_active:
		if _mode == "political":
			_update_nation_labels(camera)
		_nation_labels_dirty = false

	# Câmera parada, nada sujo, nenhum fade/job e nenhuma fila: o frame custa só
	# a comparação da câmera. Modelo na fila continua drenando (é o pico real).
	if not moved and not _layout_dirty and not _layout_pending and not fading \
			and not _layout_job_active and _level_fade_queue.is_empty() \
			and not _icons_dirty and not _flags_dirty \
			and not _layout_after_scan:
		if _urban_queue.is_empty() and _port_model_queue.is_empty():
			_update_convoy_lod(camera, camera_local, delta)
			return
		_process_lod_queues(camera)
		_update_convoy_lod(camera, camera_local, delta)
		return

	# Pulo grande (clique no HUD, limite, teleporte) ou resize da viewport:
	# varredura e layout completos já neste frame.
	var jump := local_delta > LAYOUT_JUMP or viewport_changed
	if moved:
		_layout_accum += delta
		_layout_pending = true
	var layout_due := _layout_dirty or (moved and (jump \
			or _layout_accum >= LAYOUT_MIN_INTERVAL))
	if not moved and _layout_pending:
		layout_due = true
	if _layout_job_active:
		# Fatiado: continua de onde parou. Se a câmera mudou no meio, agenda
		# um layout novo para depois do commit (não reinicia no meio).
		if layout_due:
			_layout_pending = true
			_layout_dirty = false
		_layout_place_labels(camera, LAYOUT_BUDGET_USEC)
	elif _layout_after_scan:
		# A varredura do ciclo anterior já gastou o frame: agora só o layout.
		_layout_after_scan = false
		_layout_accum = 0.0
		_layout_pending = false
		_layout_dirty = false
		_layout_place_labels(camera, LAYOUT_BUDGET_USEC)
	elif layout_due:
		_layout_accum = 0.0
		_layout_pending = false
		_layout_dirty = false
		var scan_start := Time.get_ticks_usec()
		_scan_lod(camera, camera_local, jump)
		_apply_wrap_positions()
		var scan_usec := Time.get_ticks_usec() - scan_start
		if _zoom_level == ZoomLevel.FAR and not fading \
				and _level_fade_queue.is_empty():
			# Estratégico assentado: não há nome/bandeira/porto para colidir.
			# Limpar é O(labels ativos), não O(cidades).
			_clear_labels_only()
			# A varredura é quem muda o conjunto visível: o rebuild das bolas
			# acompanha a cadência de 4 Hz em vez de rodar a cada varredura.
			if _icon_accum >= FAR_ICON_INTERVAL:
				_icons_dirty = true
		elif jump:
			# Teleporte/pulo grande: layout completo neste frame (a tela inteira
			# mudou). Pan/zoom normal continua fatiado pelo orçamento.
			_layout_place_labels(camera)
		elif moved and scan_usec >= SCAN_DEFER_USEC:
			# Varredura cara (pan no nível MÉDIO/LONGE): o layout entra no
			# próximo frame, em vez de somar os dois picos no mesmo frame.
			_layout_after_scan = true
		else:
			_layout_place_labels(camera, LAYOUT_BUDGET_USEC)

	# Anéis de hover/seleção seguem a câmera mesmo entre layouts (1–2 registros).
	_update_place_indicators()
	# Modelos detalhados entram fatiados, com orçamento por frame. Tecido urbano
	# espera a câmera parar (construir durante pan contínuo era o pico do frame).
	_process_lod_queues(camera, not moved)
	_update_convoy_lod(camera, camera_local, delta)


## Offset da cópia do mundo (wrap cilíndrico) mais próxima da câmera.
func _wrap_offset(x: float) -> float:
	return roundf((_camera_x - x) / PLANE_SIZE.x) * PLANE_SIZE.x


## Força um layout no próximo frame (hover, seleção, idioma, filtro, modo).
## `nation_labels = false` para mudanças puramente de estilo (hover): os nomes
## de nação não devem ser reavaliados por causa do mouse.
func _invalidate_layout(nation_labels: bool = true) -> void:
	_layout_dirty = true
	if nation_labels:
		_nation_labels_dirty = true


## Nível de zoom atual (enum `ZoomLevel`). Exposto para testes/captura.
func zoom_level() -> int:
	return _zoom_level


## Distância (u) da câmera ao ponto do chão que ela mira.
func zoom_distance() -> float:
	return _zoom_distance


## Define a nação do jogador: no nível MÉDIO, as cidades dela mantêm o ícone
## normal e o nome.
func set_player_nation(tag: String) -> void:
	if tag == _player_nation:
		return
	_player_nation = tag
	if _zoom_level >= 0:
		_refresh_level_targets(false)
	_layout_dirty = true


## Distância da câmera ao plano do mapa ao longo da sua direção de visão.
func _camera_ground_distance(camera: Camera3D) -> float:
	var origin := camera.global_position
	var forward := -camera.global_transform.basis.z
	var plane_y := to_global(Vector3.ZERO).y
	if forward.y < -0.0001:
		var t := (plane_y - origin.y) / forward.y
		if t > 0.0:
			return origin.distance_to(origin + forward * t)
	return maxf(origin.y - plane_y, 1.0)


## Atualiza o nível com histerese. Devolve `true` se o nível mudou.
func _update_zoom_level(camera: Camera3D) -> bool:
	var distance := _camera_ground_distance(camera)
	_zoom_distance = distance
	var level := _zoom_level
	if _zoom_level < 0:
		if distance <= ZOOM_NEAR_EXIT:
			level = ZoomLevel.NEAR
		elif distance <= ZOOM_MID_EXIT:
			level = ZoomLevel.MID
		else:
			level = ZoomLevel.FAR
	elif _zoom_level == ZoomLevel.NEAR:
		if distance > ZOOM_NEAR_EXIT:
			level = ZoomLevel.MID
	elif _zoom_level == ZoomLevel.MID:
		if distance > ZOOM_MID_EXIT:
			level = ZoomLevel.FAR
		elif distance < ZOOM_NEAR_ENTER:
			level = ZoomLevel.NEAR
	elif distance < ZOOM_MID_ENTER:
		level = ZoomLevel.MID
	if level == _zoom_level:
		return false
	_zoom_level = level
	return true


## Recalcula o alvo de alfa de cada cidade/porto no nível atual. `snap` assenta
## sem fade (primeiro frame); caso contrário a transição de 150 ms começa aqui.
func _refresh_level_targets(snap: bool) -> void:
	_level_fade_queue.clear()
	for record in _city_nodes:
		var target := 1.0 if _city_label_wanted(record) else 0.0
		record["label_target"] = target
		# Cache do passo/distância do ícone e do marcador: o nível novo muda
		# escala/glifo, então reaplica mesmo com a câmera parada.
		record["lod_step"] = -1
		record["placed_step"] = -1
		if snap:
			record["label_alpha"] = target
		elif absf(float(record.get("label_alpha", 0.0)) - target) > 0.0001:
			_level_fade_queue.append(record)
	for record in _port_nodes:
		var target := 1.0 if _zoom_level == ZoomLevel.NEAR else 0.0
		record["label_target"] = target
		if snap:
			record["label_alpha"] = target
		elif absf(float(record.get("label_alpha", 0.0)) - target) > 0.0001:
			_level_fade_queue.append(record)
	_icons_dirty = true
	_flags_dirty = true


## O nível atual mostra o nome desta cidade? PERTO: todas. MÉDIO: capitais e as
## cidades da nação do jogador. LONGE: nenhuma (só a estrela das capitais).
func _city_label_wanted(record: Dictionary) -> bool:
	if _zoom_level == ZoomLevel.FAR:
		return false
	if _zoom_level == ZoomLevel.NEAR:
		return true
	return _city_marker_wanted(record)


## O nível atual desenha o marcador desta cidade? PERTO: todas. MÉDIO: capitais
## e cidades da nação do jogador. LONGE: só as capitais (estrela).
func _city_marker_wanted(record: Dictionary) -> bool:
	if _zoom_level < 0 or _zoom_level == ZoomLevel.NEAR:
		return true
	if bool(record.get("capital", false)):
		return true
	return _zoom_level == ZoomLevel.MID and _is_player_city(record)


## Cidade pertence à nação do jogador? Vazio = nenhuma nação declarada ainda.
func _is_player_city(record: Dictionary) -> bool:
	return not _player_nation.is_empty() \
			and String(record.get("nation", "")) == _player_nation


## Avança o fade de 150 ms de todos os registros na fila. Devolve `true` se
## ainda há alpha animando neste frame.
func _advance_level_fade(delta: float) -> bool:
	if _level_fade_queue.is_empty():
		return false
	var step := delta / LEVEL_FADE_SECONDS
	var index := 0
	while index < _level_fade_queue.size():
		var record: Dictionary = _level_fade_queue[index]
		var target := float(record.get("label_target", 0.0))
		var alpha := move_toward(float(record.get("label_alpha", 0.0)),
				target, step)
		record["label_alpha"] = alpha
		_apply_fade_visual(record)
		if absf(alpha - target) <= 0.0001:
			_level_fade_queue.remove_at(index)
		else:
			index += 1
	_icons_dirty = true
	_flags_dirty = true
	if _level_fade_queue.is_empty():
		# Nomes que entraram/saíram do nível precisam de um layout novo.
		_layout_dirty = true
		return false
	return true


## Aplica o alfa do fade ao rótulo/glyph já existentes. O selo atrás do ícone
## acompanha o glifo (no LONGE ele não existe: fica só a estrela da capital).
func _apply_fade_visual(record: Dictionary) -> void:
	if String(record.get("kind", "")) != "city":
		return
	var alpha := float(record.get("label_alpha", 0.0))
	var label: Label3D = record.get("label")
	if label != null:
		label.modulate.a = alpha
	var icon: Sprite3D = record.get("icon")
	if icon != null:
		icon.visible = _city_marker_wanted(record)
		icon.modulate.a = 1.0 if bool(record.get("capital", false)) else alpha


## Varredura do LOD: portos e cidades só nas células que cobrem a viewport
## (+ margem para tecido/portos), com buffers reaproveitados.
func _scan_lod(camera: Camera3D, camera_local: Vector3, jump: bool) -> void:
	_visible_city_records.clear()
	_visible_port_records.clear()
	_scanned_once = true
	var radius := maxf(_viewport_world_radius(camera, camera_local),
			URBAN_BUILD_DISTANCE)
	var port_radius := maxf(radius, PORT_MODEL_VISIBLE_DISTANCE)
	if jump:
		for record in _port_nodes:
			_apply_lod_port(record, camera_local, camera)
		for record in _city_nodes:
			_apply_lod_city(record, camera_local, camera)
		return
	_collect_lod_records(_port_grid, camera_local, port_radius, _scan_port_records)
	for record in _scan_port_records:
		_apply_lod_port(record, camera_local, camera)
	_collect_lod_records(_city_grid, camera_local, radius, _scan_city_records)
	for record in _scan_city_records:
		_apply_lod_city(record, camera_local, camera)


## Render dos comboios: só no nível PERTO e dentro do frustum. O tween (posição
## lógica) continua; o que para é a submissão visual de veículos invisíveis.
func _update_convoy_lod(camera: Camera3D, _camera_local: Vector3,
		delta: float) -> void:
	_convoy_timer -= delta
	if _convoy_timer > 0.0:
		return
	_convoy_timer = 0.18
	if _convoy_renderer == null or not is_instance_valid(_convoy_renderer):
		_convoy_renderer = null
		for child in get_children():
			if child is Node3D and child.get("_convoys") is Array:
				_convoy_renderer = child
				break
		if _convoy_renderer == null:
			return
	var convoys: Variant = _convoy_renderer.get("_convoys")
	if not (convoys is Array):
		return
	var near := _zoom_level == ZoomLevel.NEAR
	for convoy: Variant in convoys:
		if not (convoy is Node3D) or not is_instance_valid(convoy):
			continue
		var node := convoy as Node3D
		var visible := near and not camera.is_position_behind(node.global_position) \
				and camera.is_position_in_frustum(node.global_position)
		if node.visible != visible:
			node.visible = visible


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
## mais próxima. Só cidades detalhadas (nós próprios) e rótulos ativos precisam
## disso; os ícones MultiMesh usam a âncora direto do LOD.
func _apply_wrap_positions() -> void:
	for record in _visible_city_records:
		if record.get("label") != null or bool(record.get("detailed", false)):
			_place_city_marker(record)


## Reancora os anéis de hover/seleção no ícone do lugar. Sem isso, um zoom com
## o cursor parado deixava o aro no raio/escala do frame anterior (enorme e
## deslocado), mesmo com o ícone já noutra projeção.
func _update_place_indicators() -> void:
	if _hover != null and _hover.visible and not _hovered_id.is_empty() \
			and _hovered_id != _selected_id:
		_place_indicator(_hover, null, _hovered_id)
	if _selection != null and _selection.visible and not _selected_id.is_empty():
		_place_indicator(_selection, _selection_dot, _selected_id)


## A posição do registro (já com o wrap) está dentro do frustum da câmera?
## Usado só para não construir modelo 3D de cidade/porto fora da tela.
func _in_frustum(record: Dictionary, camera: Camera3D) -> bool:
	var local: Vector3 = record["position"] \
			+ Vector3(float(record.get("wrap_x", 0.0)), 0.0, 0.0)
	return camera.is_position_in_frustum(to_global(local))


## Visibilidade para ícones/rótulos: frustum OU projeção dentro da tela com a
## mesma margem do layout (o nome encostado na borda ainda conta).
func _in_view(record: Dictionary, camera: Camera3D) -> bool:
	var local: Vector3 = record["position"] \
			+ Vector3(float(record.get("wrap_x", 0.0)), 0.0, 0.0)
	var world := to_global(local)
	if camera.is_position_behind(world):
		return false
	if camera.is_position_in_frustum(world):
		return true
	var viewport := camera.get_viewport()
	if viewport == null:
		return false
	var screen := Rect2(Vector2.ZERO,
			viewport.get_visible_rect().size).grow(LABEL_SCREEN_MARGIN_PX * 2.0)
	return screen.has_point(camera.unproject_position(world))


## Monta a grade do LOD uma vez (as posições dos lugares são estáticas).
func _build_lod_grid() -> void:
	_grid_cells = maxi(1, roundi(PLANE_SIZE.x / LOD_CELL))
	_city_grid.clear()
	_port_grid.clear()
	_city_grid_radius = 0.0
	_port_grid_radius = 0.0
	for record in _city_nodes:
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
## local), preenchendo `out` (buffer reaproveitado; sem alocar Array por frame).
func _collect_lod_records(grid: Dictionary, camera_local: Vector3,
		radius: float, out: Array) -> void:
	out.clear()
	if grid.is_empty():
		return
	var cell := PLANE_SIZE.x / float(maxi(_grid_cells, 1))
	var cells_z := maxi(1, roundi(PLANE_SIZE.y / cell))
	var span := maxi(1, ceili(radius / cell))
	var cx := floori((camera_local.x + PLANE_SIZE.x * 0.5) / cell)
	var cz := floori((camera_local.z + PLANE_SIZE.y * 0.5) / cell)
	for iz in range(cz - span, cz + span + 1):
		if iz < 0 or iz >= cells_z:
			continue
		for ix in range(cx - span, cx + span + 1):
			var bucket: Variant = grid.get(Vector2i(wrapi(ix, 0, _grid_cells), iz))
			if bucket != null:
				out.append_array(bucket)


## Raio (u) do chão coberto pela viewport atual. Interseção de 4 raios de canto
## com o plano do mapa; sem acerto (câmera no horizonte), cai num múltiplo da
## altura. É o que evita varrer meia Europa no nível MÉDIO e o mundo no LONGE.
func _viewport_world_radius(camera: Camera3D, camera_local: Vector3) -> float:
	var viewport := camera.get_viewport()
	if viewport == null:
		return LOD_NEAR_RADIUS
	var size := viewport.get_visible_rect().size
	var plane_y := to_global(Vector3.ZERO).y
	var far := 0.0
	far = maxf(far, _ground_hit_radius(camera, Vector2.ZERO, camera_local, plane_y))
	far = maxf(far, _ground_hit_radius(camera, Vector2(size.x, 0.0), camera_local, plane_y))
	far = maxf(far, _ground_hit_radius(camera, size, camera_local, plane_y))
	far = maxf(far, _ground_hit_radius(camera, Vector2(0.0, size.y), camera_local, plane_y))
	if far <= 0.0:
		return maxf(absf(camera_local.y) * 2.4, LOD_NEAR_RADIUS)
	return minf(far, PLANE_SIZE.x * 0.5)


func _ground_hit_radius(camera: Camera3D, screen_point: Vector2,
		camera_local: Vector3, plane_y: float) -> float:
	var origin := camera.project_ray_origin(screen_point)
	var direction := camera.project_ray_normal(screen_point)
	if absf(direction.y) < 0.0001:
		return 0.0
	var t := (plane_y - origin.y) / direction.y
	if t <= 0.0:
		return 0.0
	var local := to_local(origin + direction * t)
	return Vector2(local.x - camera_local.x, local.z - camera_local.z).length()




## Layout dos rótulos na tela, SEM deslocamento de grupos e SEM linhas-guia:
##  - cidades em ordem de prioridade (hover, capital, nação do jogador, população);
##  - colisão por retângulo + teto de nomes por célula de tela (declutter);
##  - portos são ícones de âncora no ponto de costa, com ajuste local de no
##    máximo `PORT_MAX_OFFSET_PX` para não entrar no rótulo da própria cidade.
##
## `budget_usec < 0` roda tudo de uma vez (testes). O `_process` passa um
## orçamento e o restante continua nos frames seguintes (time-slicing).
func _layout_place_labels(camera: Camera3D, budget_usec: int = -1) -> void:
	if budget_usec < 0:
		_layout_job_reset(camera)
		while _layout_job_active:
			_layout_job_step(camera, -1)
		return
	if not _layout_job_active:
		_layout_job_reset(camera)
	_layout_job_step(camera, budget_usec)


## Nível LONGE assentado: nenhum rótulo, bandeira ou porto na tela. Desanexa os
## labels ativos e limpa o relatório sem varrer as cidades. Não suja os buffers
## de ícone/bandeira: quem decide a cadência deles é o `_process` (4 Hz no LONGE).
func _clear_labels_only() -> void:
	for record in _active_labels:
		record["label"] = null
	_active_labels.clear()
	for label in _label_pool:
		if label.visible:
			label.visible = false
	_label_rects.clear()
	_drawn_ports.clear()
	# No LONGE o layout de cidade não roda: sem limpar a grade, os nomes de
	# nação colidiriam com blocos de cidade que não estão mais na tela.
	_label_grid.clear()
	_label_cell_counts.clear()
	_layout_job_active = false
	# Grade vazia: os nomes de nação voltam a ser avaliados sem bloqueios.
	_nation_labels_dirty = true


func _layout_job_reset(camera: Camera3D) -> void:
	_label_rects.clear()
	for record in _active_labels:
		record["label"] = null
	_active_labels.clear()
	_label_free.clear()
	for label in _label_pool:
		_label_free.append(label)
	_label_grid.clear()
	_label_cell_counts.clear()
	_layout_job_entries.clear()
	_layout_job_city_blocks.clear()
	_drawn_ports.clear()
	_layout_job_index = 0
	_layout_job_phase = 0
	_layout_job_camera = camera
	# Snapshot: o LOD do próximo frame pode limpar as listas visíveis.
	_layout_job_cities = _visible_city_records.duplicate()
	_layout_job_ports = _visible_port_records.duplicate()
	_layout_job_active = true


## Executa uma fatia do layout. `budget_usec < 0` não interrompe.
func _layout_job_step(camera: Camera3D, budget_usec: int) -> void:
	var start := Time.get_ticks_usec()
	while true:
		match _layout_job_phase:
			0:
				while _layout_job_index < _layout_job_cities.size():
					var record: Dictionary = _layout_job_cities[_layout_job_index]
					_layout_job_index += 1
					# Só projeta quem pode mostrar nome (ou está em hover).
					if float(record.get("label_alpha", 0.0)) < LABEL_TEXT_MIN_ALPHA \
							and String(record.get("id", "")) != _hovered_id:
						continue
					# Recalcula centro/escala só quando passo/wrap mudaram (early-out).
					_place_city_marker(record)
					var metrics := _city_metrics(record, camera)
					if metrics.is_empty():
						continue
					var icon_rect := _metrics_rect(metrics, "icon_rect")
					var text_rect := _metrics_rect(metrics, "text_rect")
					if icon_rect.size.x <= 0.0 or text_rect.size.x <= 0.0:
						continue
					_layout_job_entries.append({
						"record": record,
						"icon_rect": icon_rect,
						"text_rect": text_rect,
						"flag_rect": _flag_rect(record, camera),
						"capital": bool(record.get("capital", false)),
						"hovered": String(record["id"]) == _hovered_id,
						"player": not _player_nation.is_empty() \
								and String(record.get("nation", "")) == _player_nation,
						"priority": float(record.get("priority", 0.0)),
					})
					if budget_usec >= 0 \
							and Time.get_ticks_usec() - start >= budget_usec:
						return
				_layout_job_entries.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
					if bool(a["hovered"]) != bool(b["hovered"]):
						return bool(a["hovered"])
					if bool(a["capital"]) != bool(b["capital"]):
						return bool(a["capital"])
					if bool(a["player"]) != bool(b["player"]):
						return bool(a["player"])
					return float(a["priority"]) > float(b["priority"])
				)
				_layout_job_index = 0
				_layout_job_phase = 1
			1:
				while _layout_job_index < _layout_job_entries.size():
					_layout_accept_city(_layout_job_entries[_layout_job_index])
					_layout_job_index += 1
					if budget_usec >= 0 \
							and Time.get_ticks_usec() - start >= budget_usec:
						return
				_layout_job_index = 0
				_layout_job_phase = 2
			2:
				while _layout_job_index < _layout_job_ports.size():
					var record: Dictionary = _layout_job_ports[_layout_job_index]
					_layout_job_index += 1
					_layout_accept_port(record, camera)
					if budget_usec >= 0 \
							and Time.get_ticks_usec() - start >= budget_usec:
						return
				_layout_job_phase = 3
			_:
				# Rótulos que voltaram ao pool não podem ficar na tela com o texto
				# da cidade anterior (fantasmas ao reduzir o conjunto visível).
				for label in _label_free:
					if label.visible:
						label.visible = false
				_layout_job_active = false
				_icons_dirty = true
				_flags_dirty = true
				# A grade de ocupação só está completa agora: o nome de nação
				# precisa ser reavaliado contra ela (senão fica sobre um nome de
				# cidade aceito no fim do job, ou escondido por um bloco que a
				# nova câmera descartou).
				_nation_labels_dirty = true
				return


## Aceita (ou não) o nome de uma cidade, na ordem de prioridade já ordenada.
func _layout_accept_city(entry: Dictionary) -> void:
	var record: Dictionary = entry["record"]
	var icon_rect: Rect2 = entry["icon_rect"]
	var text_rect: Rect2 = entry["text_rect"]
	var flag_rect: Rect2 = entry["flag_rect"]
	var text_allowed := float(record.get("label_alpha", 1.0)) \
			>= LABEL_TEXT_MIN_ALPHA
	if text_allowed and not bool(entry["hovered"]):
		# Declutter: teto de nomes por célula de tela (além da colisão).
		var cell := _grid_key(text_rect.get_center())
		if int(_label_cell_counts.get(cell, 0)) >= LABEL_MAX_PER_CELL:
			text_allowed = false
		elif _grid_hits(_label_grid, text_rect, LABEL_COLLIDE_PAD_PX):
			text_allowed = false
	var label: Label3D = null
	if text_allowed:
		label = _take_label()
		if label == null:
			# Pool esgotado: as cidades de menor prioridade ficam só com o ícone.
			text_allowed = false
	if label != null:
		_configure_city_label(label, record)
		_active_labels.append(record)
		var cell := _grid_key(text_rect.get_center())
		_label_cell_counts[cell] = int(_label_cell_counts.get(cell, 0)) + 1
	var block: Rect2 = icon_rect
	# A bandeira da capital também ocupa tela: entra no bloco de colisão.
	if flag_rect.size.x > 0.0:
		block = block.merge(flag_rect)
	if text_allowed:
		block = block.merge(text_rect)
	_grid_insert(_label_grid, block)
	var icon: Sprite3D = record.get("icon")
	var flag: Variant = record.get("flag")
	var flag_dict: Dictionary = flag if flag is Dictionary else {}
	var flag_loaded := not flag_dict.is_empty() \
			and flag_dict.get("texture") != null
	_label_rects.append({
		"id": String(record.get("id", "")),
		"kind": "city",
		"rect": block,
		"text_rect": text_rect,
		"icon_rect": icon_rect,
		"flag_rect": flag_rect,
		"text_visible": text_allowed,
		"marker_visible": icon == null or icon.visible,
		"flag_visible": flag_loaded \
				and float(record.get("label_alpha", 0.0)) > 0.01,
		"flag_loaded": flag_loaded,
		"label_alpha": float(record.get("label_alpha", 1.0)),
		"capital": bool(record.get("capital", false)),
		"population": float(record.get("priority", 0.0)),
		"same_city": false,
		"fused": false,
		"badge": false,
		"city_id": "",
		"offset": Vector2.ZERO,
	})
	_layout_job_city_blocks[String(record.get("id", ""))] = {
		"block": block,
		"icon": icon_rect,
	}


## Portos por último: nunca deslocam rótulos, só o próprio ícone.
func _layout_accept_port(record: Dictionary, camera: Camera3D) -> void:
	record["draw"] = false
	record["badge"] = false
	if float(record.get("label_alpha", 0.0)) <= 0.01:
		return
	var metrics := _port_metrics(record, camera)
	if _metrics_rect(metrics).size.x <= 0.0:
		# Porto atrás da câmera/fora do frustum: não desenha neste frame.
		_append_port_report(record, Rect2(), false, Vector2.ZERO)
		return
	var city_info: Dictionary = _layout_job_city_blocks.get(
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
				return
		# Sem selo da cidade na tela ou sem vaga ao lado dele: o modelo 3D
		# fica na costa e o ícone não é desenhado neste frame.
		_append_port_report(record, Rect2(), false, Vector2.ZERO)
		return
	var chosen := Vector2.ZERO
	# A posição de costa é o candidato preferido; só projeta o lado do mar (e
	# os deslocamentos) quando ela de fato colide.
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
			return
		if not chosen.is_zero_approx():
			_apply_port_offset(record, chosen, camera)
			metrics = _port_metrics(record, camera)
			# O deslocamento pode tirar o porto do frustum (borda da tela,
			# encosta atrás da câmera): sem retângulo, nada de desenhar.
			if _metrics_rect(metrics).size.x <= 0.0:
				_append_port_report(record, Rect2(), false, Vector2.ZERO)
				return
	var final_rect := _metrics_rect(metrics)
	if final_rect.size.x <= 0.0:
		_append_port_report(record, Rect2(), false, Vector2.ZERO)
		return
	record["draw"] = true
	_drawn_ports.append(record)
	_grid_insert(_label_grid, final_rect)
	_append_port_report(record, final_rect, true, chosen)


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
		"flag_rect": Rect2(),
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


## Retângulo de tela da bandeira da capital no frame, na mesma posição do
## MultiMesh (`_flag_world_center`). Sem bandeira carregada ou com alfa apagado
## devolve `Rect2` vazio — o layout trata isso como "não ocupa tela".
func _flag_rect(record: Dictionary, camera: Camera3D) -> Rect2:
	var flag: Variant = record.get("flag")
	var flag_dict: Dictionary = flag if flag is Dictionary else {}
	if flag_dict.is_empty() or flag_dict.get("texture") == null:
		return Rect2()
	if float(record.get("label_alpha", 0.0)) <= 0.01:
		return Rect2()
	var center := _flag_world_center(record)
	if camera.is_position_behind(center):
		return Rect2()
	var em := float(record["font_size"]) * float(record["pixel_size"])
	var flag_h := em * PLACE_ICON_HEIGHT * FLAG_SCALE
	return _world_rect_to_screen(camera, center, flag_h, flag_h * 0.675)


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


## Tamanho do rótulo/marcador no frame. O alfa NÃO é mais uma função contínua da
## distância: o nível de zoom (PERTO/MÉDIO/LONGE) define o alvo e o fade de
## 150 ms é animado em `_advance_level_fade`.
##
## O passo é quantizado (distância arredondada; `pixel_size` é proporcional a
## ela): enquanto o passo não muda, escrever visibilidade/alfa de cada sprite de
## novo não muda pixel nenhum — e a varredura roda a cada 1/12 s, sobre centenas
## de registros. `_refresh_level_targets` apaga o cache na troca de nível.
func _apply_place_label_lod(record: Dictionary, distance: float) -> void:
	record["pixel_size"] = clampf(LABEL_PIXEL_PER_UNIT * distance,
			LABEL_PIXEL_MIN, LABEL_PIXEL_MAX)
	var step := roundi(distance)
	if int(record.get("lod_step", -1)) == step:
		return
	record["lod_step"] = step
	var alpha := float(record.get("label_alpha", 1.0))
	var marker := _city_marker_wanted(record)
	var icon: Sprite3D = record.get("icon")
	if icon != null:
		icon.visible = marker
		icon.modulate.a = 1.0 if bool(record.get("capital", false)) else alpha
	var ring: Sprite3D = record.get("ring")
	if ring != null:
		# O selo é apenas o fundo do ícone: sem glifo não há bolinha, e no LONGE
		# a capital fica só com a estrela.
		ring.visible = marker and _zoom_level != ZoomLevel.FAR
		ring.transparency = 0.0


## LOD da âncora de porto. O selo (e o modelo 3D) só existe no nível PERTO;
## nos demais o registro assenta e o que já foi construído é escondido.
func _apply_port_lod(record: Dictionary, distance: float,
		icon_on_screen: bool) -> void:
	if _zoom_level != ZoomLevel.NEAR:
		if not bool(record.get("lod_settled", false)):
			record["lod_settled"] = true
			var model: Node3D = record.get("model")
			if model != null:
				model.visible = false
		return
	record["lod_settled"] = false
	record["pixel_size"] = clampf(LABEL_PIXEL_PER_UNIT * distance,
			LABEL_PIXEL_MIN, LABEL_PIXEL_MAX)
	record["marker_world"] = record["marker_base"] \
			+ Vector3(record["wrap_x"], 0.0, 0.0)
	# Badge da própria cidade: usa a posição ao lado do ícone calculada no
	# layout, para o selo não voltar para cima da cidade nos frames sem layout.
	if _port_is_badge(record) and record.has("badge_local"):
		record["marker_world"] = record["badge_local"] \
				+ Vector3(record["wrap_x"], 0.0, 0.0)
	if icon_on_screen and float(record.get("label_alpha", 0.0)) > 0.01:
		_visible_port_records.append(record)
	_update_port_model(record, distance, icon_on_screen)


func _apply_city_lod(record: Dictionary, distance: float,
		on_screen: bool) -> void:
	if on_screen and _city_marker_wanted(record):
		_visible_city_records.append(record)
	_apply_place_label_lod(record, distance)
	_update_urban(record, distance, on_screen)


## Pede a construção do tecido urbano quando a câmera se aproxima E a cidade
## está na tela (construir fora do frustum só gerava picos de 5 ms por frame à
## toa). Depois de construída, só alterna a visibilidade (histerese evita
## reconstrução em vaivém).
##
## Vale para TODAS as cidades, não só as 40 detalhadas: ao aproximar, qualquer
## cidade ganha prédios/blocos na própria posição e no tamanho do estágio de
## população. Não há disco de fundo nem clareamento do terreno — o solo urbano
## é o shader orgânico escuro do `UrbanGrowthManager`.
func _update_urban(record: Dictionary, distance: float,
		on_screen: bool) -> void:
	var urban: Node3D = record.get("urban")
	var near := _zoom_level == ZoomLevel.NEAR
	if urban == null:
		# Liberado pelo teto de modelos: só reconstrói quando a câmera mudou
		# de faixa (aproximou o bastante) em relação ao ponto em que liberou.
		if bool(record.get("urban_hold", false)):
			if record.has("urban_hold_distance") \
					and distance >= float(record["urban_hold_distance"]) \
							- URBAN_HOLD_RELEASE:
				return
			record["urban_hold"] = false
		if near and on_screen and distance <= URBAN_BUILD_DISTANCE \
				and not bool(record.get("urban_queued", false)):
			record["urban_queued"] = true
			_urban_queue.append(record)
		return
	var want_visible := near and on_screen and distance <= CITY_VISIBLE_DISTANCE
	if urban.visible != want_visible:
		urban.visible = want_visible
	var target: Vector3 = record["position"] \
			+ Vector3(record["wrap_x"], 0.0, 0.0) \
			+ Vector3(0.0, URBAN_GROUND_LIFT, 0.0)
	if not urban.position.is_equal_approx(target):
		urban.position = target


## Consome as filas de construção detalhada (cidade e porto) com orçamento de
## 1,5 ms por frame. Cada modelo entra no máximo um por frame.
func _process_lod_queues(camera: Camera3D, allow_urban := true) -> void:
	if _urban_queue.is_empty() and _port_model_queue.is_empty():
		return
	var camera_local := to_local(camera.global_position)
	var start := Time.get_ticks_usec()
	if allow_urban:
		while not _urban_queue.is_empty():
			var record: Dictionary = _urban_queue.pop_front()
			record["urban_queued"] = false
			if _zoom_level != ZoomLevel.NEAR:
				continue
			if record.get("urban") != null:
				continue
			if _camera_distance(record, camera_local) > URBAN_BUILD_DISTANCE:
				continue
			if not _in_frustum(record, camera):
				continue
			record["urban"] = _build_urban(record)
			if Time.get_ticks_usec() - start >= LOD_BUILD_BUDGET_USEC:
				break
	_trim_urban_models(camera_local)
	while not _port_model_queue.is_empty():
		var record: Dictionary = _port_model_queue.pop_front()
		record["model_queued"] = false
		if _zoom_level != ZoomLevel.NEAR:
			continue
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


## Mantém no máximo `MAX_URBAN_MODELS` tecidos urbanos vivos. Sem isso, um pan
## pelo mundo inteiro acumularia 858 modelos de cidade. Libera os mais
## distantes e marca `urban_hold` para não reconstruir o mesmo enquanto a
## câmera não se afastar (evita o vaivém construir/liberar).
func _trim_urban_models(camera_local: Vector3) -> void:
	if _live_urban_models <= MAX_URBAN_MODELS:
		return
	var live: Array[Dictionary] = []
	for record in _city_nodes:
		if record.get("urban") != null:
			live.append(record)
	if live.size() <= MAX_URBAN_MODELS:
		_live_urban_models = live.size()
		return
	live.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return _camera_distance(a, camera_local) > _camera_distance(b, camera_local)
	)
	var target := maxi(1, MAX_URBAN_MODELS - URBAN_TRIM_HYSTERESIS)
	var excess := live.size() - target
	if excess <= 0:
		_live_urban_models = live.size()
		return
	for index in excess:
		var urban: Node3D = live[index]["urban"]
		urban.queue_free()
		live[index]["urban"] = null
		live[index]["urban_hold"] = true
		live[index]["urban_hold_distance"] = _camera_distance(live[index], camera_local)
	_live_urban_models = live.size() - excess


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
	if _zoom_level != ZoomLevel.NEAR:
		# Fora do nível PERTO o porto nem é desenhado: só assenta e esconde o
		# modelo já construído (sem distância/frustum por registro).
		_apply_port_lod(record, 0.0, false)
		return
	dx += wrap
	var dz := position.z - camera_local.z
	# Distância 3D (inclui a altura da câmera): referência do LOD de modelo.
	var dy := position.y - camera_local.y
	var distance := sqrt(dx * dx + dy * dy + dz * dz)
	var on_screen := _in_view(record, camera)
	_apply_port_lod(record, distance, on_screen)


## LOD de uma cidade: wrap, distância e (só perto) checagem de frustum.
func _apply_lod_city(record: Dictionary, camera_local: Vector3,
		camera: Camera3D) -> void:
	var position: Vector3 = record["position"]
	var dx := position.x - camera_local.x
	var wrap := -roundf(dx / PLANE_SIZE.x) * PLANE_SIZE.x
	record["wrap_x"] = wrap
	var detailed := bool(record.get("detailed", false))
	if _zoom_level == ZoomLevel.FAR and not detailed:
		# Nível estratégico: sem nome/tecido; o ícone usa um pixel_size único do
		# zoom (sem sqrt por cidade). Só a capital entra — as demais cidades não
		# têm marcador nenhum no LONGE.
		record["pixel_size"] = clampf(LABEL_PIXEL_PER_UNIT * _zoom_distance,
				LABEL_PIXEL_MIN, LABEL_PIXEL_MAX)
		if _city_marker_wanted(record) and _in_view(record, camera):
			_visible_city_records.append(record)
		return
	dx += wrap
	var dz := position.z - camera_local.z
	# Mesma distância 3D do porto/cidade (ver `_apply_lod_port`).
	var dy := position.y - camera_local.y
	var distance := sqrt(dx * dx + dy * dy + dz * dz)
	# Culling de viewport: só entra na lista visível (ícones, nomes e tecido) o
	# que está dentro do frustum. O layout ganha margem de tela própria para o
	# nome encostado na borda.
	var on_screen := _in_view(record, camera)
	_apply_city_lod(record, distance, on_screen)


func _build_urban(record: Dictionary) -> Node3D:
	var city: Dictionary = record["city"]
	var manager := UrbanGrowthScript.new() as Node3D
	manager.name = "Urban_" + String(record["id"])
	manager.position = record["position"] \
			+ Vector3(record["wrap_x"], 0.0, 0.0) \
			+ Vector3(0.0, URBAN_GROUND_LIFT, 0.0)
	manager.scale = Vector3.ONE * URBAN_SCALE
	_places.add_child(manager)

	manager.setup(String(record["id"]))
	# Crescimento orgânico só em terra firme e dentro do território da nação.
	manager.buildable_filter = _make_urban_filter(
			manager.position, String(city.get("nation", "")))
	# O tecido urbano cresce pela aglomeração da fonte (`sim_population`), não
	# pela população nacional rateada — senão toda cidade viraria metrópole.
	var urban_population := float(city.get("sim_population", 0.0))
	if urban_population <= 0.0:
		urban_population = float(city.get("population", 0.0))
	manager.apply_indicators({
		"population": urban_population,
		"industrialization": float(city.get("industrialization", 0.0)),
		"income": float(city.get("income", 0.5)),
		"jobs": float(city.get("jobs", 0.0)),
	})
	manager.apply_industry(IndustryProfileScript.derive(city))
	_live_urban_models += 1
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
## Atualiza os MultiMesh de ícones (cidade/porto). Só roda quando `_icons_dirty`
## está ligado (layout/zoom/fade/wrap): pan puro não reescreve o buffer, porque
## as instâncias vivem no mundo e a câmera não muda a posição delas.
func _apply_icon_transforms(camera: Camera3D) -> void:
	if camera == null:
		return
	_icons_dirty = false
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
		# Troca de nível: a lista visível pode ser do nível anterior; no
		# MÉDIO/LONGE só entra quem tem marcador agora.
		if not _city_marker_wanted(record):
			continue
		# A lista já passou pelo culling de tela no scan; a instância usa a
		# posição LOCAL do mapa (sem to_global/to_local por ícone).
		var local: Vector3 = record["anchor"] \
				+ Vector3(float(record.get("wrap_x", 0.0)), 0.0, 0.0)
		var size := _city_icon_size(record)
		if bool(record.get("capital", false)):
			_set_mm_local(_city_star_mm, star_index, local, size)
			star_index += 1
		else:
			_set_mm_local(_city_icon_mm, plain_index, local, size)
			plain_index += 1
		# O selo escuro é só o fundo do ícone; sem glifo (ou no LONGE) não há
		# bolinha solta no mapa.
		if _zoom_level != ZoomLevel.FAR:
			_set_mm_local(_city_ring_mm, ring_index, local, size * RING_SCALE)
			ring_index += 1
	_set_mm_count(_city_icon_mm, plain_index)
	_set_mm_count(_city_star_mm, star_index)
	_set_mm_count(_city_ring_mm, ring_index)

	var port_index := 0
	var port_ring := 0
	if _zoom_level == ZoomLevel.NEAR:
		for record in _drawn_ports:
			var alpha := float(record.get("label_alpha", 1.0))
			if alpha <= 0.01:
				continue
			var local: Vector3 = record["marker_world"]
			var size := _port_icon_height(record) * alpha
			_set_mm_local(_port_icon_mm, port_index, local, size)
			port_index += 1
			if not _port_is_badge(record):
				_set_mm_local(_port_ring_mm, port_ring, local,
						size * RING_SCALE)
				port_ring += 1
	_set_mm_count(_port_icon_mm, port_index)
	_set_mm_count(_port_ring_mm, port_ring)


func _city_icon_size(record: Dictionary) -> float:
	var em := float(record["font_size"]) * float(record["pixel_size"])
	var factor := FAR_CAPITAL_ICON_SCALE \
			if _zoom_level == ZoomLevel.FAR \
					and bool(record.get("capital", false)) else 1.0
	return em * PLACE_ICON_HEIGHT * factor


func _city_icon_world(record: Dictionary) -> Vector3:
	return to_global(record["anchor"] + Vector3(record["wrap_x"], 0.0, 0.0))


func _set_mm_local(instance: MultiMeshInstance3D, index: int,
		local: Vector3, size: float) -> void:
	if instance == null or instance.multimesh == null or index < 0:
		return
	if index >= instance.multimesh.instance_count:
		return
	instance.multimesh.set_instance_transform(index,
			Transform3D(Basis().scaled(Vector3(size, size, size)), local))


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
