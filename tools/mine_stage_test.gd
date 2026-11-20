extends SceneTree

## D-086 — ESTÁGIO DE MINA: o recurso NUNCA acaba (decisão do dono, 2026-11-17).
##
## Mundo sintético e determinístico (catálogo REAL de bens). Cobre o aceite da
## nova regra:
##   (a) aprofundar NUNCA é bloqueado por reserva — mesmo depois de extrair
##       além da reserva de referência da jazida; só fica mais caro;
##   (b) o custo do estágio n+1 é maior que o do estágio n para vários n e o
##       crescimento é CONVEXO (curva geométrica declarada no núcleo);
##   (c) com duas minas do mesmo mineral e custos de próximo estágio
##       diferentes, a IA (governo) escolhe a mais barata;
##   (d) o dinheiro é conservado e o save/load (v17, sem mudança de formato)
##       continua preservando o estágio; save v16 migra para o último estágio.
##   mais: a produção da mina no piso NÃO zera quando a reserva de referência
##   acaba, e nenhuma jazida é publicada como esgotada.
##
## Uso: godot --headless --path . --script res://tools/mine_stage_test.gd

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const GEOGRAPHY_PATH := "res://core/economy/geography_service.gd"
const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const SEED_PATH := "res://data/definitions/natural/extraction_2025.json"
## Produção real atribuída (t/dia) e dono das três entradas da semente
## conferidas abaixo. Valores lidos de `extraction_2025.json` (fonte: USGS
## MCS 2025 / Energy Institute 2025 via OWID), não estimativa do teste.
const SEED_CASES := {
	"CAN-EDM|oil": 166.5586,
	"CHN-XIAN|coal": 107.8395,
	"AUS-PER|bauxite": 684.5264,
}
## Índice do ferro no catálogo (order 1 em goods.json).
const IRON := 1
## Piso documentado do estágio esgotado (kMineStageFloor do núcleo).
const STAGE_FLOOR := 0.05
## Estágio das minas históricas (kMineInitialStage do núcleo).
const INITIAL_STAGE := 3
## Prazo da obra de aprofundamento (kMineDeepenBaseDays + 30 × estágio).
const DEEPEN_BASE_DAYS := 90
const DEEPEN_DAYS_PER_STAGE := 30
## Fator de capacidade por aprofundamento (kMineDeepenCapacityFactor).
const CAPACITY_FACTOR := 1.5
## Base do crescimento geométrico do custo (kMineDeepenStageCostFactor).
## Só é usada aqui como checagem de ordem de grandeza; os valores cobrados vêm
## do status do núcleo.
const STAGE_COST_FACTOR := 1.25
## Estágios extras medidos na checagem de convexidade.
const CURVE_STEPS := 5
const MONEY_EPSILON := 0.5

var _failures: Array[String] = []


func _init() -> void:
	_test_never_blocked()
	_test_cost_curve()
	_test_ai_picks_cheaper()
	_test_money_and_save()
	_test_seeded_real()
	_finish()


