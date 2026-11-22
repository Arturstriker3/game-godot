extends RefCounted
class_name PowerService

## PARTE E / D-116 — ENERGIA ELÉTRICA: ponte entre o DADO público e o núcleo.
##
## Declara no núcleo, ANTES do dia 1 (mesmo contrato dos setters da geografia,
## agricultura e armazenagem):
##
##   * ACESSO À ELETRICIDADE e PERDAS T&D por país (World Bank WDI, via
##     `access_losses.json`, CC BY 4.0): acesso total/urbano/rural e
##     `tnd_losses_pct`. É o dado que decide se a nação tem OPERADOR NACIONAL
##     (regra declarada `operator_access_pct`) e a perda base de cada cidade.
##   * PARQUE INSTALADO por cidade e fonte (GPPD 2021 escalonado à matriz
##     Ember 2024–2025, D-115(b)): `power_plants_2026_scaled.json`.
##   * ÂNCORA NACIONAL do ano-base (D-116): demanda Ember por nação (MWh/dia) e
##     geração real por fonte (MWh/ano) de `national_demand.json`. A demanda
##     modelada passa a ser base × (renda_t/renda_0)^e (só relativa ao ano-base)
##     e o fator de capacidade por nação × fonte é calibrado para reproduzir a
##     geração real. Nação sem Ember fica na curva global declarada.
##   * POTENCIAL RENOVÁVEL: hídrico (`hydro_potential.json`, potencial técnico
##     rateado; `null` = sem âncora nacional, sem expansão) e solar/eólico
##     (`renewable_potential.json`, Global Solar/Wind Atlas, CC BY 4.0). O
##     PVOUT MENSAL do GSA vira o fator de capacidade solar de cada mês do jogo.
##   * REGRA (`power_params.json`): espelho declarado de `sim/power.h`.
##
## O que NÃO é dado: os tetos de expansão solar/eólico. O atlas publica
## irradiância/vento, não MW permitidos, então a conversão área→MW é MODELO
## DECLARADO no próprio `power_params.json` (`model.solar_mw_per_km2`,
## `wind_mw_per_km2`) e aparece no relatório. Sem dado de potencial, o teto é o
## próprio parque: a cidade não inventa local novo.
##
## Cidade sem nenhuma linha nos arquivos fica `has_data = false` no núcleo: a
## ausência é declarada e NÃO vira blecaute (nada de racionamento fabricado).

const ACCESS_PATH := "res://data/definitions/energy/access_losses.json"
const PLANTS_PATH := "res://data/definitions/energy/power_plants_2026_scaled.json"
const RENEWABLE_PATH := "res://data/definitions/energy/renewable_potential.json"
const HYDRO_PATH := "res://data/definitions/energy/hydro_potential.json"
const PARAMS_PATH := "res://data/definitions/energy/power_params.json"
const DEMAND_PATH := "res://data/definitions/energy/national_demand.json"
const INTERCONNECTS_PATH := "res://data/definitions/energy/interconnects.json"

const HOURS_PER_DAY := 24.0
const DAYS_PER_YEAR := 365.0

var _access: Dictionary = {}
var _plants: Dictionary = {}
var _renewable: Dictionary = {}
var _hydro: Dictionary = {}
var _params: Dictionary = {}
var _demand: Dictionary = {}
var _interconnects: Array = []
var _loaded := false


func load_data() -> bool:
	if _loaded:
		return true
	_access = _read_dict(ACCESS_PATH).get("by_country", {})
	_plants = _read_dict(PLANTS_PATH).get("by_city", {})
	_renewable = _read_dict(RENEWABLE_PATH).get("by_city", {})
	_hydro = _read_dict(HYDRO_PATH).get("by_city", {})
	_params = _read_dict(PARAMS_PATH)
	_demand = _read_dict(DEMAND_PATH).get("by_nation", {})
	var links: Variant = _read_dict(INTERCONNECTS_PATH).get("links", [])
	_interconnects = links if typeof(links) == TYPE_ARRAY else []
	_loaded = true
	return not _access.is_empty() or not _plants.is_empty()


func _read_dict(path: String) -> Dictionary:
	if not FileAccess.file_exists(path):
		return {}
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {}
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_DICTIONARY else {}


## Modelo declarado de teto de expansão (MW por km²). Sem o valor no arquivo,
## 0: cidade sem teto declarado não expande por essa fonte.
func _model_value(key: String) -> float:
	return float(_params.get("model", {}).get(key, 0.0))


