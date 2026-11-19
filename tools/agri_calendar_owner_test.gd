extends SceneTree

## G — CALENDÁRIO DE SAFRA HERDADO DA REGIÃO (decisão do dono, D-098).
##
##   godot --headless --path . --script res://tools/agri_calendar_owner_test.gd
##
## Regra: o calendário de plantio/colheita fica GRAVADO NA REGIÃO no início do
## jogo, herdado da nação ORIGINAL (a do `cities.json`), e a produção usa esse
## calendário para sempre. **Trocar o dono da região não muda a época da
## safra.**
##
## O teste prova, nesta ordem:
##   (a) o plano da cidade-alvo nasce com o calendário da nação original
##       (BRA soja: colheita mar–mai);
##   (b) a derivação ingênua (dono novo, USA) daria outra janela — o dado
##       difere, então a guarda tem o que proteger;
##   (c) conquista simulada no SAVE (a cidade muda de `nation` e o snapshot é
##       recarregado): a janela da safra continua a original;
##   (d) reaplicar o service no load com `calendar_nations` + `only_missing`
##       não redeclara nada (o plano veio em `definitions`);
##   (e) o caminho SEM a guarda (reaplicar com o dono novo) TROCARIA a janela
##       — é a regressão que a decisão do dono fecha;
##   (f) 200 dias no mundo conquistado: a soja do dono novo só entra em
##       colheita nas janelas de BRA/USA, e entra na janela brasileira (mar–mai).
##
## Sem IA: mede o núcleo (o calendário não depende de decisão de agente).

