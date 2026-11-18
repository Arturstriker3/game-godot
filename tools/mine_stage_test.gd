extends SceneTree

## D-083 — ESTÁGIO DE MINA (mecânica, sem calibração de produção).
##
## Mundo sintético e determinístico (duas cidades da mesma nação, duas minas de
## ferro) com o catálogo REAL de bens. Cobre o aceite da decisão do dono
## (2026-11-17):
##   (a) mina inicial no ÚLTIMO estágio produz ~piso (5% da capacidade);
##   (b) aprofundar custa X, leva N dias e devolve a produção (estágio novo
##       cheio, capacidade maior);
##   (c) o 2º aprofundamento custa mais que o 1º (mais fundo = mais caro);
##   (d) reserva esgotada bloqueia o aprofundamento;
##   (e) o dinheiro é conservado (obra sai do caixa do dono e vira demanda de
##       construção);
##   mais: status/A4 publicam estágio, produção relativa e custo do próximo
##   aprofundamento; save v17 preserva o estágio e save antigo migra para o
##   último estágio.
##
## Uso: godot --headless --path . --script res://tools/mine_stage_test.gd

const GOODS_PATH := "res://data/definitions/economy/goods.json"
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
const MONEY_EPSILON := 0.5

var _failures: Array[String] = []


