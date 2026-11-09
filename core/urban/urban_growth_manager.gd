extends Node3D
class_name UrbanGrowthManager

## Dono do crescimento visual de uma cidade. Fica separado da simulação
## econômica: recebe indicadores prontos, decide o estágio, pede o plano de
## bairros e entrega os lotes ao renderer.
##
## Regra central: a cidade só cresce — lotes existentes nunca são removidos,
## apenas melhorados (casa -> prédio -> torre).

signal growth_changed(stage: int, lot_count: int)

const MAX_POPULATION := 14_000_000.0
const POPULATION_WEIGHT := 0.65
const INDUSTRIAL_WEIGHT := 0.35

var city_id := ""
var layout: CityLayout
var industry_profile: Dictionary = {}

## Filtro opcional de espaço construível (água, fronteira). Recebe a posição
## do lote em espaço local e devolve true quando pode construir.
var buildable_filter: Callable = Callable()

var _library: BuildingLibrary
var _planner: DistrictPlanner
var _industrial_planner: IndustrialPlanner
var _renderer: UrbanRenderer
var _ground: MeshInstance3D

## Solo urbano: mancha de terra com borda orgânica e suave, sem contorno duro.
const GROUND_SHADER_CODE := """
shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_disabled, shadows_disabled;

uniform vec4 tint : source_color = vec4(0.30, 0.26, 0.20, 0.72);
uniform float seed = 0.0;

float hash(vec2 p) {
	return fract(sin(dot(p, vec2(127.1, 311.7)) + seed) * 43758.5453);
}

float noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x),
			mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

void fragment() {
	vec2 p = UV * 2.0 - 1.0;
	float r = length(p);
	float a = atan(p.y, p.x);
	float edge = 0.80 + 0.10 * sin(3.0 * a + seed) + 0.06 * sin(5.0 * a + seed * 1.7)
			+ 0.12 * (noise(p * 3.0) - 0.5);
	float mask = 1.0 - smoothstep(edge * 0.45, edge, r);
	float grain = 0.85 + 0.3 * noise(p * 14.0);
	ALBEDO = tint.rgb * grain;
	ALPHA = mask * tint.a;
}
"""


## Prepara a cidade vazia. O seed padrão vem do id, então a mesma cidade sempre
## nasce igual.
func setup(p_city_id: String, seed_value: int = 0) -> void:
	city_id = p_city_id
	_planner = DistrictPlanner.new()
	_industrial_planner = IndustrialPlanner.new()
	_library = BuildingLibrary.new()
	_library.load_library()

	_renderer = UrbanRenderer.new()
	_renderer.name = "UrbanRenderer"
	add_child(_renderer)
	_renderer.setup(_library)

	layout = CityLayout.new()
	layout.city_id = city_id
	layout.seed_value = seed_value if seed_value != 0 else absi(city_id.hash())


## Aplica população/industrialização e atualiza apenas o que cresceu.
func apply_indicators(indicators: Dictionary) -> void:
	var population := float(indicators.get("population", 0.0))
	var industrialization := clampf(float(indicators.get("industrialization", 0.0)), 0.0, 1.0)
	var income := clampf(float(indicators.get("income", 0.5)), 0.0, 1.0)
	var jobs := maxf(float(indicators.get("jobs", 0.0)), 0.0)

	var previous_stage := layout.stage
	layout.stage = maxi(layout.stage, stage_for(population, industrialization))

	var index := layout.lot_index()
	var added := 0
	var upgraded := 0
	for lot in _planner.desired_lots(layout, industrialization, income, jobs):
		if not _is_buildable(lot):
			continue

		var key := "%d,%d" % [int(lot["i"]), int(lot["j"])]
		if not index.has(key):
			layout.add_lot(lot)
			index[key] = lot
			added += 1
			continue

		# Bairro antigo: preserva o módulo, mas aceita verticalização.
		var current: Dictionary = index[key]
		if DistrictPlanner.can_upgrade(current["module"], lot["module"]):
			current["module"] = lot["module"]
			current["yaw"] = lot["yaw"]
			upgraded += 1

	if added > 0 or upgraded > 0 or layout.stage != previous_stage:
		_refresh_render()
		growth_changed.emit(layout.stage, _total_lots())


