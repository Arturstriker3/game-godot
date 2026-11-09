extends RefCounted
class_name UrbanPersistence

## Salva e restaura o layout de uma cidade. Como a geração é determinística, o
## arquivo guarda os lotes já construídos e um hash para conferir que o estado
## carregado é exatamente o que foi salvo.

const VERSION := 1


static func save_city(path: String, manager: UrbanGrowthManager) -> bool:
	var payload := {
		"version": VERSION,
		"signature": manager.layout.signature(),
		"city": manager.to_dict(),
	}
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("Não foi possível salvar a cidade em " + path)
		return false
	file.store_string(JSON.stringify(payload, "\t"))
	file.close()
	return true


static func load_city(path: String, manager: UrbanGrowthManager) -> bool:
	if not FileAccess.file_exists(path):
		return false

	var parsed = JSON.parse_string(FileAccess.get_file_as_string(path))
	if typeof(parsed) != TYPE_DICTIONARY:
		return false
	if int(parsed.get("version", 0)) != VERSION:
		return false

	var city: Dictionary = parsed.get("city", {})
	if city.is_empty():
		return false

	manager.restore(city)
	return manager.layout.signature() == int(parsed.get("signature", 0))