## Aplica o dado no núcleo. `cities` é a lista do cenário (id + nation).
## `only_missing`: save v35 manda — `set_power_definitions` não toca no estado
## que veio do save; save v34 e anterior recebe o parque real aqui.
func apply_to_sim(sim: Object, cities: Array, options: Dictionary = {}) -> Dictionary:
	if sim == null or not sim.has_method("set_power_definitions"):
		return {"applied": 0, "reason": "core_without_power"}
	load_data()
	var only_missing := bool(options.get("only_missing", true))

	# --- Nações: acesso e perdas T&D (dado real) -----------------------------
	var nations: Dictionary = {}
	var anchor_count := 0
	var generation_count := 0
	for iso3: Variant in _access:
		var row: Dictionary = _access[iso3]
		var tag := _text(row.get("nation"))
		if tag.is_empty() or not bool(row.get("playable", true)):
			continue
		var access := _indicator(row, "electricity_access_pct")
		if access < 0.0:
			continue
		var urban := _indicator(row, "electricity_access_urban_pct")
		var rural := _indicator(row, "electricity_access_rural_pct")
		var loss := _indicator(row, "tnd_losses_pct")
		var def := {
			"access_pct": access,
			"access_urban_pct": access if urban < 0.0 else urban,
			"access_rural_pct": access if rural < 0.0 else rural,
			"loss_pct": 10.0 if loss < 0.0 else loss,
		}
		# Ancoragem por nação (D-116): demanda Ember do ano-base e geração real
		# por fonte. Sem Ember, o núcleo usa a curva/fatores globais declarados.
		var anchor := _nation_anchor(tag)
		if anchor.has("demand_base_mwh_day"):
			def["demand_base_mwh_day"] = anchor["demand_base_mwh_day"]
			anchor_count += 1
		if anchor.has("generation_mwh_year"):
			def["generation_mwh_year"] = anchor["generation_mwh_year"]
			generation_count += 1
		nations[tag] = def

	# --- Cidades: parque, acesso local e potencial ---------------------------
	var city_defs: Dictionary = {}
	var with_park := 0
	var total_mw := 0.0
	var park_by_fuel: Dictionary = {}
	var cap_by_fuel: Dictionary = {}
	var solar_source := 0
	for entry: Variant in cities:
		var city: Dictionary = entry
		var city_id := String(city.get("id", ""))
		var tag := String(city.get("nation", ""))
		if city_id.is_empty() or tag.is_empty():
			continue
		var park := _city_park(city_id)
		var caps := _city_caps(city_id, park)
		var renewable: Dictionary = _renewable.get(city_id, {})
		var solar_month: Array = _solar_month_cf(renewable)
		if not solar_month.is_empty():
			solar_source += 1
		# Acesso: o dado de país vale para a cidade (não há acesso por cidade na
		# fonte pública); a perda é a do país, ajustada pela infra B5 no núcleo.
		# Cidade cujo país não tem acesso declarado fica SEM dado.
		if not nations.has(tag):
			continue
		var nation_row: Dictionary = nations[tag]
		var def := {
			"access_pct": float(nation_row["access_pct"]),
			"loss_pct": float(nation_row["loss_pct"]),
			"park": park,
			"cap": caps,
			"solar_month": solar_month,
			"wind_ms": _wind_speed(renewable),
		}
		city_defs[city_id] = def
		var park_total := 0.0
		for fuel: Variant in park:
			park_total += float(park[fuel])
			park_by_fuel[fuel] = float(park_by_fuel.get(fuel, 0.0)) + float(park[fuel])
		for fuel: Variant in caps:
			cap_by_fuel[fuel] = float(cap_by_fuel.get(fuel, 0.0)) + float(caps[fuel])
		total_mw += park_total
		if park_total > 0.0:
			with_park += 1

	var applied := int(sim.set_power_definitions(nations, city_defs,
		_params.get("params", {}), only_missing))
	# E5 (D-117): interconexões do ano-base (dado derivado). `only_missing`
	# preserva as linhas de um save v36; save v35 chega sem elas e recebe o
	# ano-base aqui (a ausência anterior era declarada).
	var links_applied := 0
	if sim.has_method("set_power_interconnects"):
		links_applied = int(sim.set_power_interconnects(_interconnects,
			only_missing))
	var report := {
		"applied": applied,
		"cities_in_scenario": cities.size(),
		"cities_with_def": city_defs.size(),
		"cities_with_park": with_park,
		"nations": nations.size(),
		"nations_with_demand_anchor": anchor_count,
		"nations_with_generation_calibration": generation_count,
		"park_mw": total_mw,
		"park_mw_by_fuel": park_by_fuel,
		"cap_mw_by_fuel": cap_by_fuel,
		"cities_with_solar_month": solar_source,
		"only_missing": only_missing,
		"interconnect_links_data": _interconnects.size(),
		"interconnect_links_applied": links_applied,
	}
	if applied == 0:
		report["reason"] = "save_has_power" if not only_missing else "no_city_def"
	# Relatório com o estado que o núcleo publicou (executa só com dado).
	if sim.has_method("get_power_status"):
		var status: Dictionary = sim.get_power_status()
		if bool(status.get("ready", false)):
			report["operator_nations"] = _operator_nations(status)
	return report


## Valor do indicador do World Bank; -1 quando ausente (nunca 0 inventado).
func _indicator(row: Dictionary, key: String) -> float:
	var raw: Variant = row.get(key)
	if typeof(raw) != TYPE_DICTIONARY:
		return -1.0
	var value: Variant = raw.get("value")
	if typeof(value) != TYPE_FLOAT and typeof(value) != TYPE_INT:
		return -1.0
	return float(value)