# ---------------------------------------------------------------------------
# (a) A reserva de referência acaba e o aprofundamento continua aceito.
# ---------------------------------------------------------------------------
func _test_never_blocked() -> void:
	var failures_before := _failures.size()
	var sim = _make_sim()
	_boost_budget(sim, "MINE-B", 1.0e9)
	var info: Dictionary = sim.get_city_status("MINE-B")
	var mine := _find_building(info, "MINE-B-IRON")
	if mine.is_empty():
		_fail("(a) mina MINE-B-IRON ausente do status")
		return
	if int(mine.get("stage", -1)) != INITIAL_STAGE:
		_fail("(a) estágio inicial %d != %d" % [
			int(mine.get("stage", -1)), INITIAL_STAGE])
	if absf(float(mine.get("stage_factor", 0.0)) - STAGE_FLOOR) > 1e-9:
		_fail("(a) produção relativa %.4f != piso %.2f" % [
			float(mine.get("stage_factor", 0.0)), STAGE_FLOOR])
	var cost_zero := float(mine.get("next_deepen_cost", 0.0))
	if cost_zero <= 0.0:
		_fail("(a) custo do próximo aprofundamento não publicado")
	var deposit_before: Dictionary = info["deposits"]["iron"]
	var reference_kt := float(deposit_before.get("initial_kt", 0.0))
	if reference_kt <= 0.0:
		_fail("(a) jazida de referência sem initial_kt")

	# Capacidade 1.000 t/dia no piso = 50 t/dia: a jazida de 0,05 kt (50 t)
	# acaba no 1º dia e a extração SEGUE nos dias seguintes.
	var last_produced := 0.0
	for day in range(1, 7):
		var status: Dictionary = sim.tick_day()
		_check(status, day)
		last_produced = float(status["produced_today"][IRON])
		if last_produced <= 0.0:
			_fail("(a) dia %d: mina no piso zerou a produção (%.3f t)" % [
				day, last_produced])
		if day >= 2 and last_produced < 49.0:
			_fail("(a) dia %d: produção após a reserva de referência %.2f t (< 50)" % [
				day, last_produced])

	info = sim.get_city_status("MINE-B")
	var deposit: Dictionary = info["deposits"]["iron"]
	if float(deposit.get("remaining_kt", -1.0)) > 1e-9:
		_fail("(a) reserva restante %.6f deveria ser 0" % float(
			deposit.get("remaining_kt", -1.0)))
	if float(deposit.get("extracted_kt", 0.0)) <= reference_kt:
		_fail("(a) extração %.4f kt não passou da reserva de referência %.4f kt" % [
			float(deposit.get("extracted_kt", 0.0)), reference_kt])
	if bool(deposit.get("depleted", true)):
		_fail("(a) jazida publicada como esgotada (o recurso nunca acaba)")
	if int(sim.get_status().get("depleted_mines", -1)) != 0:
		_fail("(a) depleted_mines != 0 com a nova regra")
	mine = _find_building(info, "MINE-B-IRON")
	var cost_after := float(mine.get("next_deepen_cost", 0.0))
	if cost_after <= cost_zero:
		_fail("(a) custo não subiu com a extração: %.2f -> %.2f" % [
			cost_zero, cost_after])
	if cost_after < cost_zero * 2.0:
		_fail("(a) extração além da referência deveria encarecer ao menos 2x (%.2f -> %.2f)" % [
			cost_zero, cost_after])

	var deepen: Dictionary = sim.deepen_mine("MINE-B", "MINE-B-IRON")
	if not bool(deepen.get("started", false)):
		_fail("(a) aprofundamento BLOQUEADO após a reserva de referência: '%s'" % String(
			deepen.get("error", "")))
		return
	if int(deepen.get("stage", 0)) != INITIAL_STAGE + 1:
		_fail("(a) estágio aberto %d != %d" % [
			int(deepen.get("stage", 0)), INITIAL_STAGE + 1])
	if _failures.size() == failures_before:
		print("[mine] (a) reserva de referência esgotada: extração continuou (%.2f t/dia) e aprofundar foi aceito (custo %.0f -> %.0f)" % [
			last_produced, cost_zero, cost_after])


