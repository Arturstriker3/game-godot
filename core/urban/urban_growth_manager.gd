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
