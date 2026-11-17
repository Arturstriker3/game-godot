extends RefCounted
class_name HistoryService

## Histórico de 12 meses amostrado do status publicado pela economia.
##
## Regra de amostragem (uma vez por dia simulado, no fechamento do dia):
## - 1 ponto por dia, mantendo os últimos `DAILY_LIMIT` (30) dias;
## - 1 ponto por mês, mantendo os últimos `MONTH_LIMIT` (12) meses — o valor do
##   mês é o do último dia amostrado nele (downsampling, sem interpolação).
##
## Por nação: população, PIB, comida (produção ÷ necessidade) e reservas
## minerais conhecidas (kt). Por cidade: população, comida (dias de cobertura do
## estoque) e renda (padrão de vida 0..1 publicado pelo núcleo).
##
## Nada é inventado: valores ausentes ficam NAN (buraco honesto) e as séries
## voltam vazias quando a entidade não foi amostrada. O serviço não conhece o
## HUD nem regras econômicas; só agrega o dicionário já publicado e guarda arrays
## compactos. `serialize`/`deserialize` existem para o save futuro.

const DAILY_LIMIT := 30
const MONTH_LIMIT := 12
const VERSION := 1

const NATION_FIELDS: Array[String] = ["population", "gdp", "food", "reserves"]
const CITY_FIELDS: Array[String] = ["population", "food", "income"]

var _last_day := -1
var _months: Array[String] = []
var _nation: Dictionary = {}
var _city: Dictionary = {}
var _month_nation: Dictionary = {}
var _month_city: Dictionary = {}


func clear() -> void:
	_last_day = -1
	_months.clear()
	_nation.clear()
	_city.clear()
	_month_nation.clear()
	_month_city.clear()


## Registra o dia. `month_key` vem do calendário do jogo ("2026-01") e define o
## balde mensal; o mesmo dia duas vezes não duplica a amostra.
##
## A4: `source` são as COLUNAS por cidade (`Economy.city_columns()`), lidas
## direto do estado do núcleo. Um resumo antigo com `cities` (fixtures/testes)
## continua aceito e produz os mesmos agregados.
func sample(day: int, source: Dictionary, month_key: String) -> void:
	if day == _last_day:
		return
	_last_day = day
	var columns := source.has("id")
	var nations := _nation_values_columns(source) if columns \
			else _nation_values(source)
	var cities := _city_values_columns(source) if columns \
			else _city_values(source)
	_push_daily(day, nations, cities)
	if not month_key.is_empty():
		_push_monthly(month_key, nations, cities)


# --- Leituras ---------------------------------------------------------------

func month_count() -> int:
	return _months.size()


func month_labels() -> PackedStringArray:
	var out := PackedStringArray()
	out.append_array(_months)
	return out


func nation_daily(tag: String, field: String) -> PackedFloat32Array:
	return _series(_nation, tag, field)


func city_daily(city_id: String, field: String) -> PackedFloat32Array:
	return _series(_city, city_id, field)


func nation_monthly(tag: String, field: String) -> PackedFloat32Array:
	return _month_series(_month_nation, tag, field)


func city_monthly(city_id: String, field: String) -> PackedFloat32Array:
	return _month_series(_month_city, city_id, field)


func days_for_nation(tag: String) -> PackedInt32Array:
	var row: Dictionary = _nation.get(tag, {})
	return (row.get("days", PackedInt32Array()) as PackedInt32Array).duplicate()


func days_for_city(city_id: String) -> PackedInt32Array:
	var row: Dictionary = _city.get(city_id, {})
	return (row.get("days", PackedInt32Array()) as PackedInt32Array).duplicate()


func nation_daily_count(tag: String) -> int:
	var row: Dictionary = _nation.get(tag, {})
	return (row.get("days", PackedInt32Array()) as PackedInt32Array).size()


func city_daily_count(city_id: String) -> int:
	var row: Dictionary = _city.get(city_id, {})
	return (row.get("days", PackedInt32Array()) as PackedInt32Array).size()


# --- Agregação --------------------------------------------------------------

func _nation_values(summary: Dictionary) -> Dictionary:
	var light: Dictionary = summary.get("light", {})
	var cities: Array = summary.get("cities", [])
	var totals := {}
	for index in cities.size():
		var city: Dictionary = cities[index]
		var tag := String(city.get("nation", ""))
		if tag.is_empty():
			continue
		var entry: Dictionary = totals.get(tag, {
			"population": 0.0, "gdp": 0.0, "production": 0.0, "need": 0.0,
			"reserves": 0.0,
		})
		entry["population"] = float(entry["population"]) \
				+ float(city.get("population", 0.0))
		entry["gdp"] = float(entry["gdp"]) + float(city.get("gdp", 0.0))
		entry["production"] = float(entry["production"]) \
				+ float(city.get("production", 0.0))
		entry["need"] = float(entry["need"]) + float(city.get("need", 0.0))
		entry["reserves"] = float(entry["reserves"]) \
				+ _city_reserves(city, light, index)
		totals[tag] = entry

	var values := {}
	for tag in totals:
		var entry: Dictionary = totals[tag]
		var need := float(entry["need"])
		values[tag] = {
			"population": float(entry["population"]),
			"gdp": float(entry["gdp"]),
			"food": float(entry["production"]) / need if need > 0.0 else NAN,
			"reserves": float(entry["reserves"]),
		}
	return values