# ---------------------------------------------------------------------------
# (b) Custo crescente e convexo entre estágios consecutivos.
# ---------------------------------------------------------------------------
func _test_cost_curve() -> void:
	var sim = _make_sim()
	sim.set_instant_construction(true)
	_boost_budget(sim, "MINE-A", 1.0e9)
	var failures_before := _failures.size()
	var costs: Array[float] = []
	var durations: Array[int] = []
	for step in range(CURVE_STEPS):
		var info: Dictionary = sim.get_city_status("MINE-A")
		var mine := _find_building(info, "MINE-A-IRON")
		if mine.is_empty():
			_fail("(b) mina MINE-A-IRON ausente no passo %d" % step)
			return
		costs.append(float(mine.get("next_deepen_cost", 0.0)))
		var deepen: Dictionary = sim.deepen_mine("MINE-A", "MINE-A-IRON")
		if not bool(deepen.get("started", false)):
			_fail("(b) passo %d: aprofundamento recusado: '%s'" % [
				step, String(deepen.get("error", ""))])
			return
		durations.append(int(deepen.get("duration_days", 0)))
		if absf(float(deepen.get("cost", 0.0)) - costs[step]) > 0.01:
			_fail("(b) passo %d: custo cobrado %.2f != publicado %.2f" % [
				step, float(deepen.get("cost", 0.0)), costs[step]])
		if int(deepen.get("duration_days", 0)) != DEEPEN_BASE_DAYS \
				+ DEEPEN_DAYS_PER_STAGE * (INITIAL_STAGE + 1 + step):
			_fail("(b) passo %d: prazo %d inesperado" % [
				step, int(deepen.get("duration_days", 0))])
		if int(deepen.get("stage", 0)) != INITIAL_STAGE + 1 + step:
			_fail("(b) passo %d: estágio %d inesperado" % [
				step, int(deepen.get("stage", 0))])

	for step in range(1, costs.size()):
		if costs[step] <= costs[step - 1]:
			_fail("(b) custo do estágio %d (%.0f) <= estágio %d (%.0f)" % [
				INITIAL_STAGE + 1 + step, costs[step],
				INITIAL_STAGE + step, costs[step - 1]])
	# Convexo: a diferença entre passos consecutivos também cresce.
	for step in range(1, costs.size() - 1):
		var previous_delta := costs[step] - costs[step - 1]
		var delta := costs[step + 1] - costs[step]
		if delta <= previous_delta:
			_fail("(b) curva não convexa no passo %d: Δ%.0f <= Δ%.0f" % [
				step, delta, previous_delta])
	var last_ratio := costs[costs.size() - 1] / costs[0]
	var factor_lower := pow(STAGE_COST_FACTOR, CURVE_STEPS - 1) * 0.9
	if last_ratio < factor_lower:
		_fail("(b) crescimento %.2fx abaixo da curva declarada (%.2fx)" % [
			last_ratio, factor_lower])
	if durations[1] - durations[0] != DEEPEN_DAYS_PER_STAGE:
		_fail("(b) prazo não cresce %d dia por estágio" % DEEPEN_DAYS_PER_STAGE)
	if _failures.size() == failures_before:
		print("[mine] (b) custo convexo: " + "%s" % str(costs))


# ---------------------------------------------------------------------------
# (c) A IA escolhe a mina mais barata entre duas do mesmo mineral.
# ---------------------------------------------------------------------------
func _test_ai_picks_cheaper() -> void:
	var failures_before := _failures.size()
	var sim = _make_ai_sim()
	var cheap_before := _find_building(sim.get_city_status("AI-CITY"), "AI-CHEAP-IRON")
	var dear_before := _find_building(sim.get_city_status("AI-CITY"), "AI-DEAR-IRON")
	var cheap_cost := float(cheap_before.get("next_deepen_cost", 0.0))
	var dear_cost := float(dear_before.get("next_deepen_cost", 0.0))
	if cheap_cost <= 0.0 or dear_cost <= 0.0:
		_fail("(c) custos de aprofundamento não publicados (%.2f, %.2f)" % [
			cheap_cost, dear_cost])
		return
	if cheap_cost >= dear_cost:
		_fail("(c) cenário inválido: mina 'barata' %.0f >= 'cara' %.0f" % [
			cheap_cost, dear_cost])
		return
	# A IA de uma cidade roda a cada 7 dias (cadência A6) — 8 dias cobrem o dia 7.
	for day in range(1, 9):
		_check(sim.tick_day(), day)
	var info: Dictionary = sim.get_city_status("AI-CITY")
	var cheap := _find_building(info, "AI-CHEAP-IRON")
	var dear := _find_building(info, "AI-DEAR-IRON")
	var targeted_cheap := false
	var targeted_dear := false
	for entry: Dictionary in sim.get_command_log():
		if String(entry.get("type", "")) != "deepen_mine":
			continue
		var slots: Array = entry.get("s", [])
		if slots.size() < 2:
			continue
		var building := String(slots[1])
		if building == "AI-CHEAP-IRON":
			targeted_cheap = true
		elif building == "AI-DEAR-IRON":
			targeted_dear = true
	if not targeted_cheap:
		_fail("(c) IA não aprofundou a mina mais barata (custo %.0f)" % cheap_cost)
	if targeted_dear:
		_fail("(c) IA emitiu aprofundamento da mina cara (custo %.0f)" % dear_cost)
	if int(cheap.get("stage", -1)) != INITIAL_STAGE + 1:
		_fail("(c) mina barata no estágio %d, esperava %d após a obra" % [
			int(cheap.get("stage", -1)), INITIAL_STAGE + 1])
	if int(dear.get("stage", -1)) != INITIAL_STAGE + 3:
		_fail("(c) mina cara mudou de estágio (%d)" % int(dear.get("stage", -1)))
	if _failures.size() == failures_before:
		print("[mine] (c) IA escolheu a mina mais barata: %.0f XGS (a cara custava %.0f)" % [
			cheap_cost, dear_cost])