## Crescimento industrial: independente do tecido urbano. O perfil vem da
## economia (frações por setor + intensidade) e só adiciona/atualiza lotes.
func apply_industry(profile: Dictionary) -> void:
	industry_profile = profile.duplicate(true)
	var intensity := clampf(float(profile.get("intensity", 0.0)), 0.0, 1.0)

	var previous_stage := layout.industry_stage
	layout.industry_stage = maxi(layout.industry_stage, _industry_stage_for(intensity))

	var index := layout.industrial_index()
	var changed := 0
	for lot in _industrial_planner.desired_lots(layout.seed_value, profile,
			layout.industry_stage):
		if not _is_buildable(lot):
			continue

		var key := String(lot["key"])
		if not index.has(key):
			layout.add_industrial_lot(lot)
			index[key] = lot
			changed += 1
			continue

		# Complexo existente: aceita só modernização (nível maior).
		var current: Dictionary = index[key]
		if IndustrialPlanner.can_upgrade(String(current["module"]), String(lot["module"])):
			current["module"] = lot["module"]
			current["level"] = int(lot.get("level", 1))
			changed += 1

	if changed > 0 or layout.industry_stage != previous_stage:
		_refresh_render()
		growth_changed.emit(layout.stage, _total_lots())


func _industry_stage_for(intensity: float) -> int:
	if intensity < 0.35:
		return 0
	if intensity < 0.7:
		return 1
	return 2


func _total_lots() -> int:
	return layout.lots.size() + layout.industrial_lots.size()


## Redesenha urbano + industrial juntos; o renderer agrupa por arquétipo.
func _refresh_render() -> void:
	_renderer.apply_lots(layout.lots + layout.industrial_lots)
	_update_ground()


## Ajusta o solo urbano ao tamanho atual da cidade (núcleo + distrito).
func _update_ground() -> void:
	var extent := 0.0
	for lot in layout.lots:
		extent = maxf(extent, Vector2(float(lot["x"]), float(lot["z"])).length())
	if _ground == null:
		var shader := Shader.new()
		shader.code = GROUND_SHADER_CODE
		var material := ShaderMaterial.new()
		material.shader = shader
		material.set_shader_parameter("seed", float(layout.seed_value % 997))
		_ground = MeshInstance3D.new()
		_ground.name = "UrbanGround"
		_ground.mesh = PlaneMesh.new()
		_ground.material_override = material
		_ground.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		_ground.position.y = 0.004
		add_child(_ground)
	var size := (extent + 0.9) * 2.0
	(_ground.mesh as PlaneMesh).size = Vector2(size, size)


## Estágio 0 (núcleo), 1 (bairros + indústria) e 2 (metrópole).
func stage_for(population: float, industrialization: float) -> int:
	var population_norm := clampf(population / MAX_POPULATION, 0.0, 1.0)
	var score := population_norm * POPULATION_WEIGHT + industrialization * INDUSTRIAL_WEIGHT
	if score < 0.33:
		return 0
	if score < 0.66:
		return 1
	return 2


func to_dict() -> Dictionary:
	return {"layout": layout.to_dict(), "stage": layout.stage}


func restore(data: Dictionary) -> void:
	if not data.has("layout"):
		return
	layout = CityLayout.from_dict(data["layout"])
	_refresh_render()
	growth_changed.emit(layout.stage, _total_lots())


func drawn_instances() -> int:
	return _renderer.drawn_instances()


func _is_buildable(lot: Dictionary) -> bool:
	if not buildable_filter.is_valid():
		return true
	return bool(buildable_filter.call(Vector3(float(lot["x"]), 0.0, float(lot["z"]))))
