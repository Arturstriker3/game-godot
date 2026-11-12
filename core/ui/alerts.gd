extends RefCounted
class_name EconomyAlerts

## Alertas econômicos derivados do resumo real da simulação.
##
## Cada cidade gera no máximo um alerta, priorizando déficit; o conjunto é
## ordenado por gravidade e limitado a MAX_ALERTS para não virar ruído.
##
## Seleção limitada: com 858 cidades o cenário real gera centenas de alertas por
## dia. Antes `derive` criava um Dictionary para cada um, ordenava todos e
## descartava o excedente — ~6 ms no frame da virada. Agora a comparação é
## escalar e só os poucos que entram no topo pagam a criação do dicionário.

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

		var kind := ""
		var tone := ""
		var amount := 0.0
		if deficit > 0.0:
			kind = "deficit"
			tone = "negative" if coverage < LOW_COVERAGE_DAYS else "warning"
			amount = deficit
		elif coverage < LOW_COVERAGE_DAYS:
			kind = "low_stock"
			tone = "warning"
			amount = stock
		elif float(city.get("production", 0.0)) <= 0.0:
			kind = "no_production"
			tone = "warning"
			amount = 0.0
		else:
			continue

		var city_id := String(city.get("id", ""))
		# Fora do topo: não aloca dicionário nem entra na lista.
		if alerts.size() >= MAX_ALERTS and not _precedes_scalar(
				tone == "negative", amount, city_id, alerts[alerts.size() - 1]):
			continue
		var entry := {
			"city_id": city_id,
			"kind": kind,
			"tone": tone,
			"amount": amount,
			"coverage": coverage,
		}
		var slot := alerts.size()
		while slot > 0 and _precedes(entry, alerts[slot - 1]):
			slot -= 1
		alerts.insert(slot, entry)
		if alerts.size() > MAX_ALERTS:
			alerts.pop_back()
	return alerts


## Ordem de gravidade: `negative` antes de `warning`, depois valor decrescente.
## Empate: id da cidade (ordem estável e determinística entre dias).
static func _precedes(left: Dictionary, right: Dictionary) -> bool:
	return _precedes_scalar(String(left["tone"]) == "negative",
			float(left["amount"]), String(left["city_id"]), right)


static func _precedes_scalar(negative: bool, amount: float, city_id: String,
		right: Dictionary) -> bool:
	var right_negative := String(right["tone"]) == "negative"
	if negative != right_negative:
		return negative
	var right_amount := float(right["amount"])
	if amount != right_amount:
		return amount > right_amount
	return city_id < String(right["city_id"])