## Âncora nacional do ano-base (Ember): demanda em MWh/dia e geração real por
## fonte em MWh/ano. Nação sem linha fica fora do dicionário (curva global).
func _nation_anchor(tag: String) -> Dictionary:
	var row: Dictionary = _demand.get(tag, {})
	var out: Dictionary = {}
	if row.is_empty():
		return out
	var demand: Variant = row.get("demand_twh")
	if typeof(demand) == TYPE_FLOAT or typeof(demand) == TYPE_INT:
		var base := maxf(0.0, float(demand)) * 1000000.0 / DAYS_PER_YEAR
		if base > 0.0:
			out["demand_base_mwh_day"] = base
	var by_source: Variant = row.get("generation_twh")
	if typeof(by_source) == TYPE_DICTIONARY:
		var generation: Dictionary = {}
		var sources: Dictionary = by_source
		for fuel: Variant in sources:
			var value: Variant = sources[fuel]
			if typeof(value) != TYPE_FLOAT and typeof(value) != TYPE_INT:
				continue
			generation[String(fuel)] = maxf(0.0, float(value)) * 1000000.0
		if not generation.is_empty():
			out["generation_mwh_year"] = generation
	return out


## Texto tolerante a `null` do JSON (nunca converte null em "<null>").
static func _text(value: Variant) -> String:
	if value == null:
		return ""
	return String(value)


## Parque instalado da cidade por fonte (MW) — dado GPPD escalonado à Ember.
func _city_park(city_id: String) -> Dictionary:
	var row: Dictionary = _plants.get(city_id, {})
	var by_fuel: Variant = row.get("by_fuel")
	if typeof(by_fuel) != TYPE_DICTIONARY:
		return {}
	var park: Dictionary = {}
	for fuel: Variant in by_fuel:
		var mw := float(by_fuel[fuel])
		if mw > 0.0:
			park[String(fuel)] = mw
	return park


## Teto (potencial) por fonte em MW. Hídrico = potencial técnico rateado do dado;
## solar/eólico = área da cidade × conversão declarada; fonte sem dado fica de
## fora (o núcleo usa o próprio parque como teto — não se inventa local).
func _city_caps(city_id: String, park: Dictionary) -> Dictionary:
	var caps: Dictionary = {}
	var hydro: Dictionary = _hydro.get(city_id, {})
	var hydro_mw: Variant = hydro.get("potential_mw")
	if typeof(hydro_mw) == TYPE_FLOAT or typeof(hydro_mw) == TYPE_INT:
		if float(hydro_mw) > 0.0:
			caps["hydro"] = maxf(float(hydro_mw), float(park.get("hydro", 0.0)))
	var renewable: Dictionary = _renewable.get(city_id, {})
	var area := float(renewable.get("area_km2", 0.0))
	if area > 0.0:
		var solar := area * _model_value("solar_mw_per_km2")
		if solar > 0.0:
			caps["solar"] = maxf(solar, float(park.get("solar", 0.0)))
		var wind := area * _model_value("wind_mw_per_km2")
		if wind > 0.0:
			caps["wind"] = maxf(wind, float(park.get("wind", 0.0)))
	return caps


## Fator de capacidade solar por mês (0..1) a partir do PVOUT diário do GSA:
## kWh/kWp/dia ÷ 24 h. Sem a série mensal, [] e o núcleo usa `cf_solar`.
func _solar_month_cf(renewable: Dictionary) -> Array:
	var solar: Variant = renewable.get("solar")
	if typeof(solar) != TYPE_DICTIONARY:
		return []
	var monthly: Variant = solar.get("monthly_pvout_kwh_kwp_day")
	if typeof(monthly) != TYPE_ARRAY:
		return []
	var values: Array = monthly
	if values.size() != 12:
		return []
	var result: Array = []
	for value: Variant in values:
		result.append(clampf(float(value) / HOURS_PER_DAY, 0.0, 1.0))
	return result


## Vento médio anual do sítio (m/s a 100 m, Global Wind Atlas). Sem a amostra,
## 0: a usina nova usa o CF declarado e não inventa potencial local (D-122).
func _wind_speed(renewable: Dictionary) -> float:
	var wind: Variant = renewable.get("wind")
	if typeof(wind) != TYPE_DICTIONARY:
		return 0.0
	var value: Variant = (wind as Dictionary).get("m_s_mean")
	if typeof(value) != TYPE_FLOAT and typeof(value) != TYPE_INT:
		return 0.0
	return maxf(0.0, float(value))


func _operator_nations(status: Dictionary) -> Array:
	var tags: Array = []
	var nations: Dictionary = status.get("nations", {})
	for tag: Variant in nations:
		if bool((nations[tag] as Dictionary).get("exists", false)):
			tags.append(String(tag))
	tags.sort()
	return tags
