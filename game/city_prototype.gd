extends Node3D

## Protótipo isolado de cidades industriais (não altera o mapa principal).
##
## Três versões da mesma cidade (mesmo id, mesmo seed, mesmo tecido urbano) que
## diferem apenas na composição econômica: residencial, industrial e polo
## avançado. O distrito industrial é planejado separadamente do urbano.
##
## Modo automático (para capturas): --auto
## Salva res://docs/industry_prototype/{residencial,industrial,polo_avancado}.png

const PLAYER_SCENE: PackedScene = preload("res://game/player/player.tscn")
const MANAGER_SCRIPT: Script = preload("res://core/urban/urban_growth_manager.gd")
const PERSISTENCE: Script = preload("res://core/urban/urban_persistence.gd")

const SAVE_PATH := "user://industry_prototype.json"
const SHOT_DIR := "res://docs/industry_prototype"

const POPULATION_MIN := 50_000.0
const POPULATION_MAX := 14_000_000.0
const INDUSTRY_MAX := 1.0

## Mesma população e industrialização em todos os cenários: o tecido urbano é
## idêntico e só a composição industrial muda.
const SCENARIOS := [
	{
		"name": "Residencial",
		"slug": "residencial",
		"population": 4_000_000.0,
		"industrialization": 0.5,
		"profile": {
			"intensity": 0.16, "steel": 0.0, "manufacturing": 0.45,
			"refining": 0.0, "power": 0.15, "storage": 0.40,
		},
		"target": Vector3(2.5, 0.0, 0.0),
		"distance": 10.5,
	},
	{
		"name": "Industrial",
		"slug": "industrial",
		"population": 4_000_000.0,
		"industrialization": 0.5,
		"profile": {
			"intensity": 0.65, "steel": 0.42, "manufacturing": 0.40,
			"refining": 0.05, "power": 0.08, "storage": 0.05,
		},
		"target": Vector3(2.5, 0.0, 0.0),
		"distance": 10.5,
	},
	{
		"name": "Polo avançado",
		"slug": "polo_avancado",
		"population": 4_000_000.0,
		"industrialization": 0.5,
		"profile": {
			"intensity": 1.0, "steel": 0.28, "manufacturing": 0.24,
			"refining": 0.20, "power": 0.16, "storage": 0.12,
		},
		"target": Vector3(2.5, 0.0, 0.0),
		"distance": 10.5,
	},
]

var _manager: UrbanGrowthManager
var _player: Node3D
var _camera: Camera3D
var _population_slider: HSlider
var _industry_slider: HSlider
var _stage_label: Label
var _sector_label: Label
var _stats_label: Label
var _ui_layer: CanvasLayer

var _current_scenario := 0
var _auto := false
var _auto_elapsed := 0.0
var _auto_step := 0


func _ready() -> void:
	_auto = "--auto" in OS.get_cmdline_user_args()
	_build_environment()
	_build_ground()
	_build_camera()
	_build_city()
	_build_ui()
	_apply_scenario(0)

	if _auto:
		_ui_layer.visible = false
		DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(SHOT_DIR))


func _process(delta: float) -> void:
	_update_stats()
	if _auto:
		_process_auto(delta)


func _build_environment() -> void:
	# Mesma iluminação do mapa principal (main_game.tscn): fundo escuro, ambiente
	# quente e direcional com sombras — sem céu azul lavando os materiais.
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color("#0A0E0F")
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.62, 0.66, 0.58)
	environment.ambient_light_energy = 0.45
	environment.tonemap_mode = Environment.TONE_MAPPER_ACES

	var world_environment := WorldEnvironment.new()
	world_environment.environment = environment
	add_child(world_environment)

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-50.0, 35.0, 0.0)
	light.light_color = Color(1.0, 0.97, 0.92)
	light.light_energy = 1.1
	light.shadow_enabled = true
	add_child(light)


func _build_ground() -> void:
	var terrain := PlaneMesh.new()
	terrain.size = Vector2(80.0, 80.0)

	var terrain_material := StandardMaterial3D.new()
	terrain_material.albedo_color = Color("#46503E")
	terrain_material.roughness = 1.0

	var ground := MeshInstance3D.new()
	ground.mesh = terrain
	ground.material_override = terrain_material
	ground.name = "Ground"
	add_child(ground)

	# Placa do distrito: o tecido urbano e a indústria não nascem sobre um vazio.
	var plate_mesh := PlaneMesh.new()
	plate_mesh.size = Vector2(34.0, 26.0)

	var plate_material := StandardMaterial3D.new()
	plate_material.albedo_color = Color("#57544C")
	plate_material.roughness = 1.0

	var plate := MeshInstance3D.new()
	plate.mesh = plate_mesh
	plate.material_override = plate_material
	plate.position = Vector3(3.0, 0.002, 0.0)
	plate.name = "CityPlate"
	add_child(plate)


