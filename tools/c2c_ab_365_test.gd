extends SceneTree

const TmpPaths := preload("res://tools/tmp_paths.gd")

## C2c (D-108) — A/B POR CANAL, 365 d com IA (threads=1), tres mundos em
## sequencia NO MESMO processo (1 Godot por vez, nada em paralelo).
##
##   variante A `regimes_only`: regimes reais do AREAER, SEM a Bulgaria no euro
##     (BGN volta como moeda propria da BGR, exatamente como na base C3:
##     `currencies.json` de `53ce78dc^`, rate 0,493812 `derived:gdp_ppp`).
##   variante B `bulgaria_only`: SEM `set_fx_regimes` (o mundo nasce com o
##     "peg administrativo" da base C3/C2b2) e COM a Bulgaria no euro
##     (`currencies.json` da C2c: BGR na lista de nacoes do EUR).
##   variante C `default`: os dois canais ligados = default do jogo (D-108).
##
## So medicao: nao muda regra, nao muda parametro e nao muda o default. Roda
## pelo runner canonico (que nao passa argumentos):
##
##   powershell -File tools/run_tests.ps1 -List c2c_ab_365_test -TimeoutSec 1500
##
## Saida: `--out=` / `OGS_C2C_AB_OUT`; com `OPENGS_TMP` (o runner define) vai
## para `$OPENGS_TMP/c2c_ab_365.json`; senao `docs/economy/perf/c2c_ab_365.json`.
## O teste FALHA (exit 1) so se um mundo nao fechar integro (violacao do
## invariante, dia travado ou numero nao finito): e medicao, nao criterio de
## aceite.

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
const FX_REGIMES_PATH := "res://data/definitions/economy/fx_regimes_2026.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"
const INFORMALITY_PATH := "res://data/definitions/economy/informality.json"
const HEALTH_ACCESS_PATH := "res://data/definitions/population/health_access.json"
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

const OUT_PATH := "res://docs/economy/perf/c2c_ab_365.json"
## `currencies.json` antes da C2c (`git show 53ce78dc^:...`): BGN era moeda
## propria da BGR. Numero do proprio arquivo, nao inventado aqui.
const BGN_RATE_C3 := 0.493812
## Decisao (UE) 2025/1407 (1 EUR = 1,95583 BGN), a mesma do `retired` do dado.
const BGN_PER_EUR := 1.95583

const GATE := 0.975
const WINDOW := 30
const SNAPSHOT_EVERY := 30
const DAYS := 365

const VARIANTS := [
	{"name": "A_regimes_only", "regimes": true, "bulgaria": false},
	{"name": "B_bulgaria_only", "regimes": false, "bulgaria": true},
	{"name": "C_default", "regimes": true, "bulgaria": true},
]

var _out := OUT_PATH
var _days := DAYS
var _failures: Array[String] = []