func _init() -> void:
	var sim = EconomySim.new()
	# Catálogo real: a obra compra construction_material/steel_structure do
	# mercado (sem ele a obra não gasta o fundo reservado e o saldo sobra).
	sim.set_goods(_load_goods())
	sim.initialize_world(_cities(), [], _buildings())
	# Jazida grande da cidade A (a mina "histórica" existe desde o dia 0) e
	# jazida ZERADA da cidade B (para o teste de reserva esgotada).
	sim.set_deposit("MINE-A", "iron", 2000.0, 1.0, 500.0)
	sim.set_deposit("MINE-B", "iron", 0.0, 0.5, 100.0)
	# Mercado comprador: sem teto de exportação a produção não teria para quem
	# vender e a rampa da produção não seria observável.
	sim.set_external_demand("MINE-A", "iron", 100.0)

	var status: Dictionary = sim.get_status()
	var money_initial := float(status["money_total"])
	_check(status, 0)

	# (a) Mina inicial: último estágio, produção relativa no piso, custo do
	# próximo aprofundamento publicado (status/A4).
	var info: Dictionary = sim.get_city_status("MINE-A")
	var mine := _find_building(info, "MINE-A-IRON")
	if mine.is_empty():
		_fail("(a) mina inicial MINE-A-IRON ausente do status")
		_finish()
		return
	if int(mine.get("stage", -1)) != INITIAL_STAGE:
		_fail("(a) estágio inicial %d != %d (último estágio)" % [
			int(mine.get("stage", -1)), INITIAL_STAGE])
	if absf(float(mine.get("stage_factor", 0.0)) - STAGE_FLOOR) > 1e-9:
		_fail("(a) produção relativa %.4f != piso %.2f" % [
			float(mine.get("stage_factor", 0.0)), STAGE_FLOOR])
	var cost1 := float(mine.get("next_deepen_cost", 0.0))
	if cost1 <= 0.0:
		_fail("(a) custo do próximo aprofundamento não publicado")
	if String(mine.get("owner", "")) != "state":
		_fail("(a) dono inicial deveria ser Estado (é %s)" % String(mine.get("owner", "")))

	# Primeiro dia: a mina no piso extrai ~5% da capacidade de 10 t/dia.
	status = sim.tick_day()
	_check(status, 1)
	var floor_tons := float(status["produced_today"][IRON])
	if absf(floor_tons - 0.5) > 0.02:
		_fail("(a) produção no piso %.3f t != 0.5 t (5%% de 10 t/dia)" % floor_tons)
	info = sim.get_city_status("MINE-A")
	mine = _find_building(info, "MINE-A-IRON")
	if absf(float(mine.get("utilization", 0.0)) - STAGE_FLOOR) > 0.01:
		_fail("(a) utilização %.3f != piso" % float(mine.get("utilization", 0.0)))
	print("[mine] (a) mina inicial: estágio=%d produção=%.2f t/dia (piso %.0f%% de 10)" % [
		INITIAL_STAGE, floor_tons, STAGE_FLOOR * 100.0])

	# (d) Reserva esgotada bloqueia — cidade B tem jazida zerada.
	var blocked: Dictionary = sim.deepen_mine("MINE-B", "MINE-B-IRON")
	if bool(blocked.get("started", false)):
		_fail("(d) aprofundamento aceito com reserva zerada")
	if not String(blocked.get("error", "")).contains("reserva"):
		_fail("(d) erro inesperado com reserva zerada: '%s'" % String(blocked.get("error", "")))
	print("[mine] (d) reserva esgotada bloqueia: '%s'" % String(blocked.get("error", "")))

	# (b) Aprofundar: obra física (§15) paga pelo dono, prazo declarado.
	var budget_before := _city_budget(sim.get_city_status("MINE-A"))
	var deepen: Dictionary = sim.deepen_mine("MINE-A", "MINE-A-IRON")
	if not bool(deepen.get("started", false)):
		_fail("(b) aprofundamento recusado: '%s'" % String(deepen.get("error", "")))
		_finish()
		return
	var cost_started := float(deepen.get("cost", 0.0))
	if absf(cost_started - cost1) > 0.01:
		_fail("(b) custo cobrado %.2f != custo publicado %.2f" % [cost_started, cost1])
	if int(deepen.get("stage", 0)) != INITIAL_STAGE + 1:
		_fail("(b) estágio aberto %d != %d" % [
			int(deepen.get("stage", 0)), INITIAL_STAGE + 1])
	var expected_days := DEEPEN_BASE_DAYS + DEEPEN_DAYS_PER_STAGE * (INITIAL_STAGE + 1)
	if int(deepen.get("duration_days", 0)) != expected_days:
		_fail("(b) prazo %d dias != %d dias" % [
			int(deepen.get("duration_days", 0)), expected_days])
	if String(deepen.get("project_id", "")).is_empty():
		_fail("(b) aprofundamento deveria virar projeto físico (§15)")
	var budget_after := _city_budget(sim.get_city_status("MINE-A"))
	if absf(budget_before - budget_after - cost_started) > 0.01:
		_fail("(b) orçamento do dono não debitado (%.2f -> %.2f, custo %.2f)" % [
			budget_before, budget_after, cost_started])
	_check(sim.get_status(), 1)

	# A obra avança um dia por tick: a produção só volta ao concluir.
	for day in range(2, 2 + expected_days):
		status = sim.tick_day()
		_check(status, day)
	info = sim.get_city_status("MINE-A")
	mine = _find_building(info, "MINE-A-IRON")
	if int(mine.get("stage", -1)) != INITIAL_STAGE + 1:
		_fail("(b) após %d dias o estágio é %d (esperava %d)" % [
			expected_days, int(mine.get("stage", -1)), INITIAL_STAGE + 1])
	if absf(float(mine.get("capacity", 0.0)) - 10.0 * CAPACITY_FACTOR) > 0.01:
		_fail("(b) capacidade após aprofundar %.2f != %.1f" % [
			float(mine.get("capacity", 0.0)), 10.0 * CAPACITY_FACTOR])
	if float(mine.get("stage_factor", 0.0)) < 1.0 - 1e-3:
		_fail("(b) estágio novo não deveria começar esgotado (fator %.5f)" % [
			float(mine.get("stage_factor", 0.0))])
	print("[mine] (b) aprofundado: custo=%.0f, prazo=%d d, capacidade=%.1f t/dia" % [
		cost_started, expected_days, float(mine.get("capacity", 0.0))])

	# A produção VOLTA: com o mercado comprando, a rampa sobe até a capacidade.
	for day in range(2 + expected_days, 2 + expected_days + 20):
		status = sim.tick_day()
		_check(status, day)
	var ramp_tons := float(status["produced_today"][IRON])
	if ramp_tons <= 5.0 * floor_tons:
		_fail("(b) produção após aprofundar %.3f t não voltou (piso era %.3f)" % [
			ramp_tons, floor_tons])
	print("[mine] (b) produção devolvida: %.2f t/dia (piso era %.2f)" % [ramp_tons, floor_tons])

	# (c) O 2º aprofundamento custa mais que o 1º (mais fundo = mais caro).
	info = sim.get_city_status("MINE-A")
	mine = _find_building(info, "MINE-A-IRON")
	var cost2_published := float(mine.get("next_deepen_cost", 0.0))
	var second: Dictionary = sim.deepen_mine("MINE-A", "MINE-A-IRON")
	if not bool(second.get("started", false)):
		_fail("(c) 2º aprofundamento recusado: '%s'" % String(second.get("error", "")))
	elif float(second.get("cost", 0.0)) <= cost_started:
		_fail("(c) 2º custo %.2f <= 1º %.2f" % [
			float(second.get("cost", 0.0)), cost_started])
	if absf(cost2_published - float(second.get("cost", 0.0))) > 0.01:
		_fail("(c) custo publicado %.2f != cobrado %.2f" % [
			cost2_published, float(second.get("cost", 0.0))])
	if int(second.get("duration_days", 0)) <= int(deepen.get("duration_days", 0)):
		_fail("(c) 2º prazo não cresceu (%d <= %d)" % [
			int(second.get("duration_days", 0)), int(deepen.get("duration_days", 0))])
	print("[mine] (c) 2º aprofundamento: custo=%.0f (> %.0f), prazo=%d d" % [
		float(second.get("cost", 0.0)), cost_started,
		int(second.get("duration_days", 0))])

	# Anti-duplicação: uma obra de aprofundamento por mina.
	var duplicate: Dictionary = sim.deepen_mine("MINE-A", "MINE-A-IRON")
	if bool(duplicate.get("started", false)):
		_fail("anti-duplicação: 2ª obra aceita na mesma mina")

	# (e) Dinheiro conservado em todo o caminho (obra, materiais, serviço).
	var money_final := float(sim.get_status()["money_total"])
	if absf(money_final - money_initial) > MONEY_EPSILON:
		_fail("(e) dinheiro não conservado: %.2f -> %.2f" % [money_initial, money_final])
	print("[mine] (e) dinheiro conservado: %.2f -> %.2f" % [money_initial, money_final])

	# Save v17: round-trip preserva o estágio; save antigo (sem as chaves) migra
	# para o último estágio com o estágio esgotado.
	_check_save_round_trip(sim)

	_finish()