## Reservas de uma cidade. O status diário publica só o bloco leve, com as
## jazidas em tabela CSR indexada por cidade (`d_offset`/`d_remaining_kt`);
## sem `light` (binário antigo ou fixture), cai no dicionário pesado da cidade.
static func _city_reserves(city: Dictionary, light: Dictionary,
		index: int) -> float:
	var offset: PackedInt32Array = light.get("d_offset", PackedInt32Array())
	if index >= 0 and index + 1 < offset.size():
		var remaining: PackedFloat64Array = light.get("d_remaining_kt",
				PackedFloat64Array())
		var total := 0.0
		for at in range(offset[index], mini(offset[index + 1], remaining.size())):
			total += remaining[at]
		return total
	var deposits: Dictionary = city.get("deposits", {})
	var total := 0.0
	for resource in deposits:
		total += float(deposits[resource].get("remaining_kt", 0.0))
	return total


func _city_values(summary: Dictionary) -> Dictionary:
	var values := {}
	for city in summary.get("cities", []):
		var id := String(city.get("id", ""))
		if id.is_empty():
			continue
		var need := float(city.get("need", 0.0))
		var stock := float(city.get("stockpile", 0.0))
		values[id] = {
			"population": float(city.get("population", 0.0)),
			"food": stock / need if need > 0.0 else NAN,
			"income": float(city.get("income", 0.0)),
		}
	return values


## A4: agregado nacional a partir das colunas por cidade (mesma soma, mesma
## ordem das cidades do laço antigo sobre `cities[]`).
func _nation_values_columns(columns: Dictionary) -> Dictionary:
	var ids: PackedStringArray = columns.get("id", PackedStringArray())
	var nations: PackedStringArray = columns.get("nation", PackedStringArray())
	var population: PackedFloat64Array = columns.get("population",
			PackedFloat64Array())
	var gdp: PackedFloat64Array = columns.get("gdp", PackedFloat64Array())
	var production: PackedFloat64Array = columns.get("production",
			PackedFloat64Array())
	var need: PackedFloat64Array = columns.get("need", PackedFloat64Array())
	var reserves: PackedFloat64Array = columns.get("deposit_remaining_kt",
			PackedFloat64Array())
	var totals := {}
	for index in ids.size():
		var tag := nations[index] if index < nations.size() else ""
		if tag.is_empty():
			continue
		var entry: Dictionary = totals.get(tag, {
			"population": 0.0, "gdp": 0.0, "production": 0.0, "need": 0.0,
			"reserves": 0.0,
		})
		entry["population"] = float(entry["population"]) \
				+ (population[index] if index < population.size() else 0.0)
		entry["gdp"] = float(entry["gdp"]) \
				+ (gdp[index] if index < gdp.size() else 0.0)
		entry["production"] = float(entry["production"]) \
				+ (production[index] if index < production.size() else 0.0)
		entry["need"] = float(entry["need"]) \
				+ (need[index] if index < need.size() else 0.0)
		entry["reserves"] = float(entry["reserves"]) \
				+ (reserves[index] if index < reserves.size() else 0.0)
		totals[tag] = entry

	var values := {}
	for tag in totals:
		var entry: Dictionary = totals[tag]
		var tag_need := float(entry["need"])
		values[tag] = {
			"population": float(entry["population"]),
			"gdp": float(entry["gdp"]),
			"food": float(entry["production"]) / tag_need if tag_need > 0.0 else NAN,
			"reserves": float(entry["reserves"]),
		}
	return values


## A4: valores por cidade a partir das colunas (id/population/stockpile/need/income).
func _city_values_columns(columns: Dictionary) -> Dictionary:
	var ids: PackedStringArray = columns.get("id", PackedStringArray())
	var population: PackedFloat64Array = columns.get("population",
			PackedFloat64Array())
	var stockpile: PackedFloat64Array = columns.get("stockpile",
			PackedFloat64Array())
	var need: PackedFloat64Array = columns.get("need", PackedFloat64Array())
	var income: PackedFloat64Array = columns.get("income", PackedFloat64Array())
	var values := {}
	for index in ids.size():
		var id := ids[index]
		if id.is_empty():
			continue
		var city_need := need[index] if index < need.size() else 0.0
		var stock := stockpile[index] if index < stockpile.size() else 0.0
		values[id] = {
			"population": population[index] if index < population.size() else 0.0,
			"food": stock / city_need if city_need > 0.0 else NAN,
			"income": income[index] if index < income.size() else 0.0,
		}
	return values


