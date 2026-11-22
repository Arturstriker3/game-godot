extends SceneTree

## C2b2 (D-106) — ESTRESSE FORTE DO CÂMBIO: 365 d com reservas mínimas.
##
## Roda pelo runner canônico:
##   powershell -File tools/run_tests.ps1 -List c2b2_fx_stress_test -TimeoutSec 7200
##   (opcional) $env:OGS_C2B2_STRESS_OUT = "res://docs/economy/perf/c2b2_fx_stress_365.json"
##
## Espelha o mundo do harness `tools/c1_world_ai_365.gd` (mesmas definições, mesma
## ordem de aplicação, IA ligada, threads=1) e muda SÓ a política cambial do
## cenário, para forçar esgotamento de reserva:
##   * reservas fundadas no dia 30 com 3 dias da importação MEDIDA do mundo
##     (`seed_central_bank_reserves(3)`);
##   * todas as moedas passam a `peg` na mesma data (o default de jogo é
##     `floating`, onde não há defesa e portanto não há como esgotar reserva);
##   * banda de 0,5 % (default de jogo: 5 %) — o desvio sombreado vira "gap"
##     maior e a defesa custa mais reserva por dia;
##   * defesa de 0,9 (default 0,5) — o BC tenta segurar 90 % do gap.
##
## Não é critério de aceite: é MEDIÇÃO. Sai != 0 só se o invariante financeiro
## do dia for violado (ou se um método do contrato sumir), porque isso seria bug.
## Números default (para comparar) em docs/economy/C2b2_fx_gold.md.

const GeoScript := preload("res://core/economy/geography_service.gd")
const AgriScript := preload("res://core/economy/agriculture_service.gd")
const StorageScript := preload("res://core/economy/storage_service.gd")
const RegionDataScript := preload("res://core/economy/region_data.gd")
const TrajectoryDigest := preload("res://core/economy/trajectory_digest.gd")

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
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

const DAYS := 365
const FX_SEED_DAYS := 3
const FX_BAND := 0.005
const FX_DEFENCE := 0.9
## Paridade do peg como fração da paridade corrente (0,8 = moeda 20 % mais forte
## que o mercado). Sem uma paridade FORA do mercado o sombreado nunca sai da
## banda `peg`: com a taxa presa na paridade o passo diário é 0,05 % e a defesa
## nunca dispara (medido: 365 d, 0 desvalorizações, reserva intacta).
const FX_PEG_OVERVALUE := 0.8
const GATE := 0.975
const WINDOW := 30

var _out := ""


func _init() -> void:
	_out = OS.get_environment("OGS_C2B2_STRESS_OUT")
	_run()


func _load(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		print("[c2b2-stress] FALHA: não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _make_world() -> Object:
	var cities: Array = _load(CITIES_PATH)
	var ports: Array = _load(PORTS_PATH)
	var buildings: Array = _load(BUILDINGS_PATH)
	var goods: Array = _load(GOODS_PATH)
	if cities.is_empty() or goods.is_empty():
		print("[c2b2-stress] FALHA: cenário vazio")
		return null
	RegionDataScript.attach(cities)
	var sim = EconomySim.new()
	sim.set_goods(goods)
	sim.set_recipes(_load(RECIPES_PATH))
	sim.set_needs(_load(NEEDS_PATH))
	sim.set_services(_load(SERVICES_PATH))
	sim.set_currencies(_load(CURRENCIES_PATH))
	sim.set_sector_employment(_load(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_load(AGE_STRUCTURE_PATH))
	sim.set_informality(_load(INFORMALITY_PATH))
	sim.set_health_access(_load(HEALTH_ACCESS_PATH))
	sim.set_sim_threads(1)
	var geo := GeoScript.new()
	geo.load_data()
	buildings = geo.resolve_initial_farms(buildings, cities)
	if geo.has_method("resolve_initial_mines"):
		buildings = geo.resolve_initial_mines(buildings, cities)
	sim.initialize_world(cities, ports, buildings)
	GeoScript.apply_initial_infra(sim)
	if sim.has_method("set_urban"):
		sim.set_urban(_load(URBAN_LAND_PATH), _load(HOUSING_CITY_PATH),
			_load(HOUSING_PATH), _load(MUNICIPAL_FINANCE_PATH))
	var centers: Array = []
	for city: Dictionary in cities:
		centers.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)),
				float(city.get("y", 0.0))),
		})
	geo.apply_to_sim(sim, centers)
	if sim.has_method("set_crop_plan"):
		var agri := AgriScript.new()
		agri.apply_to_sim(sim, cities, geo, {
			"calendar_nations": agri.calendar_nations(cities),
			"initial_stock": true,
			"farms": AgriScript.farms_of(buildings),
		})
	if sim.has_method("set_storage_capacity"):
		var storage := StorageScript.new()
		storage.apply_to_sim(sim, cities, buildings,
			{"only_missing": true, "initial_stock": true})
	sim.set_ai_enabled(true)
	return sim