func _init() -> void:
	if OS.get_environment("OGS_C2C_AB_OUT") != "":
		_out = OS.get_environment("OGS_C2C_AB_OUT")
	elif OS.get_environment("OPENGS_TMP").strip_edges() != "":
		# Regra de disco: o runner define OPENGS_TMP; o JSON grande sai do repo.
		_out = TmpPaths.path("c2c_ab_365.json")
	if OS.get_environment("OGS_C2C_AB_DAYS") != "":
		_days = maxi(1, int(OS.get_environment("OGS_C2C_AB_DAYS")))
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
		elif arg.begins_with("--days="):
			_days = maxi(1, int(arg.split("=", true, 1)[1]))
	var reports: Array = []
	for variant: Dictionary in VARIANTS:
		print("[c2c-ab] === %s (regimes=%s bulgaria=%s) ===" % [
			String(variant["name"]), str(variant["regimes"]),
			str(variant["bulgaria"])])
		var report: Dictionary = _run_variant(bool(variant["regimes"]),
			bool(variant["bulgaria"]))
		if report.is_empty():
			_failures.append("%s: mundo nao montou" % String(variant["name"]))
			continue
		report["variant"] = String(variant["name"])
		report["regimes"] = bool(variant["regimes"])
		report["bulgaria"] = bool(variant["bulgaria"])
		reports.append(report)
		_print_report(report)
	_write_out(reports)
	print("[c2c-ab] tabela (%d d, IA threads=1):" % _days)
	print("[c2c-ab] variante        falencias portao  infl p10   p50      p90      "
		+ "juros p50  abaixo-3pct  hash")
	for report: Dictionary in reports:
		var infl: Dictionary = report["inflation_12m_pct"]
		print("[c2c-ab] %-15s %9d %4d/%-3d %8.3f %8.3f %8.3f %8.3f %5d  %s" % [
			String(report["variant"]), int(report["bankruptcies"]),
			int(report["food_gate"]["below_gate"]),
			int(report["food_gate"]["nations"]),
			float(infl["p10"]), float(infl["median"]), float(infl["p90"]),
			float(report["rate_pct"]["median"]),
			int(report["nations_below_minus_3"]),
			String(report["trajectory_sha256"]).substr(0, 8)])
	if _failures.is_empty():
		print("[c2c-ab] OK: %d variantes medidas; 0 violacoes e 0 dias travados."
			% reports.size())
		quit(0)
	else:
		print("[c2c-ab] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _load(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


## Moedas como na base C3 quando `bulgaria` e falso: EUR sem BGR e BGN de volta
## como moeda propria (o resto do arquivo fica igual).
func _currencies_data(bulgaria: bool) -> Dictionary:
	var data: Dictionary = _load(CURRENCIES_PATH)
	if bulgaria:
		return data
	var list: Array = data.get("currencies", [])
	var out: Array = []
	for entry: Variant in list:
		var row: Dictionary = entry
		if String(row.get("code", "")) != "EUR":
			out.append(row)
			continue
		var eur: Dictionary = row.duplicate(true)
		var nations: Array = []
		for tag: Variant in eur.get("nations", []):
			if String(tag) != "BGR":
				nations.append(tag)
		eur["nations"] = nations
		out.append(eur)
	out.append({"code": "BGN", "nations": ["BGR"],
		"rate_per_anchor": BGN_RATE_C3, "source": "derived:gdp_ppp"})
	data["currencies"] = out
	return data


func _make_world(regimes: bool, bulgaria: bool) -> Object:
	var cities: Array = _load(CITIES_PATH)
	var ports: Array = _load(PORTS_PATH)
	var buildings: Array = _load(BUILDINGS_PATH)
	var goods: Array = _load(GOODS_PATH)
	if cities.is_empty() or goods.is_empty():
		print("[c2c-ab] FALHA: cenario vazio")
		return null
	RegionDataScript.attach(cities)
	var sim = EconomySim.new()
	sim.set_goods(goods)
	sim.set_recipes(_load(RECIPES_PATH))
	sim.set_needs(_load(NEEDS_PATH))
	sim.set_services(_load(SERVICES_PATH))
	# C2c (D-108): canal dos regimes reais, ANTES do carregamento das moedas.
	if regimes and sim.has_method("set_fx_regimes"):
		sim.set_fx_regimes(_load(FX_REGIMES_PATH))
	sim.set_currencies(_currencies_data(bulgaria))
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
	for tag in tags:
		var entry: Dictionary = tags[tag]
		values.append({"nation": String(tag),
			"food_avg30": float(entry["sum"]) / float(maxi(1, int(entry["days"])))})
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
		"below_gate": below.size(),
		"below": below,
		"worst": values.slice(0, mini(10, values.size())),
	}


func _spread(values: Array) -> Dictionary:
	if values.is_empty():
		return {"n": 0, "median": 0.0, "p10": 0.0, "p90": 0.0}
	var sorted := values.duplicate()
	sorted.sort()
	var n := sorted.size()
	var median := float(sorted[n / 2]) if n % 2 == 1 \
		else (float(sorted[n / 2 - 1]) + float(sorted[n / 2])) * 0.5
	return {
		"n": n,
		"median": median,
		"p10": float(sorted[clampi(int(floor(0.10 * (n - 1))), 0, n - 1)]),
		"p90": float(sorted[clampi(int(ceil(0.90 * (n - 1))), 0, n - 1)]),
	}