# ---------------------------------------------------------------------------
# (d) Dinheiro conservado e save/load v18 (formato inalterado).
# ---------------------------------------------------------------------------
func _test_money_and_save() -> void:
	var failures_before := _failures.size()
	var sim = _make_sim()
	sim.set_instant_construction(true)
	_boost_budget(sim, "MINE-A", 1.0e9)
	var money_initial := float(sim.get_status()["money_total"])
	for day in range(1, 11):
		_check(sim.tick_day(), day)
	var deepen: Dictionary = sim.deepen_mine("MINE-A", "MINE-A-IRON")
	if not bool(deepen.get("started", false)):
		_fail("(d) aprofundamento recusado: '%s'" % String(deepen.get("error", "")))
		return
	_check(sim.tick_day(), 11)
	var money_final := float(sim.get_status()["money_total"])
	if absf(money_final - money_initial) > MONEY_EPSILON:
		_fail("(d) dinheiro não conservado: %.2f -> %.2f" % [
			money_initial, money_final])
	else:
		print("[mine] (d) dinheiro conservado: %.2f -> %.2f" % [
			money_initial, money_final])

	var saved: Dictionary = sim.to_dict()
	if int(saved.get("version", 0)) != 27:
		_fail("(d) save v%d != v21 (B4 acrescentou a migração; as minas não mudaram)" % int(
			saved.get("version", 0)))
	var clone = EconomySim.new()
	clone.set_goods(_load_goods())
	clone.from_dict(saved)
	var clone_mine := _find_building(clone.get_city_status("MINE-A"), "MINE-A-IRON")
	if int(clone_mine.get("stage", -1)) != INITIAL_STAGE + 1:
		_fail("(d) round-trip perdeu o estágio (%d)" % int(
			clone_mine.get("stage", -1)))
	var live_mine := _find_building(sim.get_city_status("MINE-A"), "MINE-A-IRON")
	if absf(float(clone_mine.get("next_deepen_cost", 0.0))
			- float(live_mine.get("next_deepen_cost", 0.0))) > 0.01:
		_fail("(d) round-trip mudou o custo do próximo aprofundamento")

	# Migração v17 -> v18: o save da B2 tem `pop.strata` mas não tem `stage`.
	var legacy17: Dictionary = saved.duplicate(true)
	legacy17["version"] = 17
	for entry in legacy17.get("buildings", []):
		entry.erase("stage")
		entry.erase("stage_reserve_kt")
		entry.erase("stage_mined_kt")
	for project in legacy17.get("construction_projects", []):
		project.erase("deepen_stage")
	var migrated17 = EconomySim.new()
	migrated17.set_goods(_load_goods())
	migrated17.from_dict(legacy17)
	var mine17 := _find_building(migrated17.get_city_status("MINE-A"),
		"MINE-A-IRON")
	if int(mine17.get("stage", -1)) != INITIAL_STAGE:
		_fail("(d) migração v17: estágio %d != %d" % [
			int(mine17.get("stage", -1)), INITIAL_STAGE])
	if absf(float(mine17.get("stage_factor", 0.0)) - STAGE_FLOOR) > 1e-9:
		_fail("(d) migração v17: mina antiga não ficou no piso (fator %.3f)" % float(
			mine17.get("stage_factor", 0.0)))
	# A B2 sobrevive à passagem: o bloco `strata` gravado continua idêntico no
	# resave (o load v17 não pode perder a B2 nem re-semear por cima).
	var strata_in: Dictionary = Dictionary(Dictionary(
		Array(saved.get("cities", []))[0]).get("pop", {})).get("strata", {})
	var strata_out: Dictionary = Dictionary(Dictionary(
		Array(migrated17.to_dict().get("cities", []))[0]).get("pop", {})).get(
		"strata", {})
	if not bool(strata_out.get("ready", false)) \
			or Array(strata_out.get("income_share", [])) != Array(strata_in.get(
				"income_share", [])):
		_fail("(d) migração v17: bloco strata da B2 se perdeu")

	# Cadeia v16 -> v17 -> v18: save v16 não tem `strata` NEM `stage`; o load
	# roda as duas etapas (B2 semeia os quintis no fim, minas caem no último
	# estágio).
	var legacy: Dictionary = legacy17.duplicate(true)
	legacy["version"] = 16
	for city_entry in legacy.get("cities", []):
		Dictionary(city_entry.get("pop", {})).erase("strata")
	var migrated = EconomySim.new()
	migrated.set_goods(_load_goods())
	migrated.from_dict(legacy)
	var legacy_mine := _find_building(migrated.get_city_status("MINE-A"),
		"MINE-A-IRON")
	if int(legacy_mine.get("stage", -1)) != INITIAL_STAGE:
		_fail("(d) migração v16: estágio %d != %d" % [
			int(legacy_mine.get("stage", -1)), INITIAL_STAGE])
	if absf(float(legacy_mine.get("stage_factor", 0.0)) - STAGE_FLOOR) > 1e-9:
		_fail("(d) migração v16: mina antiga não ficou no piso (fator %.3f)" % float(
			legacy_mine.get("stage_factor", 0.0)))
	var strata16: Dictionary = Dictionary(Dictionary(
		Array(migrated.to_dict().get("cities", []))[0]).get("pop", {})).get(
		"strata", {})
	if not bool(strata16.get("ready", false)) \
			or Array(strata16.get("income_share", [])).size() != 5:
		_fail("(d) migração v16: a etapa v17 (B2) não rodou na cadeia")
	if _failures.size() == failures_before:
		print("[mine] (d) save v21 round-trip ok; v17 -> último estágio (piso, B2 intacta); v16 -> v17 -> v18 -> v20 -> v21")


