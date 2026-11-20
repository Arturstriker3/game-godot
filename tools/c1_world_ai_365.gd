extends SceneTree

## C1 — mundo real com IA por N dias, com a contabilidade por balanço ligada.
##
##   godot --headless --path . --script res://tools/c1_world_ai_365.gd -- \
##     [--days=365] [--out=docs/economy/perf/c1_world_ai_365.json]
##
## Monta o mundo pelo caminho do AUTOLOAD (cidades + geografia real + infra B5
## inicial + camada urbana + B7 + agricultura sazonal + armazenagem G2 + IA do
## núcleo, `threads=1`) e mede o aceite da C1:
##   * INVARIANTE FINANCEIRO do dia (soma das posições = 0) e conservação de
##     caixa — 0 violações;
##   * falências e dias travados;
##   * drift máximo do caixa contra o dia 0;
##   * PORTÃO DE COMIDA do M2 (média de 30 d de consumido/necessidade por nação,
##     < 0,975 = fora) — a base da D-101 é 85/179;
##   * BALANÇO por nação (PL das famílias/firmas/governo, patrimônio B6,
##     estoque G2, salário atrasado e liquidez × solvência das plantas).
##
## Só medição: não move dinheiro nem muda regra. `--days` curto serve de fumaça.

const GeoScript := preload("res://core/economy/geography_service.gd")
const LandPropertyScript := preload("res://core/economy/land_property.gd")
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

const GATE := 0.975
const WINDOW := 30
const SAMPLE_NATIONS: Array[String] = ["USA", "CHN", "IND", "BRA", "DEU", "NGA",
	"RUS", "IDN", "MEX", "ZAF"]

var _days := 365
var _property := false
var _out := "res://docs/economy/perf/c1_world_ai_365.json"
var _fx_seed_days := 0
# C3 (D-107): medição com o crédito de empresa LIGADO (default do jogo = false).
# Só laboratório: não existe botão/atalho/HUD para isso no jogo.
var _credit := false