func _food_gate(history: Array) -> Dictionary:
	var tags: Dictionary = {}
	for day_ratio in history:
		for tag in (day_ratio as Dictionary):
			if not tags.has(tag):
				tags[tag] = {"sum": 0.0, "days": 0}
			tags[tag]["sum"] = float(tags[tag]["sum"]) \
				+ float((day_ratio as Dictionary)[tag])
			tags[tag]["days"] = int(tags[tag]["days"]) + 1
	var values: Array = []
	var mean_acc := 0.0
	for tag in tags:
		var entry: Dictionary = tags[tag]
		var value := float(entry["sum"]) / float(maxi(1, int(entry["days"])))
		values.append({"nation": String(tag), "food_avg30": value})
		mean_acc += value
	values.sort_custom(func(a, b): return float(a["food_avg30"]) < float(b["food_avg30"]))
	var median := 0.0
	if not values.is_empty():
		var middle := values.size() / 2
		median = float(values[middle]["food_avg30"]) if values.size() % 2 == 1 \
			else (float(values[middle - 1]["food_avg30"])
				+ float(values[middle]["food_avg30"])) / 2.0
	var below: Array = []
	for row: Dictionary in values:
		if float(row["food_avg30"]) < GATE:
			below.append(String(row["nation"]))
	return {
		"nations": values.size(),
		"median": median,
		"mean": mean_acc / float(maxi(1, values.size())),
		"below_gate": below.size(),
		"below": below,
	}


func _currency_fx(sim: Object) -> Dictionary:
	var status: Dictionary = sim.get_central_bank_status()
	return status.get("currencies", {})


func _fx_report(sim: Object, first_day: Dictionary, days_with_event: Dictionary,
		devalued_samples: Array) -> Dictionary:
	var bank: Dictionary = sim.get_central_bank_status()
	var totals: Dictionary = bank.get("totals", {})
	var currencies: Dictionary = bank.get("currencies", {})
	var devalued: Array = []
	var funded := 0
	var unfunded: Array = []
	var regimes: Dictionary = {}
	var coverage_sum := 0.0
	var coverage_n := 0
	var reserves_zero := 0
	for code: Variant in currencies:
		var fx: Dictionary = (currencies[code] as Dictionary).get("fx", {})
		var regime := String(fx.get("regime_effective", ""))
		regimes[regime] = int(regimes.get(regime, 0)) + 1
		if bool(fx.get("funded", false)):
			funded += 1
		else:
			if unfunded.size() < 20:
				unfunded.append(String(code))
		if int(fx.get("devaluations", 0)) > 0:
			devalued.append({
				"currency": String(code),
				"devaluations": int(fx.get("devaluations", 0)),
				"first_day": int(first_day.get(String(code), 0)),
				"deviation": float(fx.get("deviation", 0.0)),
				"reserves": float(fx.get("reserves", 0.0)),
				"regime": String(fx.get("regime_effective", "")),
			})
		if float(fx.get("reserves", 0.0)) <= 0.0:
			reserves_zero += 1
		var cover := float(fx.get("coverage", 0.0))
		if cover > 0.0:
			coverage_sum += cover
			coverage_n += 1
	devalued.sort_custom(func(a, b):
		return int(a["devaluations"]) > int(b["devaluations"]))
	var by_day: Array = []
	for day: Variant in days_with_event:
		by_day.append({"day": int(day), "devaluations": int(days_with_event[day])})
	by_day.sort_custom(func(a, b): return int(a["day"]) < int(b["day"]))
	return {
		"seed_days": FX_SEED_DAYS,
		"band_width": FX_BAND,
		"defence_share": FX_DEFENCE,
		"currencies_total": currencies.size(),
		"funded_currencies": funded,
		"regimes": regimes,
		"unfunded_sample": unfunded,
		"currencies_without_reserve_at_end": reserves_zero,
		"devaluations": int(totals.get("devaluations", 0)),
		"currencies_devalued": devalued.size(),
		"devalued": devalued,
		"devalued_top": devalued.slice(0, mini(10, devalued.size())),
		"devaluation_days": by_day,
		"reserves": float(totals.get("reserves", 0.0)),
		"vault_gold_tonnes": float(totals.get("vault_gold_tonnes", 0.0)),
		"coverage_mean": coverage_sum / float(maxi(1, coverage_n)),
		"events_total": (bank.get("fx_events", []) as Array).size(),
		"events": (bank.get("fx_events", []) as Array).slice(0, 20),
		"devalued_samples_log": devalued_samples,
	}


