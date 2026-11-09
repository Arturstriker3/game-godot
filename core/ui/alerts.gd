extends RefCounted
class_name EconomyAlerts

## Alertas econômicos derivados do resumo real da simulação.
##
## Cada cidade gera no máximo um alerta, priorizando déficit; o conjunto é
## ordenado por gravidade e limitado a MAX_ALERTS para não virar ruído.

const MAX_ALERTS := 6
const LOW_COVERAGE_DAYS := 2.0


## @param summary Resumo publicado por `Economy`.
## @returns Lista de {city_id, kind, tone, amount, coverage}.
static func derive(summary: Dictionary) -> Array:
	var alerts: Array = []

	for city in summary.get("cities", []):
		var need := float(city.get("need", 0.0))
		var stock := float(city.get("stockpile", 0.0))
		var deficit := float(city.get("deficit", 0.0))
		var coverage := INF if need <= 1e-9 else stock / need

		if deficit > 0.0:
			alerts.append({
				"city_id": String(city.get("id", "")),
				"kind": "deficit",
				"tone": "negative" if coverage < LOW_COVERAGE_DAYS else "warning",
				"amount": deficit,
				"coverage": coverage,
			})
		elif coverage < LOW_COVERAGE_DAYS:
			alerts.append({
				"city_id": String(city.get("id", "")),
				"kind": "low_stock",
				"tone": "warning",
				"amount": stock,
				"coverage": coverage,
			})
		elif float(city.get("production", 0.0)) <= 0.0:
			alerts.append({
				"city_id": String(city.get("id", "")),
				"kind": "no_production",
				"tone": "warning",
				"amount": 0.0,
				"coverage": coverage,
			})

	alerts.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		if String(a["tone"]) != String(b["tone"]):
			return String(a["tone"]) == "negative"
		return float(a["amount"]) > float(b["amount"]))

	return alerts.slice(0, MAX_ALERTS)