# ---------------------------------------------------------------------------
# (e) Semente pela produção real (D-086b): mina com dado nasce OPERANDO com a
# capacidade que reproduz a produção real atribuída; sem dado, último estágio.
# ---------------------------------------------------------------------------
func _test_seeded_real() -> void:
	var failures_before := _failures.size()
	var geo = load(GEOGRAPHY_PATH).new()
	if not bool(geo.load_data()):
		_fail("(e) GeographyService não carregou os dados")
		return
	var cities: Array = _load_json_array(CITIES_PATH)
	var seed_info := _seed_info()
	# Três locais REAIS com produção atribuída (dado versionado) + uma cidade
	# cujo mineral não está na semente (nesse caso a regra histórica vale).
	var selected: Array = []
	var by_id := {}
	for city in cities:
		by_id[String(Dictionary(city).get("id", ""))] = city
	for key: String in SEED_CASES:
		var city_id := key.split("|")[0]
		if not by_id.has(city_id):
			_fail("(e) cidade %s da semente ausente do roster" % city_id)
			return
		selected.append(by_id[city_id])
	var no_data_key := ""
	for city in cities:
		var city_id := String(Dictionary(city).get("id", ""))
		var minerals: Dictionary = geo._aggregate_deposits(geo.deposits_for_city(city_id))
		for resource in minerals:
			var candidate := city_id + "|" + String(resource)
			if not seed_info.has(candidate):
				no_data_key = candidate
				break
		if no_data_key != "":
			selected.append(Dictionary(city))
			break
	if no_data_key == "":
		_fail("(e) nenhuma jazida sem semente encontrada (caso sem dado não testável)")
		return

	var resolved: Array = geo.resolve_initial_mines([], selected)
	for key: String in SEED_CASES:
		var entry := _find_entry(resolved, key)
		if entry.is_empty():
			_fail("(e) mina semeada ausente para %s" % key)
			continue
		if int(entry.get("stage", -1)) != 0:
			_fail("(e) %s nasceu no estágio %d (esperava 0, operando)" % [
				key, int(entry.get("stage", -1))])
		var city_id := key.split("|")[0]
		var resource := key.split("|")[1]
		var quality := float(Dictionary(geo._aggregate_deposits(
			geo.deposits_for_city(city_id)).get(resource, {})).get("quality", 0.0))
		var efficiency := 0.5 + 0.5 * clampf(quality, 0.0, 1.0)
		var expected := float(SEED_CASES[key]) / efficiency
		var nominal := float(entry.get("output_tons_per_day", 0.0))
		if absf(nominal - expected) > 0.002:
			_fail("(e) %s capacidade %.4f != produção real ÷ eficiência %.4f" % [
				key, nominal, expected])
		var data: Dictionary = seed_info[key]
		if String(entry.get("owner", "")) != String(data.get("owner", "state")):
			_fail("(e) %s dono '%s' != semente '%s'" % [
				key, String(entry.get("owner", "")), String(data.get("owner", ""))])
	var no_data := _find_entry(resolved, no_data_key)
	if no_data.is_empty():
		_fail("(e) mina sem dado %s não foi criada" % no_data_key)
	elif int(no_data.get("stage", 0)) != -1:
		_fail("(e) mina sem dado %s nasceu no estágio %d (esperava -1 = último)" % [
			no_data_key, int(no_data.get("stage", 0))])

	# Núcleo: a mina de `stage >= 0` abre a reserva do estágio quando a jazida é
	# declarada e produz 100%; a histórica (`stage = -1`) fica no piso.
	var sim = _make_seeded_sim()
	var seeded := _find_building(sim.get_city_status("SEED-A"), "SEED-A-IRON")
	var historical := _find_building(sim.get_city_status("SEED-B"), "SEED-B-IRON")
	if int(seeded.get("stage", -1)) != 0:
		_fail("(e) núcleo: estágio inicial %d != 0" % int(seeded.get("stage", -1)))
	if absf(float(seeded.get("stage_reserve_kt", 0.0)) - 1000.0) > 1e-6:
		_fail("(e) núcleo: reserva do estágio %.3f != 50%% de 2000 kt" % float(
			seeded.get("stage_reserve_kt", 0.0)))
	if absf(float(seeded.get("stage_factor", 0.0)) - 1.0) > 1e-9:
		_fail("(e) núcleo: fator da mina semeada %.4f != 1,0" % float(
			seeded.get("stage_factor", 0.0)))
	if absf(float(historical.get("stage_reserve_kt", -1.0))) > 1e-9:
		_fail("(e) núcleo: mina histórica abriu reserva de estágio (%.3f)" % float(
			historical.get("stage_reserve_kt", 0.0)))
	if absf(float(historical.get("stage_factor", 0.0)) - STAGE_FLOOR) > 1e-9:
		_fail("(e) núcleo: fator da mina histórica %.4f != piso %.2f" % [
			float(historical.get("stage_factor", 0.0)), STAGE_FLOOR])
	sim.tick_day()
	var seeded_after := _find_building(sim.get_city_status("SEED-A"), "SEED-A-IRON")
	var historical_after := _find_building(sim.get_city_status("SEED-B"), "SEED-B-IRON")
	var seeded_output := float(seeded_after.get("capacity", 0.0)) \
			* float(seeded_after.get("utilization", 0.0))
	var historical_output := float(historical_after.get("capacity", 0.0)) \
			* float(historical_after.get("utilization", 0.0))
	if absf(seeded_output - 100.0) > 1.0:
		_fail("(e) núcleo: dia 0 da mina semeada %.2f t/d != alvo 100" % seeded_output)
	if historical_output > 6.0:
		_fail("(e) núcleo: dia 0 da mina histórica %.2f t/d acima do piso" % (
			historical_output))
	if _failures.size() == failures_before:
		print("[mine] (e) semente: 3 pares reais operando (= produção ÷ eficiência), sem dado no último estágio; dia 0 semeada %.1f t/d X histórica %.1f t/d" % [
			seeded_output, historical_output])