## Construcoes que desapareceram (falencia ou jazida esgotada) entre snapshots
## de `SNAPSHOT_EVERY` dias. O status leve nao publica construcoes, entao o
## detalhe vem de `get_city_status` - so nos dias de snapshot.
func _snapshot_buildings(sim: Object, status: Dictionary) -> Dictionary:
	var snap: Dictionary = {}
	for city: Dictionary in status.get("cities", []):
		var cid := String(city.get("id", ""))
		var heavy: Dictionary = sim.get_city_status(cid)
		for building: Dictionary in heavy.get("buildings", []):
			snap["%s|%s" % [cid, String(building.get("id", ""))]] = {
				"city": cid,
				"nation": String(city.get("nation", "")),
				"id": String(building.get("id", "")),
				"output_product": int(building.get("output_product", -1)),
				"capacity": float(building.get("capacity", 0.0)),
				"owner": String(building.get("owner", "")),
				"profit": float(building.get("profit", 0.0)),
				"loss_days": float(building.get("loss_days", 0.0)),
				"bankrupt": bool(building.get("bankrupt", false)),
			}
	return snap


## `nations` da tabela: inflacao 12 m + regime/ancora da moeda + desvio da taxa
## contra a taxa do dia 0 (`rate / base_rate`, o fator de competitividade).
func _nation_rows(sim: Object) -> Array:
	var bank: Dictionary = sim.get_central_bank_status()
	var cpi: Dictionary = bank.get("cpi", {})
	var currencies: Dictionary = bank.get("currencies", {})
	var rows: Array = []
	for nation: Variant in cpi:
		var tag := String(nation)
		var row: Dictionary = cpi[nation]
		if not bool(row.get("has_12m", false)):
			continue
		var cur: Dictionary = sim.get_currency(tag)
		var code := String(cur.get("code", ""))
		var fx: Dictionary = (currencies.get(code, {}) as Dictionary).get("fx", {})
		var base := float(cur.get("base_rate", 0.0))
		var rate := float(cur.get("rate", 0.0))
		rows.append({
			"nation": tag,
			"currency": code,
			"inflation_12m_pct": float(row.get("inflation_12m", 0.0)) * 100.0,
			"index": float(row.get("index", 0.0)),
			"regime": String(fx.get("regime_saved", "")),
			"anchor": String(fx.get("anchor", "")),
			"anchor_type": String(fx.get("anchor_type", "")),
			"rate": rate,
			"base_rate": base,
			"rate_ratio": (rate / base) if base > 0.0 else 0.0,
			"devaluations": int(fx.get("devaluations", 0)),
			"funded": bool(fx.get("funded", false)),
		})
	rows.sort_custom(func(a, b):
		return float(a["inflation_12m_pct"]) < float(b["inflation_12m_pct"]))
	return rows


## Paridade do peg/banda contra a taxa do dia 0: `target_rate / base_rate - 1`.
## Zero em todo o mundo = a paridade inicial E a taxa vigente no dia 0 (nenhum
## ajuste forcado no primeiro dia). So leitura do status do BC.
func _parity_probe(sim: Object) -> Dictionary:
	var bank: Dictionary = sim.get_central_bank_status()
	var currencies: Dictionary = bank.get("currencies", {})
	var status: Dictionary = sim.get_status()
	var seen: Dictionary = {}
	var count := 0
	var worst := 0.0
	var worst_code := ""
	var flagged: Array = []
	for city: Dictionary in status.get("cities", []):
		var cur: Dictionary = sim.get_currency(String(city.get("nation", "")))
		var code := String(cur.get("code", ""))
		if code.is_empty() or seen.has(code):
			continue
		seen[code] = true
		var fx: Dictionary = (currencies.get(code, {}) as Dictionary).get("fx", {})
		if String(fx.get("regime_saved", "")) == "floating":
			continue
		var target := float(fx.get("target_rate", 0.0))
		var base := float(cur.get("base_rate", 0.0))
		if not (target > 0.0) or not (base > 0.0):
			continue
		count += 1
		var dev := absf(target / base - 1.0)
		if dev > worst:
			worst = dev
			worst_code = code
		if dev > 1e-9:
			flagged.append({"currency": code, "target": target, "base": base,
				"dev": dev})
	return {
		"fixed_or_band_currencies": count,
		"max_deviation": worst,
		"worst": worst_code,
		"flagged": flagged.size(),
		"sample": flagged.slice(0, 8),
	}