const GeoScript := preload("res://core/economy/geography_service.gd")
const AgriScript := preload("res://core/economy/agriculture_service.gd")
const RegionDataScript := preload("res://core/economy/region_data.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"
const INFORMALITY_PATH := "res://data/definitions/economy/informality.json"
const HEALTH_ACCESS_PATH := "res://data/definitions/population/health_access.json"
const CROP_CALENDAR_PATH := "res://data/definitions/agriculture/crop_calendar.json"

## Cidade-alvo: uma cidade brasileira com plano de soja.
const ORIGIN_NATION := "BRA"
const CROP := "soy"
## Dono novo (conquista): os EUA, que têm janela de soja bem diferente.
const NEW_OWNER := "USA"
## Janela brasileira de colheita da soja (dado: mar–mai) e a americana.
const BRA_MONTHS := [3, 4, 5]
const DAYS := 200

var _failures: Array[String] = []
var _cities: Array = []
var _geo: Object = null
var _original: Dictionary = {}   # {city_id: nation} do início do mundo
var _farms: Dictionary = {}      # {city_id: {product, capacity_t_day}} (D-099)


func _init() -> void:
	var world := _build_world()
	if world.is_empty():
		_finish()
		return
	var sim: Object = world["sim"]
	_original = world["calendar_nations"]
	var target := _pick_target(sim)
	if target.is_empty():
		_fail("não achei cidade %s com plano de %s" % [ORIGIN_NATION, CROP])
		_finish()
		return
	_test_original_calendar(sim, target)
	var snapshot: Dictionary = sim.to_snapshot()
	var naive_differs := _test_naive_differs()
	_test_conquest_keeps_window(snapshot, target)
	_test_guarded_reapply(snapshot, target)
	_test_unguarded_reapply_changes(snapshot, target, naive_differs)
	_test_harvest_months(snapshot, target)
	_finish()


func _finish() -> void:
	if _failures.is_empty():
		print("[agri-owner] OK: calendário herdado da região; conquista não "
			+ "muda a janela da safra.")
		quit(0)
	else:
		for failure in _failures:
			print("[agri-owner] FALHA: " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


# ---------------------------------------------------------------------------
# Mundo
# ---------------------------------------------------------------------------

func _build_world() -> Dictionary:
	_cities = _load_array(CITIES_PATH)
	var goods := _load_array(GOODS_PATH)
	if _cities.is_empty() or goods.is_empty():
		_fail("cenário vazio (cidades=%d, bens=%d)" % [_cities.size(), goods.size()])
		return {}
	RegionDataScript.attach(_cities)
	var sim = EconomySim.new()
	sim.set_goods(goods)
	sim.set_recipes(_load_array(RECIPES_PATH))
	sim.set_needs(_load_dict(NEEDS_PATH))
	sim.set_services(_load_dict(SERVICES_PATH))
	sim.set_currencies(_load_dict(CURRENCIES_PATH))
	sim.set_sector_employment(_load_dict(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_load_dict(AGE_STRUCTURE_PATH))
	_geo = GeoScript.new()
	_geo.load_data()
	var buildings := _load_array(BUILDINGS_PATH)
	buildings = _geo.resolve_initial_farms(buildings, _cities)
	buildings = _geo.resolve_initial_mines(buildings, _cities)
	sim.initialize_world(_cities, _load_array(PORTS_PATH), buildings)
	GeoScript.apply_initial_infra(sim)
	var centers: Array = []
	for city in _cities:
		centers.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
		})
	_geo.apply_to_sim(sim, centers)
	var agri := AgriScript.new()
	var original := agri.calendar_nations(_cities)
	_farms = AgriScript.farms_of(buildings)
	var report: Dictionary = agri.apply_to_sim(sim, _cities, _geo,
		{"calendar_nations": original, "farms": _farms})
	if int(report.get("plans", 0)) <= 0:
		_fail("nenhum plano declarado")
		return {}
	return {"sim": sim, "report": report, "calendar_nations": original}


## Primeira cidade de ORIGIN_NATION com plano de CROP nas definições.
func _pick_target(sim: Object) -> Dictionary:
	var snapshot: Dictionary = sim.to_snapshot()
	var definitions: Dictionary = snapshot.get("definitions", {})
	for plan in Array(definitions.get("crop_plans", [])):
		var row: Dictionary = plan
		if String(row.get("product", "")) != CROP:
			continue
		var city_id := String(row.get("city", ""))
		if not _original.has(city_id):
			continue
		if String(_original[city_id]) != ORIGIN_NATION:
			continue
		return {"city": city_id, "seasons": row.get("seasons", [])}
	return {}


func _test_original_calendar(sim: Object, target: Dictionary) -> void:
	var months := _harvest_months(target["seasons"])
	print("[agri-owner] alvo=%s (%s/%s) janela de colheita=%s" % [
		target["city"], ORIGIN_NATION, CROP, str(months)])
	if not _same_months(months, BRA_MONTHS):
		_fail("plano inicial não seguiu o calendário do %s: meses %s (esperado %s)"
			% [ORIGIN_NATION, str(months), str(BRA_MONTHS)])
	var report: Dictionary = sim.get_agriculture_status()
	if report.is_empty():
		_fail("get_agriculture_status vazio depois da declaração")


## O calendário do dono novo (USA) realmente difere do original? Se não
## diferisse, a guarda não teria o que provar.
func _test_naive_differs() -> bool:
	var calendars: Dictionary = _load_dict(CROP_CALENDAR_PATH)
	var bra := _calendar_months(calendars, ORIGIN_NATION, CROP)
	var usa := _calendar_months(calendars, NEW_OWNER, CROP)
	print("[agri-owner] calendário do dado: %s %s=%s | %s %s=%s" % [
		ORIGIN_NATION, CROP, str(bra), NEW_OWNER, CROP, str(usa)])
	if bra.is_empty() or usa.is_empty():
		_fail("calendário do dado faltando para %s|%s ou %s|%s"
			% [ORIGIN_NATION, CROP, NEW_OWNER, CROP])
		return false
	var differs := not _same_months(bra, usa)
	if not differs:
		_fail("BR e USA têm a MESMA janela de %s: o teste não distingue" % CROP)
	return differs


func _test_conquest_keeps_window(snapshot: Dictionary, target: Dictionary) -> void:
	var city_id := String(target["city"])
	var before := _harvest_months(target["seasons"])
	var conquered := snapshot.duplicate(true)
	if not _set_city_nation(conquered, city_id, NEW_OWNER):
		_fail("cidade %s não achada no save" % city_id)
		return
	var restored := _restore(conquered)
	if restored == null:
		return
	var plan := _plan_of(restored, city_id, CROP)
	if plan.is_empty():
		_fail("plano de %s|%s sumiu no load" % [city_id, CROP])
		return
	var after := _harvest_months(plan.get("seasons", []))
	print("[agri-owner] conquista no save (%s→%s): janela %s → %s" % [
		ORIGIN_NATION, NEW_OWNER, str(before), str(after)])
	if not _same_months(before, after):
		_fail("a conquista mudou a janela da safra: %s → %s"
			% [str(before), str(after)])
	# A nação dona do plano no save é a nova (é o dono da cidade), mas a janela
	# continua a herdada — produção usa o calendário da região, não o do dono.
	var city_nation := _city_nation(restored, city_id)
	if city_nation != NEW_OWNER:
		_fail("save recarregado não aplicou o dono novo em %s (veio %s)"
			% [city_id, city_nation])


func _test_guarded_reapply(snapshot: Dictionary, target: Dictionary) -> void:
	var city_id := String(target["city"])
	var conquered := snapshot.duplicate(true)
	_set_city_nation(conquered, city_id, NEW_OWNER)
	var restored := _restore(conquered)
	if restored == null:
		return
	var before := _harvest_months(_plan_of(restored, city_id, CROP).get("seasons", []))
	var agri := AgriScript.new()
	var report: Dictionary = agri.apply_to_sim(restored, _owner_swapped_cities(city_id),
		_geo, {
			"calendar_nations": _original,
			"only_missing": true,
			"initial_stock": false,
			"farms": _farms,
		})
	var after := _harvest_months(_plan_of(restored, city_id, CROP).get("seasons", []))
	var skipped := int(report.get("skipped_existing", 0))
	print("[agri-owner] reaplicação com guarda: pulados=%d janela %s → %s" % [
		skipped, str(before), str(after)])
	if skipped <= 0:
		_fail("only_missing não reconheceu nenhum plano existente")
	if not _same_months(before, after):
		_fail("reaplicação com guarda mudou a janela: %s → %s"
			% [str(before), str(after)])
	if float(report.get("initial_stock_t", 0.0)) > 0.0:
		_fail("reaplicação no load semeou estoque inicial de novo (%.0f t)"
			% float(report.get("initial_stock_t", 0.0)))


func _test_unguarded_reapply_changes(snapshot: Dictionary, target: Dictionary,
		naive_differs: bool) -> void:
	if not naive_differs:
		return
	var city_id := String(target["city"])
	var conquered := snapshot.duplicate(true)
	_set_city_nation(conquered, city_id, NEW_OWNER)
	var restored := _restore(conquered)
	if restored == null:
		return
	var before := _harvest_months(_plan_of(restored, city_id, CROP).get("seasons", []))
	var agri := AgriScript.new()
	agri.apply_to_sim(restored, _owner_swapped_cities(city_id), _geo,
		{"initial_stock": false})
	var after := _harvest_months(_plan_of(restored, city_id, CROP).get("seasons", []))
	print("[agri-owner] reaplicação SEM guarda (dono novo): janela %s → %s" % [
		str(before), str(after)])
	if _same_months(before, after):
		_fail("sem a guarda a janela não mudou (%s): o teste não prova a regressão"
			% str(after))


func _test_harvest_months(snapshot: Dictionary, target: Dictionary) -> void:
	var city_id := String(target["city"])
	var conquered := snapshot.duplicate(true)
	_set_city_nation(conquered, city_id, NEW_OWNER)
	var restored := _restore(conquered)
	if restored == null:
		return
	# Mesmo caminho do jogo no load: só preenche o que faltar.
	var agri := AgriScript.new()
	agri.apply_to_sim(restored, _owner_swapped_cities(city_id), _geo, {
		"calendar_nations": _original,
		"only_missing": true,
		"initial_stock": false,
		"farms": _farms,
	})
	# Janelas do dado: original (BRA) e do dono (USA).
	var calendars: Dictionary = _load_dict(CROP_CALENDAR_PATH)
	var allowed: Array = _calendar_months(calendars, ORIGIN_NATION, CROP)
	for month in _calendar_months(calendars, NEW_OWNER, CROP):
		if not allowed.has(month):
			allowed.append(month)
	allowed.sort()
	var delivered := {}
	for day in DAYS:
		restored.tick_day()
		var status: Dictionary = restored.get_agriculture_status()
		var month := _month_of_doy(int(status.get("date", {}).get("doy", 1)))
		for row in Array(status.get("nations", [])):
			if String((row as Dictionary).get("nation", "")) != NEW_OWNER:
				continue
			for crop in Array((row as Dictionary).get("crops", [])):
				var crop_row: Dictionary = crop
				if String(crop_row.get("product", "")) != CROP:
					continue
				var today := float(crop_row.get("delivered_today_t", 0.0))
				if today > 0.0:
					delivered[month] = float(delivered.get(month, 0.0)) + today
	var months: Array = delivered.keys()
	months.sort()
	print("[agri-owner] %d d no mundo conquistado: meses com colheita de %s=%s "
		% [DAYS, CROP, str(months)] + "(janelas permitidas %s)" % str(allowed))
	if months.is_empty():
		_fail("nenhuma colheita de %s depois da conquista" % CROP)
		return
	for month in months:
		if not allowed.has(month):
			_fail("colheita de %s em mês fora das janelas conhecidas: %d"
				% [CROP, month])
	var in_bra := false
	for month in months:
		if BRA_MONTHS.has(month):
			in_bra = true
	if not in_bra:
		_fail("a janela original (%s) não entregou nada: meses %s"
			% [str(BRA_MONTHS), str(months)])


# ---------------------------------------------------------------------------
# Apoio
# ---------------------------------------------------------------------------

## Lista de cidades com a cidade-alvo já com o dono novo (o mundo do save).
func _owner_swapped_cities(city_id: String) -> Array:
	var out: Array = []
	for city in _cities:
		var row: Dictionary = (city as Dictionary).duplicate()
		if String(row.get("id", "")) == city_id:
			row["nation"] = NEW_OWNER
		out.append(row)
	return out


func _restore(snapshot: Dictionary) -> Object:
	var sim = EconomySim.new()
	sim.set_goods(_load_array(GOODS_PATH))
	sim.set_recipes(_load_array(RECIPES_PATH))
	sim.set_needs(_load_dict(NEEDS_PATH))
	sim.set_services(_load_dict(SERVICES_PATH))
	sim.set_currencies(_load_dict(CURRENCIES_PATH))
	sim.set_sector_employment(_load_dict(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_load_dict(AGE_STRUCTURE_PATH))
	if sim.has_method("set_informality"):
		sim.set_informality(_load_dict(INFORMALITY_PATH))
	if sim.has_method("set_health_access"):
		sim.set_health_access(_load_dict(HEALTH_ACCESS_PATH))
	sim.from_snapshot(snapshot)
	return sim


func _set_city_nation(snapshot: Dictionary, city_id: String, nation: String) -> bool:
	var save_block: Dictionary = snapshot.get("save", {})
	for city in Array(save_block.get("cities", [])):
		var row: Dictionary = city
		if String(row.get("id", "")) == city_id:
			row["nation"] = nation
			return true
	return false


func _city_nation(sim: Object, city_id: String) -> String:
	var status: Dictionary = sim.get_city_status(city_id)
	return String(status.get("nation", ""))


func _plan_of(sim: Object, city_id: String, product: String) -> Dictionary:
	var snapshot: Dictionary = sim.to_snapshot()
	var definitions: Dictionary = snapshot.get("definitions", {})
	for plan in Array(definitions.get("crop_plans", [])):
		var row: Dictionary = plan
		if String(row.get("city", "")) == city_id \
				and String(row.get("product", "")) == product:
			return row
	return {}


## Meses (1..12) cobertos pelas janelas de colheita do plano.
func _harvest_months(seasons: Variant) -> Array:
	var out: Array = []
	for season in Array(seasons):
		var row: Dictionary = season
		var start := int(row.get("harvest_start", 0))
		var end := int(row.get("harvest_end", 0))
		if start <= 0 or end <= 0:
			continue
		var doy := start
		while true:
			var month := _month_of_doy(doy)
			if not out.has(month):
				out.append(month)
			if doy == end:
				break
			doy = (doy % 365) + 1
	out.sort()
	return out


## Meses das janelas de colheita de (nação, cultura) no calendário do dado.
func _calendar_months(calendars: Dictionary, nation: String, crop: String) -> Array:
	var entry: Dictionary = calendars.get("nations", {}).get(nation, {}).get(
		"crops", {}).get(crop, {})
	return _harvest_months(_windows_of(entry))


## O JSON do calendário traz `seasons` com `harvest` como mês(s) — converte
## para o mesmo formato do plano (doy). Espelha `_season_windows` do service.
func _windows_of(entry: Dictionary) -> Array:
	var out: Array = []
	for season in Array(entry.get("seasons", [])):
		var row: Dictionary = season
		var harvest: Dictionary = row.get("harvest", {})
		var start := _month_start(int(harvest.get("start_month", 0)))
		var end := _month_end(int(harvest.get("end_month", 0)))
		if start <= 0 or end <= 0:
			continue
		out.append({"harvest_start": start, "harvest_end": end})
	return out


func _month_start(month: int) -> int:
	if month < 1 or month > 12:
		return 0
	return [1, 32, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335][month - 1]


func _month_end(month: int) -> int:
	if month < 1 or month > 12:
		return 0
	return [31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365][month - 1]


func _month_of_doy(doy: int) -> int:
	for month in 12:
		if doy >= _month_start(month + 1) and doy <= _month_end(month + 1):
			return month + 1
	return 12


func _same_months(a: Array, b: Array) -> bool:
	if a.size() != b.size():
		return false
	for value in a:
		if not b.has(value):
			return false
	return true


func _load(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_array(path: String) -> Array:
	var data: Variant = _load(path)
	return data if typeof(data) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load(path)
	return data if typeof(data) == TYPE_DICTIONARY else {}