# --- Amostragem -------------------------------------------------------------

func _push_daily(day: int, nations: Dictionary, cities: Dictionary) -> void:
	for tag in nations:
		_append(_row(_nation, tag, NATION_FIELDS), day, nations[tag], NATION_FIELDS)
	for id in cities:
		_append(_row(_city, id, CITY_FIELDS), day, cities[id], CITY_FIELDS)


func _push_monthly(month_key: String, nations: Dictionary, cities: Dictionary) -> void:
	if not _month_nation.has(month_key):
		_months.append(month_key)
		while _months.size() > MONTH_LIMIT:
			var old: String = _months[0]
			_months.remove_at(0)
			_month_nation.erase(old)
			_month_city.erase(old)

	var month_nations: Dictionary = _month_nation.get(month_key, {})
	for tag in nations:
		month_nations[tag] = (nations[tag] as Dictionary).duplicate()
	_month_nation[month_key] = month_nations

	var month_cities: Dictionary = _month_city.get(month_key, {})
	for id in cities:
		month_cities[id] = (cities[id] as Dictionary).duplicate()
	_month_city[month_key] = month_cities


func _row(store: Dictionary, key: String, fields: Array[String]) -> Dictionary:
	if store.has(key):
		return store[key]
	var row := {"days": PackedInt32Array()}
	for field in fields:
		row[field] = PackedFloat32Array()
	store[key] = row
	return row


func _append(row: Dictionary, day: int, values: Dictionary,
		fields: Array[String]) -> void:
	var days: PackedInt32Array = row["days"]
	days.append(day)
	row["days"] = days
	for field in fields:
		var series: PackedFloat32Array = row[field]
		series.append(float(values.get(field, NAN)))
		row[field] = series
	if days.size() > DAILY_LIMIT:
		days.remove_at(0)
		row["days"] = days
		for field in fields:
			var trimmed: PackedFloat32Array = row[field]
			trimmed.remove_at(0)
			row[field] = trimmed


func _series(store: Dictionary, key: String, field: String) -> PackedFloat32Array:
	var row: Dictionary = store.get(key, {})
	if not row.has(field):
		return PackedFloat32Array()
	return (row[field] as PackedFloat32Array).duplicate()


func _month_series(store: Dictionary, key: String, field: String) -> PackedFloat32Array:
	var out := PackedFloat32Array()
	for month in _months:
		var bucket: Dictionary = store.get(month, {})
		var entry: Dictionary = bucket.get(key, {})
		out.append(float(entry.get(field, NAN)))
	return out


# --- Persistência -----------------------------------------------------------

## Formato pronto para o save: arrays simples (JSON-friendly), sem Packed.
func serialize() -> Dictionary:
	return {
		"version": VERSION,
		"last_day": _last_day,
		"months": Array(_months),
		"nation": _dump(_nation, NATION_FIELDS),
		"city": _dump(_city, CITY_FIELDS),
		"month_nation": _month_nation.duplicate(true),
		"month_city": _month_city.duplicate(true),
	}


func deserialize(data: Dictionary) -> bool:
	if data.is_empty():
		return false
	clear()
	_last_day = int(data.get("last_day", -1))
	_months.assign(data.get("months", []))
	_load(_nation, data.get("nation", {}), NATION_FIELDS)
	_load(_city, data.get("city", {}), CITY_FIELDS)
	_month_nation = (data.get("month_nation", {}) as Dictionary).duplicate(true)
	_month_city = (data.get("month_city", {}) as Dictionary).duplicate(true)
	return true


func _dump(store: Dictionary, fields: Array[String]) -> Dictionary:
	var out := {}
	for key in store:
		var row: Dictionary = store[key]
		var entry := {"days": Array(row["days"])}
		for field in fields:
			var values: Array = []
			for value in row[field]:
				values.append(null if is_nan(float(value)) else float(value))
			entry[field] = values
		out[key] = entry
	return out


func _load(store: Dictionary, data: Variant, fields: Array[String]) -> void:
	store.clear()
	if typeof(data) != TYPE_DICTIONARY:
		return
	for key in data:
		var source: Dictionary = data[key]
		var row := {"days": _ints(source.get("days", []))}
		for field in fields:
			row[field] = _floats(source.get(field, []))
		store[String(key)] = row


static func _ints(values: Variant) -> PackedInt32Array:
	var out := PackedInt32Array()
	if values is Array:
		for value in values:
			out.append(int(value))
	return out


static func _floats(values: Variant) -> PackedFloat32Array:
	var out := PackedFloat32Array()
	if values is Array:
		for value in values:
			out.append(NAN if value == null else float(value))
	return out
