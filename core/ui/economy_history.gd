extends RefCounted
class_name EconomyHistory

## Histórico econômico amostrado uma vez por dia de simulação.
##
## O painel nunca varre entidades por frame: recebe o status publicado pelo
## EconomySim no tick e guarda apenas as séries que os gráficos consomem,
## em um anel limitado a MAX_SAMPLES dias.

const MAX_SAMPLES := 360

var _samples: Array[Dictionary] = []


func clear() -> void:
	_samples.clear()


func count() -> int:
	return _samples.size()


func latest() -> Dictionary:
	return _samples.back() if not _samples.is_empty() else {}


## Registra o dia; se ele já existe no fim do anel, substitui (idempotente).
func sample(day: int, city: Dictionary) -> void:
	var entry := _build_entry(day, city)
	if not _samples.is_empty() and int(_samples.back()["day"]) == day:
		_samples[_samples.size() - 1] = entry
		return
	_samples.append(entry)
	if _samples.size() > MAX_SAMPLES:
		_samples.pop_front()


## Série de um campo escalar do histórico.
func series(field: String) -> PackedFloat32Array:
	var values := PackedFloat32Array()
	for entry in _samples:
		values.append(float(entry.get(field, 0.0)))
	return values


## Série de um campo vetorial (preços, estoques, produção, comércio).
func product_series(field: String, product: int) -> PackedFloat32Array:
	var values := PackedFloat32Array()
	for entry in _samples:
		var row: PackedFloat32Array = entry.get(field, PackedFloat32Array())
		values.append(row[product] if product < row.size() else 0.0)
	return values


func _build_entry(day: int, city: Dictionary) -> Dictionary:
	var pop: Dictionary = city.get("pop", {})
	var labor_force := maxf(1.0, float(pop.get("labor_force", 0.0)))

	var prices := PackedFloat32Array()
	var stocks := PackedFloat32Array()
	var production := PackedFloat32Array()
	var demand := PackedFloat32Array()
	var imported := PackedFloat32Array()
	var exported := PackedFloat32Array()

	for market in city.get("markets", []):
		prices.append(float(market.get("price", 0.0)))
		stocks.append(float(market.get("stock", 0.0)))
		production.append(float(market.get("supply", 0.0)))
		demand.append(float(market.get("desired", 0.0)))
	for value in city.get("imported", []):
		imported.append(float(value))
	for value in city.get("exported", []):
		exported.append(float(value))

	var import_value := _sum(city.get("import_value", []))
	var export_value := _sum(city.get("export_value", []))

	return {
		"day": day,
		"population": float(city.get("population", 0.0)),
		"gdp": float(city.get("gdp", 0.0)),
		"gdp_per_capita": float(city.get("gdp_per_capita", 0.0)),
		"disposable_per_capita": float(city.get("disposable_per_capita", 0.0)),
		"living_standard": float(city.get("living_standard", 0.0)),
		"unemployment": float(pop.get("unemployed", 0.0)) / labor_force,
		"satisfaction": float(pop.get("satisfaction", 0.0)),
		"import_value": import_value,
		"export_value": export_value,
		"balance": export_value - import_value,
		"prices": prices,
		"stocks": stocks,
		"production": production,
		"demand": demand,
		"imported": imported,
		"exported": exported,
		# Campos do modelo legado (trigo + agregados por cidade).
		"wheat_production": float(city.get("production", 0.0)),
		"wheat_consumed": float(city.get("consumed", 0.0)),
		"wheat_stock": float(city.get("stockpile", 0.0)),
		"wheat_need": float(city.get("need", 0.0)),
		"deficit": float(city.get("deficit", 0.0)),
		"in_transit": float(city.get("in_transit", 0.0)),
		"jobs": float(city.get("jobs", 0.0)),
		"income": float(city.get("income", 0.0)),
		"industrialization": float(city.get("industrialization", 0.0)),
	}


static func _sum(values: Array) -> float:
	var total := 0.0
	for value in values:
		total += float(value)
	return total
