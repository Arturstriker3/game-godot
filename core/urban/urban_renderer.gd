extends Node3D
class_name UrbanRenderer

## Desenha os lotes da cidade em MultiMeshInstance3D: um buffer por arquétipo
## perto da câmera e uma caixa simplificada por categoria no LOD distante.
## A atualização é incremental por arquétipo (só o que mudou é reenviado).

const NEAR_RANGE_END := 140.0
const FAR_RANGE_BEGIN := 140.0
const FAR_RANGE_END := 900.0

## Ruas são tiles processuais (não vêm da biblioteca do Blender).
const STREET_ID := "rua"
const STREET_SIZE := 0.5
const STREET_Y := 0.006

var _library: BuildingLibrary
var _near: Dictionary = {}
var _far: Dictionary = {}
var _signatures: Dictionary = {}
var _street_archetype: Dictionary = {}


func setup(library: BuildingLibrary) -> void:
	_library = library


## Reconstrói apenas os arquétipos cujos lotes mudaram.
func apply_lots(lots: Array[Dictionary]) -> void:
	var by_module := {}
	var by_category := {}

	for lot in lots:
		var archetype: Dictionary = _archetype(lot["module"])
		if archetype.is_empty():
			continue
		if not by_module.has(lot["module"]):
			by_module[lot["module"]] = []
		by_module[lot["module"]].append(lot)
		if lot["module"] == STREET_ID:
			continue
		if not by_category.has(archetype["category"]):
			by_category[archetype["category"]] = []
		by_category[archetype["category"]].append(lot)

	for module_id in _near.keys():
		if not by_module.has(module_id):
			_clear(_near[module_id])
	for category in _far.keys():
		if not by_category.has(category):
			_clear(_far[category])

	for module_id in by_module:
		_update_near(module_id, by_module[module_id])
	for category in by_category:
		_update_far(category, by_category[category])


## Total de instâncias desenhadas no LOD próximo, para o benchmark do protótipo.
func drawn_instances() -> int:
	var total := 0
	for instance in _near.values():
		total += instance.multimesh.instance_count
	return total


func _update_near(module_id: String, lots: Array) -> void:
	var signature := JSON.stringify(lots).hash()
	if _signatures.get(module_id, 0) == signature:
		return
	_signatures[module_id] = signature

	var multimesh: MultiMesh = _ensure_near(module_id).multimesh
	multimesh.instance_count = 0
	multimesh.instance_count = lots.size()
	for index in lots.size():
		multimesh.set_instance_transform(index, _near_transform(lots[index]))
		multimesh.set_instance_color(index, _tint(lots[index]))


func _update_far(category: String, lots: Array) -> void:
	var multimesh: MultiMesh = _ensure_far(category).multimesh
	multimesh.instance_count = 0
	multimesh.instance_count = lots.size()
	for index in lots.size():
		multimesh.set_instance_transform(index, _far_transform(lots[index]))


func _ensure_near(module_id: String) -> MultiMeshInstance3D:
	if _near.has(module_id):
		return _near[module_id]

	var archetype: Dictionary = _archetype(module_id)
	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.use_colors = true
	multimesh.mesh = archetype["mesh"]

	var instance := MultiMeshInstance3D.new()
	instance.name = "Near_" + module_id
	instance.multimesh = multimesh
	instance.visibility_range_end = NEAR_RANGE_END
	add_child(instance)
	_near[module_id] = instance
	return instance


func _ensure_far(category: String) -> MultiMeshInstance3D:
	if _far.has(category):
		return _far[category]

	var box := BoxMesh.new()
	box.size = Vector3.ONE
	var material := StandardMaterial3D.new()
	material.albedo_color = BuildingLibrary.CATEGORY_COLORS.get(category, Color.WHITE)
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	box.material = material

	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.mesh = box

	var instance := MultiMeshInstance3D.new()
	instance.name = "Far_" + category
	instance.multimesh = multimesh
	instance.visibility_range_begin = FAR_RANGE_BEGIN
	instance.visibility_range_end = FAR_RANGE_END
	add_child(instance)
	_far[category] = instance
	return instance


func _near_transform(lot: Dictionary) -> Transform3D:
	var scale: float = float(lot.get("scale", 1.0))
	var basis := Basis(Vector3.UP, float(lot["yaw"])) * Basis.from_scale(Vector3.ONE * scale)
	var height: float = STREET_Y if lot["module"] == STREET_ID else 0.0
	return Transform3D(basis, Vector3(float(lot["x"]), height, float(lot["z"])))


func _far_transform(lot: Dictionary) -> Transform3D:
	var archetype: Dictionary = _archetype(lot["module"])
	var footprint: Vector2 = archetype["footprint"]
	var height: float = archetype["height"]
	var scale: float = float(lot.get("scale", 1.0))
	var size := Vector3(footprint.x * scale, height, footprint.y * scale)
	var basis := Basis(Vector3.UP, float(lot["yaw"])) * Basis.from_scale(size)
	return Transform3D(basis, Vector3(float(lot["x"]), height * 0.5, float(lot["z"])))


func _tint(lot: Dictionary) -> Color:
	var tint: Array = lot.get("tint", [1.0, 1.0, 1.0])
	return Color(tint[0], tint[1], tint[2])


## Arquétipo do módulo; ruas têm um tile processual próprio.
func _archetype(module_id: String) -> Dictionary:
	if module_id != STREET_ID:
		return _library.get_archetype(module_id)
	if _street_archetype.is_empty():
		_street_archetype = _build_street_archetype()
	return _street_archetype


func _build_street_archetype() -> Dictionary:
	var box := BoxMesh.new()
	box.size = Vector3(STREET_SIZE, 0.01, STREET_SIZE)

	var material := StandardMaterial3D.new()
	material.albedo_color = Color("#3B3D37")
	material.roughness = 1.0
	box.material = material

	return {
		"id": STREET_ID,
		"category": "street",
		"zone": "public",
		"footprint": Vector2(STREET_SIZE, STREET_SIZE),
		"height": 0.01,
		"mesh": box,
	}


func _clear(instance: MultiMeshInstance3D) -> void:
	instance.multimesh.instance_count = 0