func _run() -> void:
	var sim = _make_world()
	if sim == null:
		quit(1)
		return
	if not sim.has_method("seed_central_bank_reserves") \
			or not sim.has_method("set_exchange_policy"):
		print("[c2b2-stress] FALHA: contrato do câmbio ausente (D-106)")
		quit(1)
		return
	# Banda/defesa do cenário (default de jogo fica em exchange_policy.json).
	sim.set_exchange_policy(FX_BAND, FX_DEFENCE, 1.0)
	var money_start := float(sim.get_status().get("money_total", 0.0))
	var history: Array = []
	var violation_count := 0
	var violation_samples: Array = []
	var stalled := 0
	var max_drift := 0.0
	var day_before := int(sim.get_status().get("day", 0))
	var hash_lines := PackedStringArray()
	var first_day: Dictionary = {}
	var days_with_event: Dictionary = {}
	var devalued_samples: Array = []
	var prev_counts: Dictionary = {}
	var seeded_anchor := 0.0
	var wall_start := Time.get_ticks_msec()

	for day in DAYS:
		if day == 30:
			# Default do jogo: toda moeda é `floating` (sem defesa, sem como
			# esgotar reserva). O cenário põe as moedas em `peg` com paridade
			# 20 % ACIMA do mercado (moeda sobrevalorizada): o sombreado fica
			# fora da banda todo dia e o BC tem de vender reserva.
			var regimes := 0
			var known: Dictionary = _currency_fx(sim)
			for code: Variant in known:
				var fx0: Dictionary = (known[code] as Dictionary).get("fx", {})
				var parity0 := float(fx0.get("parity", 0.0))
				if parity0 <= 0.0:
					parity0 = float(fx0.get("rate", 1.0))
				if sim.set_exchange_regime(String(code), "peg",
						parity0 * FX_PEG_OVERVALUE, FX_BAND):
					regimes += 1
			# No dia 0 não há importação medida; 30 d de comércio dão a base.
			seeded_anchor = sim.seed_central_bank_reserves(FX_SEED_DAYS)
			var bank0: Dictionary = sim.get_central_bank_status()
			var funded0 := int((bank0.get("totals", {}) as Dictionary).get(
				"funded_currencies", 0))
			print("[c2b2-stress] dia 30: %d moedas em peg (paridade x%.2f), reservas %.1f âncora (funded=%d)"
				% [regimes, FX_PEG_OVERVALUE, seeded_anchor, funded0])
		var summary: Dictionary = sim.tick_day()
		for violation in Array(summary.get("invariant_violations", [])):
			violation_count += 1
			if violation_samples.size() < 10:
				violation_samples.append("dia %d: %s"
					% [day + 1, String(violation)])
		var day_now := int(summary.get("day", 0))
		if day_now <= day_before:
			stalled += 1
		day_before = day_now
		var status: Dictionary = sim.get_status()
		var money := float(status.get("money_total", 0.0))
		if money_start > 0.0:
			max_drift = maxf(max_drift, absf(money - money_start) / money_start)
		history.append(TrajectoryDigest.food_ratios(status))
		if history.size() > WINDOW:
			history.pop_front()
		hash_lines.append(TrajectoryDigest.line(day_now, status, sim))
		# Quando cada moeda desvalorizou: diferença de contador dia a dia.
		if day_now == 31:
			# Ao fim do primeiro dia com os comandos A3 aplicados: o cenário
			# tem de ver as moedas em `peg`; se não tiver, o estresse é vazio.
			var hist: Dictionary = {}
			var seen: Dictionary = _currency_fx(sim)
			var intervention := 0.0
			for code: Variant in seen:
				var fx1: Dictionary = (seen[code] as Dictionary).get("fx", {})
				var reg := String(fx1.get("regime_effective", ""))
				hist[reg] = int(hist.get(reg, 0)) + 1
				intervention += float(fx1.get("intervention_today", 0.0))
			print("[c2b2-stress] dia 31 regimes=%s intervenção do dia=%.1f"
				% [str(hist), intervention])
		if day_now >= 30:
			var currencies: Dictionary = _currency_fx(sim)
			var today := 0
			for code: Variant in currencies:
				var row: Dictionary = currencies[code]
				var count := int((row.get("fx", {}) as Dictionary)
					.get("devaluations", 0))
				var key := String(code)
				if not prev_counts.has(key):
					prev_counts[key] = count
					continue
				if count > int(prev_counts[key]):
					today += count - int(prev_counts[key])
					if not first_day.has(key):
						first_day[key] = day_now
						if devalued_samples.size() < 30:
							devalued_samples.append({
								"day": day_now,
								"currency": key,
								"new_parity": float((row.get("fx", {})
									as Dictionary).get("parity", 0.0)),
								"deviation": float((row.get("fx", {})
									as Dictionary).get("deviation", 0.0)),
							})
					prev_counts[key] = count
			if today > 0:
				days_with_event[day_now] = int(
					days_with_event.get(day_now, 0)) + today
		if day_now % 30 == 0 or day == DAYS - 1:
			print("[c2b2-stress] progresso dia %d/%d t=%.1fs"
				% [day_now, DAYS, (Time.get_ticks_msec() - wall_start) / 1000.0])

	var status_final: Dictionary = sim.get_status()
	var money_end := float(status_final.get("money_total", 0.0))
	var food := _food_gate(history)
	var balance: Dictionary = sim.get_balance_status()
	var invariant: Dictionary = balance.get("invariant", {})
	var fx := _fx_report(sim, first_day, days_with_event, devalued_samples)
	var out := {
		"days": DAYS,
		"threads": 1,
		"seeded_anchor": seeded_anchor,
		"final_day": int(status_final.get("day", 0)),
		"stalled_days": stalled,
		"invariant_violations": violation_count,
		"invariant_violation_samples": violation_samples,
		"invariant_ok": bool(invariant.get("ok", false)),
		"bankruptcies": int(status_final.get("bankruptcies", -1)),
		"money_start": money_start,
		"money_end": money_end,
		"drift": (money_end - money_start) / money_start if money_start > 0.0 else 0.0,
		"max_daily_drift": max_drift,
		"food_gate": food,
		"trajectory_sha256": TrajectoryDigest.sha256(hash_lines),
		"trajectory_fields": TrajectoryDigest.FIELDS,
		"trajectory_end": TrajectoryDigest.totals(sim),
		"fx": fx,
		"wall_s": (Time.get_ticks_msec() - wall_start) / 1000.0,
	}
	if _out != "":
		var file := FileAccess.open(_out, FileAccess.WRITE)
		if file == null:
			print("[c2b2-stress] FALHA: não escreveu " + _out)
		else:
			file.store_string(JSON.stringify(out, "  ", true))
			file.close()
	print("[c2b2-stress] dias=%d falências=%d violações=%d portão=%d/%d mediana=%.4f "
		% [DAYS, int(out["bankruptcies"]), violation_count,
			int(food["below_gate"]), int(food["nations"]),
			float(food["median"])]
		+ "desvalorizações=%d moedas=%d reservas=%.1f funded=%d/%d cobertura=%.4f hash=%s"
		% [int(fx["devaluations"]), int(fx["currencies_devalued"]),
			float(fx["reserves"]), int(fx["funded_currencies"]),
			int(fx["currencies_total"]), float(fx["coverage_mean"]),
			String(out["trajectory_sha256"]).substr(0, 8)])
	if violation_count > 0:
		print("[c2b2-stress] REPROVADO: invariante do dia violado")
		quit(1)
		return
	print("[c2b2-stress] OK: estresse medido (sem violação de invariante)")
	quit(0)