func _make_seeded_sim():
	var sim = EconomySim.new()
	sim.set_goods(_load_goods())
	sim.initialize_world(
		[
			{"id": "SEED-A", "name": "Semeada", "nation": "AAA",
				"home_nation": "AAA", "landmass": "north",
				"population": 2_000_000.0, "x": 2500.0, "y": 600.0},
			{"id": "SEED-B", "name": "Histórica", "nation": "AAA",
				"home_nation": "AAA", "landmass": "north",
				"population": 2_000_000.0, "x": 2500.0, "y": 640.0},
		],
		[],
		[
			{"city_id": "SEED-A", "id": "SEED-A-IRON", "name": "Semeada — ferro",
				"resource": "iron", "output_tons_per_day": 100.0,
				"owner": "private", "stage": 0},
			{"city_id": "SEED-B", "id": "SEED-B-IRON", "name": "Histórica — ferro",
				"resource": "iron", "output_tons_per_day": 100.0,
				"owner": "state", "stage": -1},
		])
	for city_id in ["SEED-A", "SEED-B"]:
		sim.set_suitability(city_id, "iron", 1.0)
		sim.set_deposit(city_id, "iron", 2000.0, 1.0, 500.0)
		sim.set_external_demand(city_id, "iron", 1000.0)
	return sim


