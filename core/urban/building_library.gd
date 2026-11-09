extends RefCounted
class_name BuildingLibrary

## Biblioteca modular de edifícios gerada no Blender (assets/models/city_modules).
## Cada arquétipo vira um Mesh com materiais próprios para MultiMesh e tint por
## instância.

const LIBRARY_PATH := "res://assets/models/city_modules/library.json"

## Cor usada pelo LOD distante, por categoria.
const CATEGORY_COLORS := {
	"residential": Color("#6E6A5F"),
	"commercial": Color("#6B6E70"),
	"industrial": Color("#5C5A54"),
	"public": Color("#7A756A"),
	"street": Color("#2A2A28"),
}

var archetypes: Array[Dictionary] = []

var _by_id: Dictionary = {}

## Cache compartilhado: as malhas dos módulos são idênticas para toda cidade.
## Sem isso, cada cidade recarrega os ~22 GLBs (causa hitch no zoom/startup).
static var _shared: Array[Dictionary] = []
static var _shared_by_id: Dictionary = {}


func load_library() -> void:
	if not _shared.is_empty():
		archetypes = _shared
		_by_id = _shared_by_id
		return

	archetypes.clear()
	_by_id.clear()

	var file := FileAccess.open(LIBRARY_PATH, FileAccess.READ)
	if file == null:
		push_error("Biblioteca de módulos ausente: " + LIBRARY_PATH)
		return

	var data: Dictionary = JSON.parse_string(file.get_as_text())
	for entry in data.get("modules", []):
		var archetype := _build_archetype(entry)
		if archetype.is_empty():
			continue
		archetypes.append(archetype)
		_by_id[archetype["id"]] = archetype
	file.close()

	_shared = archetypes
	_shared_by_id = _by_id


func get_archetype(module_id: String) -> Dictionary:
	return _by_id.get(module_id, {})


func _build_archetype(entry: Dictionary) -> Dictionary:
	var scene: PackedScene = load(entry["file"])
	if scene == null:
		push_warning("Módulo não encontrado: " + str(entry["file"]))
		return {}

	var instance := scene.instantiate()
	var mesh_instance := _find_mesh_instance(instance)
	if mesh_instance == null:
		instance.free()
		push_warning("Módulo sem MeshInstance3D: " + str(entry["id"]))
		return {}

	var footprint := Vector2(entry["footprint"][0], entry["footprint"][1])
	var archetype := {
		"id": entry["id"],
		"category": entry["category"],
		"zone": entry["zone"],
		"footprint": footprint,
		"height": float(entry["height"]),
		"mesh": _prepare_mesh(mesh_instance.mesh),
	}
	instance.free()
	return archetype


## O tint por instância só aparece se o material multiplicar a cor do vértice.
func _prepare_mesh(source: Mesh) -> Mesh:
	var mesh: Mesh = source.duplicate()
	for surface in mesh.get_surface_count():
		var material := mesh.surface_get_material(surface)
		if material is BaseMaterial3D:
			var copy: BaseMaterial3D = material.duplicate()
			copy.vertex_color_use_as_albedo = true
			mesh.surface_set_material(surface, copy)
	return mesh


func _find_mesh_instance(root: Node) -> MeshInstance3D:
	if root is MeshInstance3D and root.mesh != null:
		return root
	for child in root.get_children():
		var found := _find_mesh_instance(child)
		if found != null:
			return found
	return null