func _init() -> void:
	# Permite rodar pelo runner canônico (tools/run_tests.ps1), que não passa
	# argumentos de usuário: `OGS_C1_OUT` escolhe o arquivo de saída.
	if OS.get_environment("OGS_C1_OUT") != "":
		_out = OS.get_environment("OGS_C1_OUT")
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = maxi(1, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--property="):
			_property = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
		elif arg.begins_with("--credit="):
			_credit = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--fx-seed-days="):
			# C2b2 (D-106): cenário de estresse — funda as reservas de cada
			# moeda com N dias de importação MEDIDA do mundo e o peg passa a ser
			# defendido (reserva esgotada = desvalorização forçada).
			_fx_seed_days = maxi(0, int(arg.split("=", true, 1)[1]))
	_run()


func _load(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
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
		print("[c1] FALHA: cenário vazio")
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
	# B6 (patrimônio da região): o autoload aplica em produção; aqui é opcional
	# para o A/B do 365 d continuar comparável ao mundo da D-101.
	if _property:
		LandPropertyScript.apply(sim)
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
	if _credit:
		sim.set_bank_credit(true)
		print("[c1] C3: credito de empresa LIGADO por flag (--credit=1)")
	return sim


func _food_ratio(status: Dictionary) -> Dictionary:
	# Dono único da conta (portão e hash): core/economy/trajectory_digest.gd.
	return TrajectoryDigest.food_ratios(status)


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
		"worst": values.slice(0, mini(10, values.size())),
	}


## C2b1 (D-104): política monetária medida no fecho do mundo — taxa básica por
## moeda e inflação por país. A inflação de 12 meses só existe com 13 amostras
## do CPI (dia 390); antes disso a ANUALIZADA é publicada no lugar e a ausência
## do número de 12 meses é declarada (`nations_has_12m`).
func _monetary_report(sim: Object) -> Dictionary:
	var bank: Dictionary = sim.get_central_bank_status()
	var rates: Array = []
	var controlled := 0
	var with_inflation := 0
	var currencies: Dictionary = bank.get("currencies", {})
	for code: Variant in currencies:
		var row: Dictionary = currencies[code]
		var policy: Dictionary = row.get("policy", {})
		rates.append(float(policy.get("rate", 0.0)) * 100.0)
		if String(policy.get("independence_effective", "")) == "controlled":
			controlled += 1
		if bool(policy.get("has_inflation", false)):
			with_inflation += 1
	var infl_12m: Array = []
	var infl_ann: Array = []
	var cpi: Dictionary = bank.get("cpi", {})
	for nation: Variant in cpi:
		var row: Dictionary = cpi[nation]
		if bool(row.get("has_12m", false)):
			infl_12m.append(float(row.get("inflation_12m", 0.0)) * 100.0)
		if bool(row.get("has_annualized", false)):
			infl_ann.append(float(row.get("inflation_annualized", 0.0)) * 100.0)
	var totals: Dictionary = bank.get("totals", {})
	return {
		"currencies": currencies.size(),
		"controlled": controlled,
		"currencies_with_inflation": with_inflation,
		"rate_pct": _spread(rates),
		"inflation_12m_pct": _spread(infl_12m),
		"nations_has_12m": infl_12m.size(),
		"inflation_annualized_pct": _spread(infl_ann),
		"nations_annualized": infl_ann.size(),
		"emission_total": float(totals.get("emission_total", 0.0)),
		"emission_today": float(totals.get("emission_today", 0.0)),
		"securities": float(totals.get("securities", 0.0)),
		"government_debt": float(totals.get("government_debt", 0.0)),
	}


## D-105: estado da flag de juro no envelope de crédito da safra. `-1` = build
## sem a flag (base C2a), onde o termo não existe.
func _harvest_credit_rate(sim: Object) -> Variant:
	if not sim.has_method("get_central_bank_status"):
		return -1
	var bank: Dictionary = sim.get_central_bank_status()
	var policy: Dictionary = bank.get("monetary_policy", {})
	if not policy.has("harvest_credit_rate"):
		return -1
	return bool(policy.get("harvest_credit_rate", false))


## C2b2 (D-106): relatório do cenário de câmbio — quantas moedas desvalorizaram,
## quantas ficaram sem reserva, cobertura e eventos. Só medição.
func _fx_report(sim: Object, seed_days: int) -> Dictionary:
	var bank: Dictionary = sim.get_central_bank_status()
	var totals: Dictionary = bank.get("totals", {})
	var currencies: Dictionary = bank.get("currencies", {})
	var devalued: Array = []
	var funded := 0
	var coverage_sum := 0.0
	var coverage_n := 0
	for code: Variant in currencies:
		var fx: Dictionary = (currencies[code] as Dictionary).get("fx", {})
		if bool(fx.get("funded", false)):
			funded += 1
		if int(fx.get("devaluations", 0)) > 0:
			devalued.append({
				"currency": String(code),
				"devaluations": int(fx.get("devaluations", 0)),
				"deviation": float(fx.get("deviation", 0.0)),
			})
		var cover := float(fx.get("coverage", 0.0))
		if cover > 0.0:
			coverage_sum += cover
			coverage_n += 1
	devalued.sort_custom(func(a, b):
		return int(a["devaluations"]) > int(b["devaluations"]))
	return {
		"seed_days": seed_days,
		"funded_currencies": funded,
		"devaluations": int(totals.get("devaluations", 0)),
		"currencies_devalued": devalued.size(),
		"devalued_top": devalued.slice(0, mini(10, devalued.size())),
		"reserves": float(totals.get("reserves", 0.0)),
		"vault_gold_tonnes": float(totals.get("vault_gold_tonnes", 0.0)),
		"coverage_mean": coverage_sum / float(maxi(1, coverage_n)),
		"events": (bank.get("fx_events", []) as Array).slice(0, 10),
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


func _balance_report(sim: Object, status: Dictionary) -> Dictionary:
	if not sim.has_method("get_balance_status"):
		# Build sem a C1 (A/B): o resto do harness continua comparável.
		return {"invariant": {"ok": true}, "arrears": {}, "world": {},
			"sample_nations": {}, "nations": 0}
	var balance: Dictionary = sim.get_balance_status()
	var world: Dictionary = balance.get("world", {})
	var by_nation: Dictionary = balance.get("by_nation", {})
	var cities: Array = status.get("cities", [])
	var population_by_nation := {}
	for city: Dictionary in cities:
		var tag := String(city.get("nation", ""))
		population_by_nation[tag] = float(population_by_nation.get(tag, 0.0)) \
			+ float(city.get("simulation_population", 0.0))
	var sample: Dictionary = {}
	for tag in SAMPLE_NATIONS:
		if not by_nation.has(tag):
			continue
		var row: Dictionary = by_nation[tag]
		var fam: Dictionary = row.get("families", {})
		var firms: Dictionary = row.get("firms", {})
		var gov: Dictionary = row.get("government", {})
		var people := float(population_by_nation.get(tag, 0.0))
		sample[tag] = {
			"families_net_worth": float(fam.get("net_worth", 0.0)),
			"families_cash": float(fam.get("cash", 0.0)),
			"families_property": float(fam.get("property", 0.0)),
			"families_receivables": float(fam.get("receivables", 0.0)),
			"firms_net_worth": float(firms.get("net_worth", 0.0)),
			"firms_inventory": float(firms.get("inventory", 0.0)),
			"firms_payables": float(firms.get("payables", 0.0)),
			"government_net_worth": float(gov.get("net_worth", 0.0)),
			"net_worth_per_capita": float(fam.get("net_worth", 0.0))
				/ people if people > 0.0 else 0.0,
		}
	var families: Dictionary = world.get("families", {})
	var firms: Dictionary = world.get("firms", {})
	var government: Dictionary = world.get("government", {})
	var external: Dictionary = world.get("external", {})
	var solvency: Dictionary = world.get("firms_solvency", {})
	var invariant: Dictionary = balance.get("invariant", {})
	return {
		"invariant": invariant,
		"arrears": {
			"plants": float(invariant.get("arrears_plants", 0.0)),
			"service": float(invariant.get("arrears_service", 0.0)),
			"rural": float(invariant.get("arrears_rural", 0.0)),
		},
		"world": {
			"families_net_worth": float(families.get("net_worth", 0.0)),
			"families_cash": float(families.get("cash", 0.0)),
			"families_receivables": float(families.get("receivables", 0.0)),
			"families_property": float(families.get("property", 0.0)),
			"families_inventory": float(families.get("inventory", 0.0)),
			"firms_net_worth": float(firms.get("net_worth", 0.0)),
			"firms_inventory": float(firms.get("inventory", 0.0)),
			"firms_fixed_capital": float(firms.get("fixed_capital", 0.0)),
			"firms_payables": float(firms.get("payables", 0.0)),
			"government_net_worth": float(government.get("net_worth", 0.0)),
			"government_inventory": float(government.get("inventory", 0.0)),
			"external_cash": float(external.get("cash", 0.0)),
			"firms_solvency": solvency,
		},
		"sample_nations": sample,
		"nations": by_nation.size(),
	}


func _run() -> void:
	var sim = _make_world()
	if sim == null:
		quit(1)
		return
	if _fx_seed_days > 0 and sim.has_method("seed_central_bank_reserves"):
		# C2b2 (D-106): o cenário funda as reservas DEPOIS de 30 dias de comércio
		# (no dia 0 não há importação medida); o laço abaixo faz a chamada.
		print("[c1] cenário C2b2: reservas = %d dias de importação no dia 30"
			% _fx_seed_days)
	var money_start := float(sim.get_status().get("money_total", 0.0))
	var history: Array = []
	var violation_count := 0
	var violation_samples: Array = []
	var stalled := 0
	var max_drift := 0.0
	var day_before := int(sim.get_status().get("day", 0))
	var hash_lines := PackedStringArray()
	var wall_start := Time.get_ticks_msec()

	for day in _days:
		if _fx_seed_days > 0 and day == 30:
			# Câmbio: as reservas do cenário são 30 dias da importação MEDIDA do
			# mundo (dia 30; no dia 0 não há comércio).
			var seeded: float = sim.seed_central_bank_reserves(_fx_seed_days)
			print("[c1] cenário C2b2: reservas = %.3f âncora no dia 30" % seeded)
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
		var ratio := _food_ratio(status)
		history.append(ratio)
		if history.size() > WINDOW:
			history.pop_front()
		var balance_block: Dictionary = status.get("balance", {})
		# C2b1-fix (D-105): linha canônica única (dinheiro 6 casas, falências,
		# comida, M0, estoque privado, crédito). Ver
		# core/economy/trajectory_digest.gd e docs/economy/inv_c2b1_gate/README.md.
		hash_lines.append(TrajectoryDigest.line(day_now, status, sim))
		if int(day_now) % 30 == 0 or day == _days - 1:
			print("[c1] progresso dia %d/%d t=%.1fs"
				% [int(day_now), _days,
					(Time.get_ticks_msec() - wall_start) / 1000.0])

	var status_final: Dictionary = sim.get_status()
	var money_end := float(status_final.get("money_total", 0.0))
	var food := _food_gate(history)
	var balance := _balance_report(sim, status_final)
	var trajectory := TrajectoryDigest.sha256(hash_lines)
	var out := {
		"days": _days,
		"threads": 1,
		"property": _property,
		"final_day": int(status_final.get("day", 0)),
		"stalled_days": stalled,
		"invariant_violations": violation_count,
		"invariant_violation_samples": violation_samples,
		"bankruptcies": int(status_final.get("bankruptcies", -1)),
		"money_start": money_start,
		"money_end": money_end,
		"drift": (money_end - money_start) / money_start if money_start > 0.0 else 0.0,
		"max_daily_drift": max_drift,
		"food_gate": food,
		"balance": balance,
		"trajectory_sha256": trajectory,
		"trajectory_fields": TrajectoryDigest.FIELDS,
		"trajectory_end": TrajectoryDigest.totals(sim),
		"harvest_credit_rate": _harvest_credit_rate(sim),
		"monetary": _monetary_report(sim),
		"wall_s": (Time.get_ticks_msec() - wall_start) / 1000.0,
	}
	if _fx_seed_days > 0:
		out["fx_scenario"] = _fx_report(sim, _fx_seed_days)
	if _credit:
		out["credit"] = _credit_report(sim)
	var file := FileAccess.open(_out, FileAccess.WRITE)
	if file == null:
		print("[c1] FALHA: não escreveu " + _out)
	else:
		file.store_string(JSON.stringify(out, "  ", true))
		file.close()
	print("[c1] dias=%d falências=%d violações=%d travados=%d drift=%.6f "
		% [_days, int(out["bankruptcies"]), violation_count, stalled,
			float(out["drift"])]
		+ "portão=%d/%d mediana=%.4f trajectory=%s"
		% [int(food["below_gate"]), int(food["nations"]),
			float(food["median"]), String(out["trajectory_sha256"]).substr(0, 8)])
	print("[c1] balanço: financeiro=%.6f caixa_residual=%.6f PL_famílias=%.0f "
		% [float((balance["invariant"] as Dictionary).get("financial_sum", 0.0)),
			float((balance["invariant"] as Dictionary).get("cash_residual", 0.0)),
			float((balance["world"] as Dictionary).get("families_net_worth", 0.0))]
		+ "atrasado=%.0f saída=" % float((balance["invariant"] as Dictionary)
			.get("receivables", 0.0)) + _out)
	print("[c1] OK: 365 d medidos")
	if _credit:
		var credit := _credit_report(sim)
		print("[c1] crédito (C3): ligado=%s empréstimos=%d dívida=%.3f "
			% [str(credit["enabled"]), int(credit["loans_count"]),
				float(credit["group_debt"])]
			+ "calotes=%d baixas=%.3f patrimônio_banco=%.3f"
			% [int(credit["defaults"]), float(credit["writeoffs"]),
				float(credit["equity"])])
	quit(0)


func _credit_report(sim) -> Dictionary:
	# C3 (D-107): o crédito de empresa medido no fim do mundo. Default do jogo
	# continua DESLIGADO; este bloco só existe na rodada de laboratório.
	var bank: Dictionary = sim.get_bank_status()
	var totals: Dictionary = bank.get("totals", {})
	return {
		"enabled": bool((bank.get("policy", {}) as Dictionary).get(
			"enabled", false)),
		"group_debt": float(totals.get("group_debt", 0.0)),
		"loans": float(totals.get("loans", 0.0)),
		"bc_borrowing": float(totals.get("bc_borrowing", 0.0)),
		"capital": float(totals.get("capital", 0.0)),
		"equity": float(totals.get("equity", 0.0)),
		"loans_count": int(totals.get("loans_count", 0)),
		"defaults": int(totals.get("defaults", 0)),
		"writeoffs": float(totals.get("writeoffs", 0.0)),
	}