func _seed_info() -> Dictionary:
	var doc: Dictionary = _load_json_dict(SEED_PATH)
	var out := {}
	var commodities: Dictionary = doc.get("commodities", {})
	for commodity in commodities:
		var countries: Dictionary = Dictionary(commodities[commodity]).get(
			"countries", {})
		for tag in countries:
			var country: Dictionary = countries[tag]
			for row in (country.get("cities", []) as Array):
				var entry: Dictionary = row
				out[String(entry.get("city_id", "")) + "|" + String(commodity)] = {
					"capacity_t_day": float(entry.get("capacity_t_day", 0.0)),
					"owner": String(country.get("owner", "state")),
				}
	return out


func _find_entry(entries: Array, key: String) -> Dictionary:
	for entry in entries:
		var row: Dictionary = entry
		if String(row.get("city_id", "")) + "|" + String(
				row.get("resource", "")) == key:
			return row
	return {}


func _load_json_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		push_error("mine_stage_test: não abriu " + path)
		return []
	var parsed: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return parsed if typeof(parsed) == TYPE_ARRAY else []


func _load_json_dict(path: String) -> Dictionary:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		push_error("mine_stage_test: não abriu " + path)
		return {}
	var parsed: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return parsed if typeof(parsed) == TYPE_DICTIONARY else {}


# ---------------------------------------------------------------------------
# Mundo sintético
# ---------------------------------------------------------------------------
func _make_sim():
	var sim = EconomySim.new()
	sim.set_goods(_load_goods())
	sim.initialize_world(_cities(), [], _buildings())
	sim.set_suitability("MINE-A", "iron", 1.0)
	sim.set_suitability("MINE-B", "iron", 1.0)
	# Jazida de referência grande (A) e jazida de referência de 50 t (B): B
	# consome a reserva no 1º dia e continua extraindo além dela.
	sim.set_deposit("MINE-A", "iron", 2000.0, 1.0, 500.0)
	sim.set_deposit("MINE-B", "iron", 0.05, 1.0, 100.0)
	sim.set_external_demand("MINE-A", "iron", 100.0)
	sim.set_external_demand("MINE-B", "iron", 100.0)
	return sim