func _run_variant(regimes: bool, bulgaria: bool) -> Dictionary:
	var sim = _make_world(regimes, bulgaria)
	if sim == null:
		return {}
	var money_start := float(sim.get_status().get("money_total", 0.0))
	# Paridade x taxa do dia 0: um peg/banda nasce preso a taxa com que o mundo
	# nasceu (initialize_anchors -> parity = ancora / base_rate). Mede o desvio.
	var parity_day0 := _parity_probe(sim)
	var parity_day1 := {}
	var history: Array = []
	var violations := 0
	var stalled := 0
	var max_drift := 0.0
	var day_before := int(sim.get_status().get("day", 0))
	var hash_lines := PackedStringArray()
	var closures: Array = []
	var bankruptcy_days: Array = []
	var bankruptcies_before := int(sim.get_status().get("bankruptcies", 0))
	var snapshot: Dictionary = {}
	var products: Array = sim.get_status().get("products", [])
	var wall_start := Time.get_ticks_msec()
	for day in _days:
		var summary: Dictionary = sim.tick_day()
		violations += Array(summary.get("invariant_violations", [])).size()
		var day_now := int(summary.get("day", 0))
		if day_now <= day_before:
			stalled += 1
		day_before = day_now
		var status: Dictionary = sim.get_status()
		var bankruptcies := int(status.get("bankruptcies", 0))
		if bankruptcies > bankruptcies_before:
			bankruptcy_days.append({"day": day_now,
				"count": bankruptcies - bankruptcies_before,
				"total": bankruptcies})
			bankruptcies_before = bankruptcies
		var money := float(status.get("money_total", 0.0))
		if money_start > 0.0:
			max_drift = maxf(max_drift, absf(money - money_start) / money_start)
		history.append(TrajectoryDigest.food_ratios(status))
		if history.size() > WINDOW:
			history.pop_front()
		hash_lines.append(TrajectoryDigest.line(day_now, status, sim))
		if day == 0:
			parity_day1 = _parity_probe(sim)
		if day == 0 or (day + 1) % SNAPSHOT_EVERY == 0 or day == _days - 1:
			var current := _snapshot_buildings(sim, status)
			if not snapshot.is_empty():
				for bid: Variant in snapshot:
					if current.has(bid):
						continue
					var gone: Dictionary = (snapshot[bid] as Dictionary).duplicate()
					gone["snapshot_before"] = day_now - SNAPSHOT_EVERY if day_now > SNAPSHOT_EVERY else 0
					gone["snapshot_after"] = day_now
					if int(gone["output_product"]) >= 0 \
							and int(gone["output_product"]) < products.size():
						gone["product"] = String(products[int(gone["output_product"])])
					closures.append(gone)
			snapshot = current
		if day_now % 60 == 0 or day == _days - 1:
			print("[c2c-ab]   dia %d/%d t=%.1fs" % [day_now, _days,
				(Time.get_ticks_msec() - wall_start) / 1000.0])
	var status_final: Dictionary = sim.get_status()
	var money_end := float(status_final.get("money_total", 0.0))
	var bank: Dictionary = sim.get_central_bank_status()
	var currencies: Dictionary = bank.get("currencies", {})
	var rates: Array = []
	var by_regime: Dictionary = {}
	var by_anchor: Dictionary = {}
	var devaluations := 0
	var locked := 0
	for code: Variant in currencies:
		var row: Dictionary = currencies[code]
		var fx: Dictionary = row.get("fx", {})
		var regime := String(fx.get("regime_saved", ""))
		by_regime[regime] = int(by_regime.get(regime, 0)) + 1
		var anchor := String(fx.get("anchor", ""))
		if not anchor.is_empty() and anchor != "<null>":
			by_anchor[anchor] = int(by_anchor.get(anchor, 0)) + 1
		if bool(fx.get("locked", false)):
			locked += 1
		devaluations += int(fx.get("devaluations", 0))
		rates.append(float((row.get("policy", {}) as Dictionary).get("rate", 0.0))
			* 100.0)
	var rows := _nation_rows(sim)
	var infl: Array = []
	var below_minus_3 := 0
	for row: Dictionary in rows:
		var value := float(row["inflation_12m_pct"])
		infl.append(value)
		if value < -3.0:
			below_minus_3 += 1
	var report := {
		"days": _days,
		"final_day": int(status_final.get("day", 0)),
		"stalled_days": stalled,
		"invariant_violations": violations,
		"bankruptcies": int(status_final.get("bankruptcies", 0)),
		"bankruptcy_days": bankruptcy_days,
		"closures": closures,
		"money_start": money_start,
		"money_end": money_end,
		"max_daily_drift": max_drift,
		"food_gate": _food_gate(history),
		"inflation_12m_pct": _spread(infl),
		"nations_below_minus_3": below_minus_3,
		"nations_with_12m": infl.size(),
		"rate_pct": _spread(rates),
		"fx": {
			"by_regime": by_regime,
			"by_anchor": by_anchor,
			"locked_currencies": locked,
			"devaluations": devaluations,
			"currencies": currencies.size(),
		},
		"parity_day0": parity_day0,
		"parity_day1": parity_day1,
		"nations": rows,
		"trajectory_sha256": TrajectoryDigest.sha256(hash_lines),
		"wall_s": (Time.get_ticks_msec() - wall_start) / 1000.0,
	}
	if violations > 0:
		_failures.append("violacoes do invariante: %d" % violations)
	if stalled > 0:
		_failures.append("dias travados: %d" % stalled)
	if not is_finite(money_end) or money_start <= 0.0:
		_failures.append("moneyTotal nao finito")
	return report


