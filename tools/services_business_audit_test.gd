extends SceneTree

## Auditoria dos NEGÓCIOS DE SERVIÇO (F8) AO LONGO DO TEMPO — não altera o núcleo.
##
## Roda o MUNDO REAL (cities/ports/buildings.json) por 90 dias com a IA
## econômica ligada (mesmo pipeline do jogo) e mede, por setor (20) e por nação
## âncora (USA, DEU, BRA, IND, NGA, CHN):
##
##   1. negócios abertos / fechados por mês;
##   2. distribuição de empregados por negócio (min/mediana/max);
##   3. lucro diário (min/mediana/max) — deve variar e ter positivos e negativos;
##   4. receita vs. custos (folha + insumos de bens + insumos de serviço);
##   5. falências de serviço (cumulativo e por mês);
##   6. demanda por serviço atendida (razão servida e fração não atendida).
##
## A observação original: "no dia 1 todos os negócios aparecem com 40 empregados
## e lucro 0,0". O teste separa o retrato do DIA 0 (semente, antes do 1º tick) do
## comportamento APÓS os ticks, para distinguir artefato de captura de bug real.
##
##   godot --headless --path . --script res://tools/services_business_audit_test.gd
##     [--days=90] [--out=res://docs/economy/services_business_audit.json]
##
## Saída: relatório em JSON + resumo no stdout. O veredito do teste é um GUARDA
## de saúde (dinheiro conservado, sem NaN, negócios existem); os ACHADOS de
## auditoria (ex.: "empregados travados em 40") são reportados no campo
## `findings` sem derrubar o teste — a correção é do gerente-simulacao.

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const EconomicAIScript := preload("res://core/economy/ai/economic_ai.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"

const DEFAULT_DAYS := 90
const MONTH_DAYS := 30
## Nações âncora da auditoria (mesmo conjunto da F9).
const FOCUS_TAGS := ["USA", "DEU", "BRA", "IND", "NGA", "CHN"]
## Valor de empregados observado na captura do dia 1 (o "bug" a investigar).
const SUSPECT_JOBS := 40.0
## Fração de negócios com exatamente SUSPECT_JOBS postos a partir da qual o
## teto rígido é considerado achado de bug.
const STUCK_SHARE := 0.8
## Fração de negócio-dias com lucro negativo a partir da qual a perda é
## considerada estrutural (não mero ruído de mercado).
const NEGATIVE_SHARE := 0.9
## Fração de setor-dias com demanda não atendida a partir da qual a oferta é
## considerada insuficiente.
const UNSERVED_SHARE := 0.5
## Razão falências/negócios ativos a partir da qual o churn é considerado bug.
const CHURN_RATIO := 0.5
## Tolerância do drift de dinheiro (mesma do teste de mundo).
const MONEY_DRIFT_TOLERANCE := 0.01

var _days := DEFAULT_DAYS
var _out := "res://docs/economy/services_business_audit.json"
var _sector_data: Dictionary = {}

var _failures: Array[String] = []
var _findings: Array[Dictionary] = []

# --- acumuladores -----------------------------------------------------------
var _prev_ids: Dictionary = {}          # id -> {nation, sector} (último dia visto)
var _jobs_seen: Dictionary = {}         # id -> true (registro único de postos)
var _opened: Dictionary = {}            # mês -> nação -> setor -> contagem
var _closed: Dictionary = {}            # mês -> nação -> setor -> contagem
var _bankruptcies_by_month: Array = [0, 0, 0]
var _bankruptcies_prev := 0

var _seed_jobs: Array = []              # postos no dia 0 (semente)
var _seed_profit: Array = []            # lucro no dia 0 (semente)

var _jobs_values: Array = []            # postos por negócio (únicos)
var _workers_values: Array = []         # trabalhadores efetivos por negócio/dia
var _profit_values: Array = []          # lucro por negócio/dia
var _profit_pos := 0
var _profit_neg := 0
var _profit_zero := 0

var _revenue_total := 0.0
var _wages_total := 0.0
var _input_cost_total := 0.0
var _service_cost_total := 0.0

var _demand_samples := 0
var _demand_unmet_positive := 0
var _demand_served_sum := 0.0

var _nation: Dictionary = {}            # tag -> agg
var _sector: Dictionary = {}            # setor -> agg


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = maxi(1, int(arg.split("=")[1]))
		elif arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
	_run_audit()
	if _failures.is_empty():
		print("[services-audit] OK: mundo real rodou %d dias com IA sem violação de saúde." % _days)
		quit(0)
	else:
		print("[services-audit] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _run_audit() -> void:
	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	var goods := _load_array(GOODS_PATH)
	if cities.is_empty() or buildings.is_empty() or goods.is_empty():
		_fail("cenário vazio (cidades=%d, construções=%d, bens=%d)" % [
			cities.size(), buildings.size(), goods.size()])
		_finish_report({})
		return

	var sim = EconomySim.new()
	sim.demo_set_goods(goods)
	sim.demo_set_recipes(_load_array(RECIPES_PATH))
	sim.demo_set_needs(_load_dict(NEEDS_PATH))
	sim.demo_set_services(_load_dict(SERVICES_PATH))
	sim.demo_set_currencies(_load_dict(CURRENCIES_PATH))
	_sector_data = _load_dict(SECTOR_EMPLOYMENT_PATH)
	if not _sector_data.is_empty():
		sim.demo_set_sector_employment(_sector_data)

	var geo := GeographyServiceScript.new()
	geo.load_data()
	buildings = geo.resolve_initial_farms(buildings, cities)
	sim.initialize_demo_world(cities, ports, buildings)
	geo.apply_to_sim(sim, _cities_with_center(cities))
	var ai := EconomicAIScript.new()
	ai.setup(sim, geo)

	var status: Dictionary = sim.get_demo_status()
	var money_initial := float(status.get("money_total", 0.0))

	# Retrato do DIA 0 (semente, antes de qualquer tick): é aqui que a captura
	# "40 empregados / lucro 0,0" do dia 1 se materializa.
	_snapshot_seed(sim, status)

	# Loop de 90 dias com IA (mesmo pipeline do jogo ao vivo).
	for day in _days:
		status = sim.tick_demo()
		ai.run(int(status.get("day", day + 1)), status)
		_collect_day(sim, status, day)

	var money_final := float(status.get("money_total", 0.0))
	var drift := money_final - money_initial
	print("[services-audit] dinheiro inicial=%.2f final=%.2f drift=%.4f" % [
		money_initial, money_final, drift])
	if absf(drift) > MONEY_DRIFT_TOLERANCE:
		_fail("dinheiro não conservado em %d dias: %.2f -> %.2f (drift %.4f)" % [
			_days, money_initial, money_final, drift])

	var bankruptcies := int(sim.demo_get_service_bankruptcies())
	var business_count := int(sim.demo_get_service_business_count())
	if business_count <= 0:
		_fail("nenhum negócio de serviço no mundo após %d dias" % _days)

	_finish_report({
		"money_initial": money_initial,
		"money_final": money_final,
		"drift": drift,
		"service_businesses": business_count,
		"service_bankruptcies": bankruptcies,
	})


# --- coleta -----------------------------------------------------------------

## Retrato do dia 0: registra postos e lucro da SEMENTE (antes do 1º tick) e
## inicializa o conjunto de ids já existentes (para não contar a semente como
## "abertura" no mês 1).
func _snapshot_seed(sim: Object, status: Dictionary) -> void:
	for city in status.get("cities", []):
		var tag := String(city.get("nation", ""))
		if tag not in FOCUS_TAGS:
			continue
		var detail: Dictionary = sim.demo_get_services_detail(String(city.get("id", "")))
		for business in detail.get("businesses", []):
			var id := int(business["id"])
			var sector := String(business["sector"])
			var jobs := float(business["jobs"])
			_prev_ids[id] = {"nation": tag, "sector": sector}
			_jobs_seen[id] = true
			_jobs_values.append(jobs)
			_seed_jobs.append(jobs)
			_seed_profit.append(float(business["profit"]))
			_append_value(_nation, tag, "jobs", jobs)
			_append_value(_sector, sector, "jobs", jobs)


func _collect_day(sim: Object, status: Dictionary, day: int) -> void:
	var month := mini(day / MONTH_DAYS, _bankruptcies_by_month.size() - 1)
	var today: Dictionary = {}
	for city in status.get("cities", []):
		var tag := String(city.get("nation", ""))
		if tag not in FOCUS_TAGS:
			continue
		var detail: Dictionary = sim.demo_get_services_detail(String(city.get("id", "")))
		for business in detail.get("businesses", []):
			var id := int(business["id"])
			var sector := String(business["sector"])
			today[id] = {"nation": tag, "sector": sector}
			if not _jobs_seen.has(id):
				_jobs_seen[id] = true
				var jobs := float(business["jobs"])
				_jobs_values.append(jobs)
				_append_value(_nation, tag, "jobs", jobs)
				_append_value(_sector, sector, "jobs", jobs)
			var profit := float(business["profit"])
			var revenue := float(business["revenue"])
			var wages := float(business["wages"])
			var input_cost := float(business["input_cost"])
			var service_cost := float(business["service_cost"])
			if not is_finite(profit) or not is_finite(revenue) or not is_finite(wages):
				_fail("negócio %d: valor não finito no dia %d" % [id, day + 1])
			_profit_values.append(profit)
			_workers_values.append(float(business["workers"]))
			if profit > 0.0:
				_profit_pos += 1
			elif profit < 0.0:
				_profit_neg += 1
			else:
				_profit_zero += 1
			_revenue_total += revenue
			_wages_total += wages
			_input_cost_total += input_cost
			_service_cost_total += service_cost
			_accumulate(_nation, tag, revenue, wages, input_cost, service_cost)
			_accumulate(_sector, sector, revenue, wages, input_cost, service_cost)
			_append_value(_nation, tag, "profit", profit)
			_append_value(_sector, sector, "profit", profit)
		for sector in detail.get("sectors", []):
			var demand := float(sector["demand"])
			var unmet := float(sector["unmet"])
			if demand <= 1e-9:
				continue
			var served := (demand - unmet) / demand
			_demand_samples += 1
			_demand_served_sum += served
			if unmet > 1e-9:
				_demand_unmet_positive += 1
			_append_value(_nation, tag, "served", served)
			_append_value(_sector, String(sector["id"]), "served", served)
			_bump_int(_nation, tag, "demand_samples", 1)
			_bump_int(_sector, String(sector["id"]), "demand_samples", 1)
			if unmet > 1e-9:
				_bump_int(_nation, tag, "unmet_positive", 1)
				_bump_int(_sector, String(sector["id"]), "unmet_positive", 1)

	# Abertos = hoje - ontem; fechados = ontem - hoje.
	for id in today:
		if not _prev_ids.has(id):
			var info: Dictionary = today[id]
			_bump(_opened, month, String(info["nation"]), String(info["sector"]))
			_bump_int(_nation, String(info["nation"]), "openings", 1)
			_bump_int(_sector, String(info["sector"]), "openings", 1)
	for id in _prev_ids:
		if not today.has(id):
			var info: Dictionary = _prev_ids[id]
			_bump(_closed, month, String(info["nation"]), String(info["sector"]))
			_bump_int(_nation, String(info["nation"]), "closings", 1)
			_bump_int(_sector, String(info["sector"]), "closings", 1)
	_prev_ids = today

	# Falências (cumulativo -> delta do mês).
	var bankruptcies := int(sim.demo_get_service_bankruptcies())
	var delta := bankruptcies - _bankruptcies_prev
	_bankruptcies_prev = bankruptcies
	_bankruptcies_by_month[month] = int(_bankruptcies_by_month[month]) + delta


func _append_value(agg: Dictionary, key: String, field: String, value: float) -> void:
	var row: Dictionary = agg.get(key, {})
	var values: Array = row.get(field, [])
	values.append(value)
	row[field] = values
	agg[key] = row


func _accumulate(agg: Dictionary, key: String, revenue: float, wages: float,
		input_cost: float, service_cost: float) -> void:
	var row: Dictionary = agg.get(key, {})
	row["revenue"] = float(row.get("revenue", 0.0)) + revenue
	row["wages"] = float(row.get("wages", 0.0)) + wages
	row["input_cost"] = float(row.get("input_cost", 0.0)) + input_cost
	row["service_cost"] = float(row.get("service_cost", 0.0)) + service_cost
	agg[key] = row


func _bump_int(agg: Dictionary, key: String, field: String, amount: int) -> void:
	var row: Dictionary = agg.get(key, {})
	row[field] = int(row.get(field, 0)) + amount
	agg[key] = row


func _bump(d: Dictionary, k1, k2: String, k3: String) -> void:
	if not d.has(k1):
		d[k1] = {}
	var d1: Dictionary = d[k1]
	if not d1.has(k2):
		d1[k2] = {}
	var d2: Dictionary = d1[k2]
	d2[k3] = int(d2.get(k3, 0)) + 1


# --- relatório --------------------------------------------------------------

func _finish_report(meta: Dictionary) -> void:
	var employees := _quantiles(_jobs_values)
	var workers := _quantiles(_workers_values)
	var profit := _quantiles(_profit_values)
	var total_cost := _wages_total + _input_cost_total + _service_cost_total

	# Achado 1: empregados travados em 40 (o bug da captura do dia 1).
	var equal_40 := 0
	for value in _jobs_values:
		if absf(value - SUSPECT_JOBS) < 1e-6:
			equal_40 += 1
	var equal_40_share := float(equal_40) / float(maxi(1, _jobs_values.size()))
	if employees["count"] > 0 and equal_40_share >= STUCK_SHARE:
		_findings.append({
			"id": "employees_stuck_at_40",
			"severity": "bug",
			"text": "%.1f%% dos negócios têm exatamente %d postos (min=%.1f mediana=%.1f max=%.1f) — a distribuição de empregados não varia; o teto rígido vem de `40.0/jobs_per_unit` no núcleo (demo_seed_services e entrada por demanda). Trabalhadores efetivos (workers) variam com a utilização: min=%.1f mediana=%.1f max=%.1f." % [
				equal_40_share * 100.0, int(SUSPECT_JOBS),
				float(employees["min"]), float(employees["median"]), float(employees["max"]),
				float(workers["min"]), float(workers["median"]), float(workers["max"])],
		})
	# Achado 2: lucro 0,0 no dia 0 (semente), mas varia após os ticks.
	var seed_profit := _quantiles(_seed_profit)
	if seed_profit["count"] > 0 and absf(seed_profit["max"]) < 1e-9 \
			and absf(seed_profit["min"]) < 1e-9:
		_findings.append({
			"id": "seed_profit_zero",
			"severity": "info",
			"text": "no dia 0 (semente) o lucro de todos os %d negócios é 0,0 — artefato de captura: o lucro só é calculado no 1º tick (demo_services_day)." % int(seed_profit["count"]),
		})
	# Achado 3: lucro deve variar (positivo e negativo) ao longo do tempo.
	if _profit_pos == 0 and _profit_neg == 0:
		_findings.append({
			"id": "profit_never_computed",
			"severity": "bug",
			"text": "lucro nunca saiu de 0 em %d ticks — serviços não estão sendo contabilizados." % _days,
		})
	elif _profit_neg == 0:
		_findings.append({
			"id": "profit_no_negatives",
			"severity": "info",
			"text": "nenhum lucro negativo em %d ticks (pos=%d zero=%d) — todos os negócios lucram, o que pode esconder margem superestimada." % [
				_days, _profit_pos, _profit_zero],
		})
	# Achado 4: lucro esmagadoramente negativo (perda estrutural).
	var profit_total := _profit_pos + _profit_neg + _profit_zero
	if profit_total > 0 and float(_profit_neg) / float(profit_total) > NEGATIVE_SHARE:
		_findings.append({
			"id": "profit_overwhelmingly_negative",
			"severity": "bug",
			"text": "%.1f%% dos negócio-dias têm lucro negativo (pos=%d neg=%d zero=%d) — os negócios de serviço operam estruturalmente no prejuízo." % [
				float(_profit_neg) / float(profit_total) * 100.0,
				_profit_pos, _profit_neg, _profit_zero],
		})

	var served_mean := _demand_served_sum / float(maxi(1, _demand_samples))
	var unmet_share := float(_demand_unmet_positive) / float(maxi(1, _demand_samples))
	# Achado 5: demanda majoritariamente não atendida.
	if _demand_samples > 0 and unmet_share > UNSERVED_SHARE:
		_findings.append({
			"id": "demand_largely_unserved",
			"severity": "bug",
			"text": "%.1f%% dos setor-dias têm demanda não atendida e a fração média atendida é %.1f%% — a oferta de serviços não serve a demanda." % [
				unmet_share * 100.0, served_mean * 100.0],
		})
	# Achado 6: churn de falências (abrir → 60 dias de prejuízo → falir → reabrir).
	var bankruptcies := int(meta.get("service_bankruptcies", 0))
	var businesses := int(meta.get("service_businesses", 0))
	if businesses > 0 and float(bankruptcies) / float(businesses) > CHURN_RATIO:
		_findings.append({
			"id": "service_bankruptcy_churn",
			"severity": "bug",
			"text": "%d falências para %d negócios ativos em %d dias (%.2fx) — ciclo contínuo de abrir → 60 dias de prejuízo → falir → reabrir." % [
				bankruptcies, businesses, _days, float(bankruptcies) / float(businesses)],
		})

	print("[services-audit] empregados(posto)/negócio: min=%.1f mediana=%.1f max=%.1f (n=%d) | ==%d: %.1f%%" % [
		float(employees["min"]), float(employees["median"]), float(employees["max"]),
		int(employees["count"]), int(SUSPECT_JOBS), equal_40_share * 100.0])
	print("[services-audit] trabalhadores efetivos: min=%.1f mediana=%.1f max=%.1f" % [
		float(workers["min"]), float(workers["median"]), float(workers["max"])])
	print("[services-audit] lucro/dia: min=%.3f mediana=%.3f max=%.3f | pos=%d neg=%d zero=%d" % [
		float(profit["min"]), float(profit["median"]), float(profit["max"]),
		_profit_pos, _profit_neg, _profit_zero])
	print("[services-audit] receita vs custos: receita=%.2f folha=%.2f insumos_bens=%.2f insumos_serv=%.2f custo_total=%.2f (margem %.2f%%)" % [
		_revenue_total, _wages_total, _input_cost_total, _service_cost_total,
		total_cost, _margin_pct(_revenue_total, total_cost)])
	print("[services-audit] demanda: amostras=%d servida_média=%.3f não_atendida=%.1f%%" % [
		_demand_samples, served_mean, unmet_share * 100.0])
	print("[services-audit] abertos/mês: %s | fechados/mês: %s | falências/mês: %s" % [
		_json(_opened), _json(_closed), _json(_bankruptcies_by_month)])

	for tag in FOCUS_TAGS:
		if not _nation.has(tag):
			continue
		_printf_row("nação", tag, _nation[tag])
	for sector in _sector:
		_printf_row("setor", sector, _sector[sector])

	_write_report(meta, employees, workers, profit, total_cost, served_mean,
		unmet_share, equal_40_share, seed_profit)

	if not _findings.is_empty():
		print("[services-audit] ACHADOS (%d):" % _findings.size())
		for finding in _findings:
			print("  [%s] %s" % [String(finding["severity"]), String(finding["id"])])


func _printf_row(kind: String, key: String, row: Dictionary) -> void:
	var jobs := _quantiles(row.get("jobs", []))
	var profit := _quantiles(row.get("profit", []))
	var revenue := float(row.get("revenue", 0.0))
	var cost := float(row.get("wages", 0.0)) + float(row.get("input_cost", 0.0)) \
		+ float(row.get("service_cost", 0.0))
	var samples := int(row.get("demand_samples", 0))
	var unmet_pos := int(row.get("unmet_positive", 0))
	var served := 0.0
	if samples > 0:
		served = _sum(row.get("served", [])) / float(samples)
	print("[services-audit]   %-5s %-22s empregos[min %.1f med %.1f max %.1f] lucro[min %.3f med %.3f max %.3f] receita=%.2f custo=%.2f margem=%.1f%% abertos=%d fechados=%d servida=%.3f não_atendida=%.1f%%" % [
		kind, key,
		float(jobs["min"]), float(jobs["median"]), float(jobs["max"]),
		float(profit["min"]), float(profit["median"]), float(profit["max"]),
		revenue, cost, _margin_pct(revenue, cost),
		int(row.get("openings", 0)), int(row.get("closings", 0)),
		served, (float(unmet_pos) / float(maxi(1, samples))) * 100.0])


func _write_report(meta: Dictionary, employees: Dictionary, workers: Dictionary,
		profit: Dictionary, total_cost: float, served_mean: float, unmet_share: float,
		equal_40_share: float, seed_profit: Dictionary) -> void:
	var dir := _out.get_base_dir()
	if dir != "" and not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var file := FileAccess.open(_out, FileAccess.WRITE)
	if file == null:
		_fail("não abriu para escrita: " + _out)
		return
	var report := {
		"meta": {
			"days": _days,
			"focus_nations": FOCUS_TAGS,
			"money_initial": float(meta.get("money_initial", 0.0)),
			"money_final": float(meta.get("money_final", 0.0)),
			"drift": float(meta.get("drift", 0.0)),
			"service_businesses": int(meta.get("service_businesses", 0)),
			"service_bankruptcies": int(meta.get("service_bankruptcies", 0)),
		},
		"employees_per_business": employees,
		"employees_equal_40": {
			"target": int(SUSPECT_JOBS),
			"share": equal_40_share,
		},
		"workers_effective": workers,
		"seed_day0": {
			"jobs": _quantiles(_seed_jobs),
			"profit": seed_profit,
		},
		"profit_daily": {
			"min": float(profit["min"]),
			"median": float(profit["median"]),
			"max": float(profit["max"]),
			"positive": _profit_pos,
			"negative": _profit_neg,
			"zero": _profit_zero,
		},
		"revenue_vs_costs": {
			"revenue": _revenue_total,
			"wages": _wages_total,
			"input_cost": _input_cost_total,
			"service_cost": _service_cost_total,
			"total_cost": total_cost,
		},
		"demand_served": {
			"samples": _demand_samples,
			"served_mean": served_mean,
			"unmet_positive": _demand_unmet_positive,
			"unmet_share": unmet_share,
		},
		"openings_by_month": _strip_month(_opened),
		"closings_by_month": _strip_month(_closed),
		"bankruptcies_by_month": _bankruptcies_by_month,
		"by_nation": _strip_agg(_nation),
		"by_sector": _strip_agg(_sector),
		"findings": _findings,
	}
	file.store_string(JSON.stringify(report, "  "))
	file.close()
	print("[services-audit] escrito %s" % _out)


func _strip_agg(agg: Dictionary) -> Dictionary:
	var out: Dictionary = {}
	for key in agg:
		var row: Dictionary = agg[key]
		var revenue := float(row.get("revenue", 0.0))
		var cost := float(row.get("wages", 0.0)) + float(row.get("input_cost", 0.0)) \
			+ float(row.get("service_cost", 0.0))
		out[key] = {
			"jobs": _quantiles(row.get("jobs", [])),
			"profit": _quantiles(row.get("profit", [])),
			"revenue": revenue,
			"wages": float(row.get("wages", 0.0)),
			"input_cost": float(row.get("input_cost", 0.0)),
			"service_cost": float(row.get("service_cost", 0.0)),
			"total_cost": cost,
			"margin_pct": _margin_pct(revenue, cost),
			"openings": int(row.get("openings", 0)),
			"closings": int(row.get("closings", 0)),
			"demand_samples": int(row.get("demand_samples", 0)),
			"unmet_positive": int(row.get("unmet_positive", 0)),
		}
	return out


func _strip_month(d: Dictionary) -> Array:
	var out: Array = []
	for key in d:
		out.append({"month": int(key), "data": d[key]})
	out.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return int(a["month"]) < int(b["month"]))
	return out


# --- apoio ------------------------------------------------------------------

func _quantiles(values: Array) -> Dictionary:
	if values.is_empty():
		return {"min": 0.0, "median": 0.0, "max": 0.0, "count": 0}
	var sorted := values.duplicate()
	sorted.sort()
	return {
		"min": float(sorted[0]),
		"median": float(sorted[sorted.size() / 2]),
		"max": float(sorted[sorted.size() - 1]),
		"count": sorted.size(),
	}


func _sum(values: Array) -> float:
	var total := 0.0
	for value in values:
		total += float(value)
	return total


func _margin_pct(revenue: float, cost: float) -> float:
	if revenue <= 1e-9:
		return 0.0
	return (revenue - cost) / revenue * 100.0


func _cities_with_center(cities: Array) -> Array:
	var result: Array = []
	for city in cities:
		result.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
		})
	return result


func _fail(message: String) -> void:
	_failures.append(message)
	print("[services-audit] FALHA: " + message)


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_DICTIONARY else {}


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _json(value: Variant) -> String:
	return JSON.stringify(value)