func _make_ai_sim():
	var sim = EconomySim.new()
	sim.set_goods(_load_goods())
	sim.set_instant_construction(true)
	sim.initialize_world(
		[{"id": "AI-CITY", "name": "Cidade IA", "nation": "AAA",
			"home_nation": "AAA", "landmass": "north", "population": 1000.0,
			"x": 2600.0, "y": 600.0}],
		[],
		[
			# Ordem de propósito: a mina CARA vem primeiro. Um "primeiro que
			# aparecer" aprofundaria a cara; a IA tem de comparar o custo.
			{"city_id": "AI-CITY", "id": "AI-DEAR-IRON",
				"name": "Mina cara — ferro", "resource": "iron",
				"output_tons_per_day": 10.0, "owner": "state", "stage": 6},
			{"city_id": "AI-CITY", "id": "AI-CHEAP-IRON",
				"name": "Mina barata — ferro", "resource": "iron",
				"output_tons_per_day": 10.0, "owner": "state", "stage": -1},
		])
	sim.set_suitability("AI-CITY", "iron", 1.0)
	sim.set_deposit("AI-CITY", "iron", 2000.0, 1.0, 100.0)
	sim.set_external_demand("AI-CITY", "iron", 50.0)
	_boost_budget(sim, "AI-CITY", 1.0e6)
	sim.set_ai_enabled(true)
	return sim


func _cities() -> Array:
	return [
		{"id": "MINE-A", "name": "Mina A", "nation": "AAA", "home_nation": "AAA",
			"landmass": "north", "population": 2_000_000.0,
			"x": 2500.0, "y": 600.0},
		{"id": "MINE-B", "name": "Mina B", "nation": "AAA", "home_nation": "AAA",
			"landmass": "north", "population": 300_000.0,
			"x": 2500.0, "y": 640.0},
	]


func _buildings() -> Array:
	# `stage` = -1: mina JÁ EXISTENTE no último estágio (contrato do
	# GeographyService, D-086). A capacidade nominal é dimensionada fora do
	# núcleo; a produção efetiva fica no piso até o aprofundamento.
	# MINE-B tem capacidade grande de propósito: 5% dela (50 t/dia) esgota a
	# jazida de referência de 50 t no primeiro dia.
	return [
		{"city_id": "MINE-A", "id": "MINE-A-IRON", "name": "Mina A — ferro",
			"resource": "iron", "output_tons_per_day": 10.0,
			"owner": "state", "stage": -1},
		{"city_id": "MINE-B", "id": "MINE-B-IRON", "name": "Mina B — ferro",
			"resource": "iron", "output_tons_per_day": 1000.0,
			"owner": "state", "stage": -1},
	]


func _boost_budget(sim, city_id: String, government: float) -> void:
	var snapshot: Dictionary = sim.to_dict()
	for city in snapshot["cities"]:
		if String(city["id"]) == city_id:
			city["government_budget"] = government
	sim.from_dict(snapshot)


func _load_goods() -> Array:
	var file := FileAccess.open(GOODS_PATH, FileAccess.READ)
	if file == null:
		push_error("mine_stage_test: não abriu " + GOODS_PATH)
		return []
	var parsed: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return parsed if typeof(parsed) == TYPE_ARRAY else []


func _find_building(info: Dictionary, building_id: String) -> Dictionary:
	for building in info.get("buildings", []):
		if String(building.get("id", "")) == building_id:
			return building
	return {}


func _check(status: Dictionary, day: int) -> void:
	var violations: Array = status.get("invariant_violations", [])
	if not violations.is_empty():
		_fail("dia %d: invariante violado: %s" % [day, str(violations[0])])


func _fail(message: String) -> void:
	_failures.append(message)
	print("[mine] FALHA: " + message)


func _finish() -> void:
	if _failures.is_empty():
		print("[mine] OK: estágio de mina validado (recurso nunca acaba, custo convexo, IA escolhe a mais barata, conservação, save).")
		quit(0)
	else:
		print("[mine] %d falha(s)" % _failures.size())
		quit(1)
