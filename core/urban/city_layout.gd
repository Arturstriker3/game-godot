extends RefCounted
class_name CityLayout

## Estado persistente de uma cidade modular: seed, estágio e os lotes ocupados.
## O layout é a fonte da verdade do que está construído — o crescimento só
## adiciona ou melhora lotes, nunca remove.

const VERSION := 1

var city_id := ""
var seed_value: int = 0
var stage: int = 0
var lots: Array[Dictionary] = []

## Crescimento industrial é independente do urbano: guarda o estágio industrial
## e os lotes do distrito, com chave própria ("setor:papel:índice").
var industry_stage: int = 0
var industrial_lots: Array[Dictionary] = []


## Índice "i,j" -> lote, para consulta rápida durante o crescimento.
func lot_index() -> Dictionary:
	var index := {}
	for lot in lots:
		index[_key(int(lot["i"]), int(lot["j"]))] = lot
	return index


## Índice "chave" -> lote industrial, para preservar o que já foi construído.
func industrial_index() -> Dictionary:
	var index := {}
	for lot in industrial_lots:
		index[String(lot["key"])] = lot
	return index


func add_lot(lot: Dictionary) -> void:
	lots.append(lot)


func add_industrial_lot(lot: Dictionary) -> void:
	industrial_lots.append(lot)


func to_dict() -> Dictionary:
	return {
		"version": VERSION,
		"city_id": city_id,
		"seed": seed_value,
		"stage": stage,
		"lots": lots.duplicate(true),
		"industry_stage": industry_stage,
		"industrial_lots": industrial_lots.duplicate(true),
	}


static func from_dict(data: Dictionary) -> CityLayout:
	var layout := CityLayout.new()
	layout.city_id = String(data.get("city_id", ""))
	layout.seed_value = int(data.get("seed", 0))
	layout.stage = int(data.get("stage", 0))
	for lot in data.get("lots", []):
		layout.lots.append(lot.duplicate(true))
	layout.industry_stage = int(data.get("industry_stage", 0))
	for lot in data.get("industrial_lots", []):
		layout.industrial_lots.append(lot.duplicate(true))
	return layout


## Assinatura estável do layout, usada para conferir o determinismo ao carregar.
## Usa valores arredondados e ordem canônica para sobreviver ao JSON.
func signature() -> int:
	var parts: PackedStringArray = []
	for lot in lots:
		var tint: Array = lot.get("tint", [1.0, 1.0, 1.0])
		parts.append("u|%d,%d|%s|%d|%d|%d,%d|%d,%d,%d" % [
			int(lot["i"]), int(lot["j"]),
			String(lot["module"]),
			roundi(float(lot["yaw"]) * 1000.0),
			roundi(float(lot.get("scale", 1.0)) * 1000.0),
			roundi(float(lot["x"]) * 1000.0), roundi(float(lot["z"]) * 1000.0),
			roundi(float(tint[0]) * 1000.0), roundi(float(tint[1]) * 1000.0),
			roundi(float(tint[2]) * 1000.0),
		])
	for lot in industrial_lots:
		var tint: Array = lot.get("tint", [1.0, 1.0, 1.0])
		parts.append("i|%s|%s|%d|%d|%d,%d|%d,%d,%d" % [
			String(lot.get("key", "")),
			String(lot["module"]),
			roundi(float(lot["yaw"]) * 1000.0),
			roundi(float(lot.get("scale", 1.0)) * 1000.0),
			roundi(float(lot["x"]) * 1000.0), roundi(float(lot["z"]) * 1000.0),
			roundi(float(tint[0]) * 1000.0), roundi(float(tint[1]) * 1000.0),
			roundi(float(tint[2]) * 1000.0),
		])
	parts.sort()
	return ",".join(parts).hash()


static func _key(i: int, j: int) -> String:
	return "%d,%d" % [i, j]