func _print_report(report: Dictionary) -> void:
	var infl: Dictionary = report["inflation_12m_pct"]
	print("[c2c-ab] %s: falencias=%d portao=%d/%d infl p10/p50/p90=%.3f/%.3f/%.3f "
		% [String(report["variant"]), int(report["bankruptcies"]),
			int(report["food_gate"]["below_gate"]),
			int(report["food_gate"]["nations"]),
			float(infl["p10"]), float(infl["median"]), float(infl["p90"])]
		+ "juros p50=%.3f abaixo_de_-3pct=%d hash=%s" % [
			float(report["rate_pct"]["median"]),
			int(report["nations_below_minus_3"]),
			String(report["trajectory_sha256"]).substr(0, 8)])
	print("[c2c-ab] regimes=%s ancoras=%s travadas=%d desvalorizacoes=%d"
		% [str(report["fx"]["by_regime"]), str(report["fx"]["by_anchor"]),
			int(report["fx"]["locked_currencies"]),
			int(report["fx"]["devaluations"])])
	var p0: Dictionary = report["parity_day0"]
	var p1: Dictionary = report["parity_day1"]
	print(("[c2c-ab] paridade x taxa dia 0: fixas/banda=%d desvio max=%s (%s); "
		+ "dia 1: %s (%s)") % [
			int(p0["fixed_or_band_currencies"]), str(float(p0["max_deviation"])),
			String(p0["worst"]), str(float(p1.get("max_deviation", 0.0))),
			String(p1.get("worst", ""))])
	var rows: Array = report["nations"]
	print("[c2c-ab] 15 menores inflacoes 12 m:")
	for index in mini(15, rows.size()):
		var row: Dictionary = rows[index]
		print("  %-4s %-4s %-9s %-5s %7.3f%% taxa/base=%.4f" % [
			String(row["nation"]), String(row["currency"]),
			String(row["regime"]), String(row["anchor"]),
			float(row["inflation_12m_pct"]), float(row["rate_ratio"])])
	var closures: Array = report["closures"]
	if not closures.is_empty():
		print("[c2c-ab] construcoes que desapareceram: %d" % closures.size())
		for closure: Dictionary in closures:
			print("  dia~%d %s/%s produto=%s cap=%.2f lucro=%.1f perda_dias=%.0f"
				% [int(closure.get("snapshot_after", 0)),
					String(closure.get("nation", "")),
					String(closure.get("city", "")),
					String(closure.get("product", "?")),
					float(closure.get("capacity", 0.0)),
					float(closure.get("profit", 0.0)),
					float(closure.get("loss_days", 0.0))])


func _write_out(reports: Array) -> void:
	var file := FileAccess.open(_out, FileAccess.WRITE)
	if file == null:
		print("[c2c-ab] FALHA: nao escreveu " + _out)
		_failures.append("sem arquivo de saida")
		return
	var data := {
		"tool": "tools/c2c_ab_365_test.gd",
		"days": _days,
		"threads": 1,
		"ai": true,
		"credit": false,
		"property": false,
		"fx_seed_days": 0,
		"note": "A=regimes reais sem Bulgaria; B=Bulgaria no euro com os regimes da base C3; C=default do jogo (D-108).",
		"bgn_rate_c3": BGN_RATE_C3,
		"bgn_per_eur": BGN_PER_EUR,
		"variants": reports,
	}
	file.store_string(JSON.stringify(data, "  ", true))
	file.close()
	print("[c2c-ab] evidencia em " + _out)