func _build_camera() -> void:
	_player = PLAYER_SCENE.instantiate()
	_player.camera_zoom_min_bound = 6.0
	_player.camera_zoom_max_bound = 80.0
	add_child(_player)

	# Mesma inclinação da câmera do mapa principal (main_game.gd).
	var socket: Node3D = _player.get_node("CameraSocket")
	socket.rotation_degrees = Vector3(-56.0, 0.0, 0.0)

	_camera = socket.get_node("Camera3D")
	_camera.position = Vector3(0.0, 0.0, 13.0)
	_camera.fov = 55.0
	_camera.far = 1200.0


func _build_city() -> void:
	_manager = MANAGER_SCRIPT.new()
	_manager.name = "City"
	add_child(_manager)
	_manager.setup("LDN", 20260101)
	_manager.growth_changed.connect(_on_growth_changed)


func _build_ui() -> void:
	var layer := CanvasLayer.new()
	layer.layer = 20
	add_child(layer)
	_ui_layer = layer

	var panel := PanelContainer.new()
	panel.position = Vector2(16.0, 16.0)
	panel.custom_minimum_size = Vector2(380.0, 0.0)
	panel.add_theme_stylebox_override("panel", UiKit.flat(UiKit.PANEL, UiKit.EDGE, 16.0))
	layer.add_child(panel)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 8)
	panel.add_child(box)

	box.add_child(UiKit.title("Cidades industriais — protótipo", 20))
	_stage_label = UiKit.caption("", UiKit.BRASS)
	box.add_child(_stage_label)
	_sector_label = UiKit.caption("", UiKit.TEXT_DIM)
	box.add_child(_sector_label)
	_stats_label = UiKit.caption("", UiKit.TEXT_MUTED)
	box.add_child(_stats_label)

	box.add_child(UiKit.rule())
	_population_slider = _add_slider(box, "População", POPULATION_MIN, POPULATION_MAX, 50_000.0)
	_industry_slider = _add_slider(box, "Industrialização", 0.0, INDUSTRY_MAX, 0.01)

	box.add_child(UiKit.rule())
	var scenarios := HBoxContainer.new()
	scenarios.add_theme_constant_override("separation", 6)
	box.add_child(scenarios)
	for index in SCENARIOS.size():
		var button := Button.new()
		button.text = String(SCENARIOS[index]["name"])
		button.add_theme_font_size_override("font_size", 11)
		button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		button.pressed.connect(_reset_to.bind(index))
		scenarios.add_child(button)

	var actions := HBoxContainer.new()
	actions.add_theme_constant_override("separation", 6)
	box.add_child(actions)

	var save_button := Button.new()
	save_button.text = "Salvar"
	save_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	save_button.pressed.connect(_save_city)
	actions.add_child(save_button)

	var load_button := Button.new()
	load_button.text = "Carregar"
	load_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	load_button.pressed.connect(_load_city)
	actions.add_child(load_button)

	var reset_button := Button.new()
	reset_button.text = "Reiniciar"
	reset_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	reset_button.pressed.connect(_reset_city)
	actions.add_child(reset_button)


func _add_slider(parent: VBoxContainer, title: String, minimum: float,
		maximum: float, step: float) -> HSlider:
	var label := UiKit.caption(title, UiKit.TEXT_MUTED)
	parent.add_child(label)

	var slider := HSlider.new()
	slider.min_value = minimum
	slider.max_value = maximum
	slider.step = step
	slider.value = minimum
	slider.custom_minimum_size = Vector2(0.0, 18.0)
	slider.value_changed.connect(_on_slider_changed)
	parent.add_child(slider)
	return slider


func _on_slider_changed(_value: float) -> void:
	_apply_indicators()


## Aplica um cenário: mesmo tecido urbano, composição industrial diferente.
func _apply_scenario(index: int) -> void:
	_current_scenario = clampi(index, 0, SCENARIOS.size() - 1)
	var scenario: Dictionary = SCENARIOS[_current_scenario]
	_population_slider.set_value_no_signal(scenario["population"])
	_industry_slider.set_value_no_signal(scenario["industrialization"])
	_apply_indicators()
	_manager.apply_industry(scenario["profile"])
	_frame_scenario(scenario)
	_update_labels()