func _check_save_round_trip(sim) -> void:
	var saved: Dictionary = sim.to_dict()
	if int(saved.get("version", 0)) < 17:
		_fail("save: versão %d < 17" % int(saved.get("version", 0)))
	var clone = EconomySim.new()
	clone.set_goods(_load_goods())
	clone.from_dict(saved)
	var info: Dictionary = clone.get_city_status("MINE-A")
	var mine := _find_building(info, "MINE-A-IRON")
	if int(mine.get("stage", -1)) != INITIAL_STAGE + 1:
		_fail("save v17: estágio perdido no round-trip (%d)" % int(mine.get("stage", -1)))
	# Migração: um save v16 não tem `stage` em nenhuma construção.
	var legacy: Dictionary = saved.duplicate(true)
	legacy["version"] = 16
	for entry in legacy.get("buildings", []):
		entry.erase("stage")
		entry.erase("stage_reserve_kt")
		entry.erase("stage_mined_kt")
	var migrated = EconomySim.new()
	migrated.set_goods(_load_goods())
	migrated.from_dict(legacy)
	var legacy_info: Dictionary = migrated.get_city_status("MINE-A")
	var legacy_mine := _find_building(legacy_info, "MINE-A-IRON")
	if int(legacy_mine.get("stage", -1)) != INITIAL_STAGE:
		_fail("migração v16: estágio %d != %d (último estágio)" % [
			int(legacy_mine.get("stage", -1)), INITIAL_STAGE])
	if absf(float(legacy_mine.get("stage_factor", 0.0)) - STAGE_FLOOR) > 1e-9:
		_fail("migração v16: mina antiga não ficou no piso (fator %.3f)" % [
			float(legacy_mine.get("stage_factor", 0.0))])
	print("[mine] save v17 ok; migração v16 -> último estágio (piso)")


func _load_goods() -> Array:
	var file := FileAccess.open(GOODS_PATH, FileAccess.READ)
	if file == null:
		push_error("mine_stage_test: não abriu " + GOODS_PATH)
		return []
	var parsed: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return parsed if typeof(parsed) == TYPE_ARRAY else []


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
	# GeographyService, D-083). A capacidade nominal é dimensionada fora do
	# núcleo; a produção efetiva fica no piso até o aprofundamento.
	return [
		{"city_id": "MINE-A", "id": "MINE-A-IRON", "name": "Mina A — ferro",
			"resource": "iron", "output_tons_per_day": 10.0,
			"owner": "state", "stage": -1},
		{"city_id": "MINE-B", "id": "MINE-B-IRON", "name": "Mina B — ferro",
			"resource": "iron", "output_tons_per_day": 4.0,
			"owner": "state", "stage": -1},
	]


func _find_building(info: Dictionary, building_id: String) -> Dictionary:
	for building in info.get("buildings", []):
		if String(building.get("id", "")) == building_id:
			return building
	return {}


func _city_budget(info: Dictionary) -> float:
	return float(info.get("government_budget", 0.0))


func _check(status: Dictionary, day: int) -> void:
	var violations: Array = status.get("invariant_violations", [])
	if not violations.is_empty():
		_fail("dia %d: invariante violado: %s" % [day, str(violations[0])])


func _fail(message: String) -> void:
	_failures.append(message)
	print("[mine] FALHA: " + message)


func _finish() -> void:
	if _failures.is_empty():
		print("[mine] OK: estágio de mina validado (piso, aprofundamento, custo crescente, reserva, conservação, save).")
		quit(0)
	else:
		print("[mine] %d falha(s)" % _failures.size())
		quit(1)