func _frame_scenario(scenario: Dictionary) -> void:
	_player.position = scenario["target"]
	_camera.position.z = float(scenario["distance"])


func _apply_indicators() -> void:
	var population := _population_slider.value
	var industrialization := _industry_slider.value
	_manager.apply_indicators({
		"population": population,
		"industrialization": industrialization,
		"income": clampf(0.15 + industrialization * 0.8, 0.0, 1.0),
		"jobs": population * industrialization * 0.1,
	})
	_update_labels()


func _reset_to(index: int) -> void:
	_current_scenario = clampi(index, 0, SCENARIOS.size() - 1)
	if _manager != null:
		_manager.queue_free()
	_build_city()
	_apply_scenario(_current_scenario)


func _reset_city() -> void:
	_reset_to(_current_scenario)


func _save_city() -> void:
	_save_feedback(PERSISTENCE.save_city(SAVE_PATH, _manager))


func _load_city() -> void:
	_save_feedback(PERSISTENCE.load_city(SAVE_PATH, _manager))


func _save_feedback(success: bool) -> void:
	_stats_label.text = "Estado salvo/carregado" if success else "Falha ao salvar/carregar"


func _on_growth_changed(_stage: int, _lot_count: int) -> void:
	_update_labels()


func _update_labels() -> void:
	_update_stage_label()
	_update_sector_label()


func _update_stage_label() -> void:
	if _stage_label == null or _manager == null:
		return
	var scenario_name: String = String(SCENARIOS[_current_scenario]["name"])
	_stage_label.text = "%s — urbano %d · indústria %d · %d lotes ind." % [
		scenario_name,
		_manager.layout.stage + 1,
		_manager.layout.industry_stage + 1,
		_manager.layout.industrial_lots.size(),
	]


func _update_sector_label() -> void:
	if _sector_label == null or _manager == null:
		return
	var profile: Dictionary = SCENARIOS[_current_scenario]["profile"]
	var parts := PackedStringArray()
	for sector in IndustryProfile.SECTORS:
		var share := float(profile.get(sector, 0.0))
		if share < 0.05:
			continue
		parts.append("%s %d%%" % [_sector_name(sector), roundi(share * 100.0)])
	_sector_label.text = " · ".join(parts) if not parts.is_empty() else "sem indústria"


func _sector_name(sector: String) -> String:
	match sector:
		"steel":
			return "aço"
		"manufacturing":
			return "manufatura"
		"refining":
			return "refino"
		"power":
			return "energia"
		_:
			return "armazenagem"


func _update_stats() -> void:
	if _stats_label == null or _manager == null:
		return
	var draw_calls := int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME))
	_stats_label.text = "pop %s | urb %d | ind %d | inst %d | %d FPS | %d draws" % [
		_format_millions(_population_slider.value),
		_manager.layout.lots.size(),
		_manager.layout.industrial_lots.size(),
		_manager.drawn_instances(),
		Engine.get_frames_per_second(),
		draw_calls,
	]


func _format_millions(value: float) -> String:
	return "%.2f M" % (value / 1_000_000.0)


## Sequência determinística: captura cada vocação e reinicia para a próxima.
func _process_auto(delta: float) -> void:
	_auto_elapsed += delta
	match _auto_step:
		0:
			if _auto_elapsed > 1.2:
				_capture(String(SCENARIOS[0]["slug"]))
				_reset_to(1)
				_auto_step = 1
		1:
			if _auto_elapsed > 2.8:
				_capture(String(SCENARIOS[1]["slug"]))
				_reset_to(2)
				_auto_step = 2
		2:
			if _auto_elapsed > 4.4:
				_capture(String(SCENARIOS[2]["slug"]))
				_auto_step = 3
		3:
			if _auto_elapsed > 4.7:
				print("[industry] urbano=%d industrial=%d instancias=%d fps=%d draws=%d" % [
					_manager.layout.lots.size(),
					_manager.layout.industrial_lots.size(),
					_manager.drawn_instances(),
					Engine.get_frames_per_second(),
					int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)),
				])
				get_tree().quit()


func _capture(file_name: String) -> void:
	var image := get_viewport().get_texture().get_image()
	image.save_png("%s/%s.png" % [SHOT_DIR, file_name])
