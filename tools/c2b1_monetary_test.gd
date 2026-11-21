extends SceneTree

## C2b1 (D-104) — EMISSÃO DE MOEDA E JUROS DO BANCO CENTRAL.
##
## Prova, no cenário de fixture (duas cidades, nação DEMO):
##   1. DEFAULTS: `bank_independence` salvo continua `no_data` e o EFETIVO é o
##      default declarado do jogo (`independent`), com os parâmetros idênticos
##      aos de data/definitions/economy/monetary_policy.json (fonte
##      "default de jogo") — nada de número de país inventado.
##   2. REGRA DE TAYLOR (BC independente): a taxa respeita piso/teto e o PASSO
##      máximo de 25 pb por dia; sem inflação na série, o termo de inflação não
##      entra (`has_inflation` falso) e nada é completado.
##   3. EMISSÃO (BC controlado): juro abaixo do neutro; a emissão cobre o
##      déficit MEDIDO do dia (caixa do governo), é limitada a 1 % do M0 por dia
##      e a conservação passa a ser "dinheiro total = anterior + emissão".
##      ΔM0 = emissão registrada; dívida pública = títulos do BC; identidade
##      financeira da C1 continua 0 (balance.ok) em todos os dias.
##   4. SAVE v28: bloco `monetary`, round-trip bit-exato, continuação idêntica
##      e migração v27 → v28 (taxa sem decisão, emissão 0, dívida 0).
##
## Roda pelo runner: powershell -File tools/run_tests.ps1 -List c2b1_monetary_test

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"
const MONETARY_PATH := "res://data/definitions/economy/monetary_policy.json"

const SAVE_VERSION := 33
const EPS := 1e-3
const DAYS_INDEPENDENT := 40
const DAYS_CONTROLLED := 60

var _failures: Array[String] = []


func _init() -> void:
	_test_defaults_and_data()
	_test_harvest_credit_flag()
	_test_independent()
	_test_controlled()
	_test_save_and_migration()
	if _failures.is_empty():
		print("[c2b1] OK: emissão contabilizada, Taylor com passo limitado, "
			+ "BC controlado, ΔM0 = emissão, save v28 e migração v27→v28.")
		quit(0)
	else:
		print("[c2b1] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _check(cond: bool, message: String) -> void:
	if not cond:
		_fail(message)


func _close(a: float, b: float, tolerance: float) -> bool:
	return absf(a - b) <= tolerance


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _make_sim() -> Object:
	var sim = EconomySim.new()
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.set_services(_json(SERVICES_PATH))
	sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_json(AGE_STRUCTURE_PATH))
	# Uma moeda para a nação do cenário (DEMO). Sem moeda a moeda do dia cai
	# em `no_currency` e não há M0 por moeda para a política agir.
	sim.set_currencies([{
		"code": "DEM",
		"nations": ["DEMO"],
		"rate_per_anchor": 1.0,
		"regime": "peg",
		"source": "fixture",
	}])
	sim.initialize_scenario()
	return sim


func _policy(sim: Object, code: String = "DEM") -> Dictionary:
	var bank: Dictionary = sim.get_central_bank_status()
	var currencies: Dictionary = bank.get("currencies", {})
	if not currencies.has(code):
		return {}
	return (currencies[code] as Dictionary).get("policy", {})


func _totals(sim: Object) -> Dictionary:
	return (sim.get_central_bank_status() as Dictionary).get("totals", {})


func _check_day(sim: Object, label: String) -> void:
	var status: Dictionary = sim.get_status()
	var balance: Dictionary = status.get("balance", {})
	_check(not balance.is_empty(), "%s: status sem `balance`" % label)
	_check(bool(balance.get("ok", false)),
		"%s: balance.ok falso (financial_sum %.9f, residual %.9f)" % [
			label, float(balance.get("financial_sum", 1e9)),
			float(balance.get("cash_residual", 1e9))])
	_check(absf(float(balance.get("financial_sum", 1e9))) <= EPS,
		"%s: identidade financeira da C1 != 0" % label)
	var invariant: Dictionary = sim.get_central_bank_status().get(
		"invariant", {})
	_check(bool(invariant.get("ok", false)),
		"%s: moeda não atribuível (resíduo %.6f)" % [
			label, float(invariant.get("residual", 1e9))])


# --- 1. Defaults declarados × dado do jogo --------------------------------
func _test_defaults_and_data() -> void:
	var data: Dictionary = _json(MONETARY_PATH)
	_check(String(data.get("source", "")) == "default de jogo",
		"dados: monetary_policy.json sem fonte 'default de jogo'")
	var defaults: Dictionary = data.get("defaults", {})
	_check(not defaults.is_empty(), "dados: monetary_policy.json sem defaults")

	var sim := _make_sim()
	var policy := _policy(sim)
	_check(not policy.is_empty(), "defaults: status sem `policy` da moeda DEM")
	if policy.is_empty():
		return
	_check(String(policy.get("independence_saved", "")) == "no_data",
		"defaults: `no_data` salvo deveria continuar no_data (saiu %s)"
			% String(policy.get("independence_saved", "")))
	_check(String(policy.get("independence_effective", ""))
			== String(defaults.get("bank_independence", "")),
		"defaults: independência efetiva != default do jogo")
	_check(_close(float(policy.get("target_inflation", -1.0)),
		float(defaults.get("target_inflation", -2.0)), 1e-12),
		"defaults: meta de inflação do núcleo != JSON")
	_check(_close(float(policy.get("neutral_rate", -1.0)),
		float(defaults.get("neutral_rate", -2.0)), 1e-12),
		"defaults: taxa neutra do núcleo != JSON")
	_check(_close(float(policy.get("max_step_per_day", -1.0)),
		float(defaults.get("max_step_per_day", -2.0)), 1e-12),
		"defaults: passo máximo do núcleo != JSON")
	_check(_close(float(policy.get("smoothing", -1.0)),
		float(defaults.get("rate_smoothing", -2.0)), 1e-12),
		"defaults: suavização do núcleo != JSON")
	# D-113: as duas regras novas também são paridade núcleo x JSON.
	_check(int(policy.get("gap_baseline_day", -1))
			== int(defaults.get("gap_baseline_day", -2)),
		"defaults: gap_baseline_day do núcleo != JSON")
	_check(int(policy.get("annualize_min_span_days", -1))
			== int(defaults.get("annualize_min_span_days", -2)),
		"defaults: annualize_min_span_days do núcleo != JSON")
	_check(not bool(policy.get("has_gap", true)),
		"defaults: no dia 0 o hiato é AUSENTE (has_gap tem de ser falso)")
	_check(_close(float(policy.get("rate", -1.0)),
		float(defaults.get("neutral_rate", -2.0)), 1e-12),
		"defaults: dia 0 sem leitura deveria estar na taxa neutra")
	_check(int(policy.get("last_day", 0)) == -1,
		"defaults: dia 0 não pode ter decisão de taxa")
	var totals: Dictionary = _totals(sim)
	_check(_close(float(totals.get("emission_total", -1.0)), 0.0, 1e-12),
		"defaults: emissão inicial deveria ser 0")
	_check(_close(float(totals.get("government_debt", -1.0)), 0.0, 1e-12),
		"defaults: dívida pública inicial deveria ser 0")
	_check_day(sim, "defaults")


# --- 1b. D-105/D-111: juro do envelope da safra LIGADO por padrão ----------
## O dado declara `harvest_credit.apply_policy_rate` (true desde a D-111, quando
## a C4 criou o credor); o núcleo expõe a flag no status e o gancho de A/B
## (`set_balance`) liga/desliga sem mover dinheiro nem quebrar a identidade
## financeira. A prova do EFECTO no portão de comida é a rodada de 365 d
## (`tools/c1_world_ai_365.gd`, ver D-105/D-111).
func _test_harvest_credit_flag() -> void:
	var data: Dictionary = _json(MONETARY_PATH)
	var harvest: Dictionary = data.get("harvest_credit", {})
	_check(not harvest.is_empty(),
		"dados: monetary_policy.json sem bloco `harvest_credit` (D-105)")
	_check(harvest.has("apply_policy_rate"),
		"dados: harvest_credit sem `apply_policy_rate`")
	_check(bool(harvest.get("apply_policy_rate", false)) == true,
		"dados: harvest_credit.apply_policy_rate deveria ser true desde a D-111")
	var sim := _make_sim()
	var world: Dictionary = sim.get_central_bank_status().get(
		"monetary_policy", {})
	_check(world.has("harvest_credit_rate"),
		"status: monetary_policy sem `harvest_credit_rate`")
	_check(bool(world.get("harvest_credit_rate", true))
			== bool(harvest.get("apply_policy_rate", true)),
		"status: flag do núcleo != dado declarado (D-105)")
	sim.set_balance({"harvest_credit_rate": 1})
	_check(bool(sim.get_central_bank_status().get("monetary_policy", {})
			.get("harvest_credit_rate", false)),
		"A/B: set_balance não ligou o juro do envelope da safra")
	var money0 := float(sim.get_status().get("money_total", 0.0))
	for day in 3:
		sim.tick_day()
		_check_day(sim, "D-105 juro ligado dia %d" % (day + 1))
	var drift := absf(float(sim.get_status().get("money_total", 0.0)) - money0)
	_check(drift <= maxf(EPS, 1e-9 * absf(money0)),
		"D-105: juro no envelope moveu dinheiro (drift %.9f)" % drift)
	sim.set_balance({"harvest_credit_rate": 0})
	_check(not bool(sim.get_central_bank_status().get("monetary_policy", {})
			.get("harvest_credit_rate", true)),
		"A/B: set_balance não desligou o juro do envelope da safra")


# --- 2. Taylor (independente) com passo limitado --------------------------
func _test_independent() -> void:
	var sim := _make_sim()
	var step := 0.0025
	var rate_prev := float(_policy(sim).get("rate", -1.0))
	var moved := 0
	var seen_no_inflation := false
	for day in DAYS_INDEPENDENT:
		sim.tick_day()
		var policy := _policy(sim)
		var rate := float(policy.get("rate", -1.0))
		# Regra recomputada de ponta a ponta: prova que os pesos de Taylor, o
		# hiato e a inflação publicados são os que produzem `target_rate`.
		var floor_v := float(policy.get("rate_floor", 0.0))
		var cap_v := float(policy.get("rate_cap", 0.30))
		var target := float(policy.get("neutral_rate", 0.0))
		if bool(policy.get("has_inflation", false)):
			target += float(policy.get("inflation_weight", 0.0)) * (
				float(policy.get("inflation_used", 0.0))
				- float(policy.get("target_inflation", 0.0)))
		target += float(policy.get("gap_weight", 0.0)) * (
			float(policy.get("gap", 0.0)) / 100.0)
		target = clampf(target, floor_v, cap_v)
		_check(_close(float(policy.get("target_rate", -1.0)), target, 1e-12),
			"taylor: target publicado != regra recomputada no dia %d (%.9f vs "
				% [day + 1, float(policy.get("target_rate", -1.0))]
				+ "%.9f)" % target)
		# D-113: a suavização é declarada por DECISÃO mensal e publicada também
		# no equivalente diário (`smoothing_per_day`), que é o que move a taxa.
		var smooth := float(policy.get("smoothing_per_day", -1.0))
		_check(smooth > 0.0 and smooth < 1.0,
			"taylor: smoothing_per_day ausente/inválido no dia %d" % (day + 1))
		var step_v := float(policy.get("max_step_per_day", 0.0))
		var expected := rate_prev + clampf((target - rate_prev) * smooth,
			-step_v, step_v)
		expected = clampf(expected, floor_v, cap_v)
		_check(_close(rate, expected, 1e-12),
			"taylor: taxa != passo/suavização recomputados no dia %d (%0.9f vs "
				% [day + 1, rate] + "%0.9f)" % expected)
		_check(rate >= -1e-12 and rate <= 0.30 + 1e-12,
			"taylor: taxa %.6f fora de [0, 0.30] no dia %d" % [rate, day + 1])
		_check(absf(rate - rate_prev) <= step + 1e-12,
			"taylor: passo %.6f > 25 pb no dia %d" % [
				absf(rate - rate_prev), day + 1])
		if not bool(policy.get("has_inflation", true)):
			seen_no_inflation = true
			_check(_close(float(policy.get("inflation_used", -1.0)), 0.0, 1e-12),
				"taylor: sem inflação o valor usado tem de ser 0")
		# D-113: o hiato é ausente antes do baseline (dia 30) e presente depois.
		var has_gap := bool(policy.get("has_gap", true))
		var baseline_day := int(policy.get("gap_baseline_day", 30))
		if day + 1 < baseline_day:
			_check(not has_gap, "taylor: hiato deveria ser AUSENTE no dia %d"
				% (day + 1))
			_check(_close(float(policy.get("gap", -9.0)), 0.0, 1e-12),
				"taylor: hiato ausente tem de publicar 0 no dia %d" % (day + 1))
		elif day + 1 == baseline_day:
			_check(has_gap, "taylor: hiato deveria existir no dia %d"
				% (day + 1))
		if absf(rate - rate_prev) > 1e-12:
			moved += 1
		rate_prev = rate
		_check_day(sim, "taylor dia %d" % (day + 1))
	_check(seen_no_inflation,
		"taylor: o início da fixture não tem 12 meses de CPI — a ausência "
		+ "deveria ser declarada em algum dia")
	_check(moved > 0,
		"taylor: a taxa não se moveu em %d dias (a regra não está ligada)"
			% DAYS_INDEPENDENT)
	var totals: Dictionary = _totals(sim)
	_check(_close(float(totals.get("emission_total", -1.0)), 0.0, 1e-9),
		"taylor: BC independente NÃO emite (emissão %.6f)" %
			float(totals.get("emission_total", -1.0)))


# --- 3. BC controlado: emissão do déficit medido --------------------------
func _test_controlled() -> void:
	var sim := _make_sim()
	_check(sim.set_central_bank_policy("DEM", "controlled"),
		"controlado: set_central_bank_policy recusou a moeda existente")
	_check(not sim.set_central_bank_policy("XXX", "controlled"),
		"controlado: moeda inexistente deveria ser recusada")
	_check(not sim.set_central_bank_policy("DEM", "inventado"),
		"controlado: nome de independência inválido deveria ser recusado")

	var money_prev := float(sim.get_status().get("money_total", 0.0))
	var emission_prev := float(_totals(sim).get("emission_total", 0.0))
	var m0_start := float(_totals(sim).get("m0", 0.0))
	var _unallocated_at_start := float(
		(sim.get_central_bank_status().get("invariant", {}) as Dictionary)
			.get("unallocated", 0.0))
	var emitted_days := 0
	var max_emission := 0.0
	for day in DAYS_CONTROLLED:
		# O governo gasta em serviço doméstico (prospecção/estudos): déficit
		# MEDIDO no caixa do governo — o BC controlado monetiza.
		if day < 10:
			_check(sim.spend_government("AGRIA", 5000.0),
				"controlado: gasto público recusado no dia %d" % (day + 1))
		var m0_prev := float((_totals(sim)).get("m0", 0.0))
		var status: Dictionary = sim.tick_day()
		var totals := _totals(sim)
		var emission_today := float(totals.get("emission_today", -1.0))
		var money_now := float(status.get("money_total", 0.0))
		# Conservação: dinheiro = anterior + emissão REGISTRADA do dia.
		_check(_close(money_now - money_prev, emission_today,
			maxf(EPS, 1e-9 * absf(money_now))),
			"controlado: dia %d dinheiro variou %.6f e a emissão registrada foi "
				% [day + 1, money_now - money_prev]
				+ "%.6f" % emission_today)
		# Senhoriagem com limite: no máximo 1 % do M0 do dia.
		_check(emission_today <= 0.01 * m0_prev + 1e-6,
			"controlado: emissão do dia %.6f > 1%% do M0 (%.6f)" % [
				emission_today, 0.01 * m0_prev])
		# Emissão = déficit MEDIDO, limitada ao teto. A regra é exata: o BC
		# financia o menor dos dois, e o que sobra fica `unfunded`.
		var policy_day := _policy(sim)
		var deficit_today := float(policy_day.get("deficit_today", -1.0))
		var unfunded := float(policy_day.get("unfunded_today", -1.0))
		_check(_close(emission_today, minf(deficit_today, 0.01 * m0_prev), EPS),
			"controlado: emissão %.6f != min(déficit %.6f, teto %.6f) no dia %d"
				% [emission_today, deficit_today, 0.01 * m0_prev, day + 1])
		_check(_close(emission_today + unfunded, deficit_today,
			maxf(EPS, 1e-9 * maxf(1.0, deficit_today))),
			"controlado: financiado + não financiado != déficit no dia %d"
				% (day + 1))
		if emission_today > 1e-9:
			emitted_days += 1
			max_emission = maxf(max_emission, emission_today)
		money_prev = money_now
		_check_day(sim, "controlado dia %d" % (day + 1))

	var totals_end := _totals(sim)
	var emission_end := float(totals_end.get("emission_total", 0.0))
	_check(emission_end > 0.0,
		"controlado: nenhuma emissão em %d dias — o déficit do governo da "
			% DAYS_CONTROLLED + "fixture não foi medido")
	_check(emitted_days > 0, "controlado: contador de dias com emissão zerado")
	# ΔM0 = emissão registrada. `money_total` inclui os buckets SEM moeda
	# (exterior/setor/obra); o que se move entre eles muda o M0 atribuído sem
	# dinheiro. A identidade honesta é: ΔM0 + Δ(não atribuído) = emissão.
	var bank: Dictionary = sim.get_central_bank_status()
	var m0_end := float((bank.get("totals", {}) as Dictionary).get("m0", 0.0))
	var invariant_start: Dictionary = bank.get("invariant", {})
	var unalloc_start := float(_unallocated_at_start)
	var unalloc_end := float((invariant_start as Dictionary)
		.get("unallocated", 0.0))
	_check(_close((m0_end - m0_start) + (unalloc_end - unalloc_start),
		emission_end - emission_prev, EPS),
		"controlado: ΔM0 %.6f + Δnão atribuído %.6f != emissão %.6f" % [
			m0_end - m0_start, unalloc_end - unalloc_start,
			emission_end - emission_prev])
	# A dívida pública é a contrapartida exata do M0 criado (títulos do BC).
	_check(_close(float(totals_end.get("government_debt", -1.0)),
		emission_end, maxf(EPS, 1e-9 * emission_end)),
		"controlado: dívida pública %.6f != emissão acumulada %.6f" % [
			float(totals_end.get("government_debt", -1.0)), emission_end])
	var bonds: Dictionary = bank.get("government_bonds", {})
	_check(bonds.has("DEMO"), "controlado: nação DEMO sem títulos emitidos")
	_check(_close(float(bonds.get("DEMO", -1.0)), emission_end,
		maxf(EPS, 1e-9 * emission_end)),
		"controlado: dívida de DEMO != emissão")
	# Juro do governo: abaixo do neutro (objetivo declarado do controlado).
	# D-113: com a suavização mensal o alvo é alcançado por convergência, não em
	# poucos dias; a checagem é contra o alvo publicado (neutro − spread).
	var policy := _policy(sim)
	var controlled_target := maxf(0.0, float(policy.get("neutral_rate", 0.0))
		- float(_json(MONETARY_PATH).get("defaults", {})
			.get("controlled_spread", 0.0)))
	var controlled_rate := float(policy.get("rate", -1.0))
	_check(controlled_rate < float(policy.get("neutral_rate", 0.0)) + 1e-12,
		"controlado: taxa (%.6f) deveria ficar abaixo do neutro" % controlled_rate)
	_check(_close(controlled_rate, controlled_target, 5e-3),
		"controlado: taxa deveria convergir ao alvo %.6f (saiu %.6f)"
			% [controlled_target, controlled_rate])
	print("[c2b1] controlado: %d dias com emissão, máxima %.6f/dia, total "
		% [emitted_days, max_emission] + "%.6f" % emission_end)


# --- 4. Save v28, round-trip e migração v27 → v28 -------------------------
func _test_save_and_migration() -> void:
	var sim := _make_sim()
	sim.set_central_bank_policy("DEM", "controlled")
	for day in 20:
		# Déficit medido: o governo gasta em serviço doméstico antes do tick,
		# inclusive nos dias que cercam o save (emissão no dia do save).
		if day < 6 or day >= 17:
			sim.spend_government("AGRIA", 5000.0)
		sim.tick_day()
	var save: Dictionary = sim.to_dict()
	_check(int(save.get("version", 0)) == SAVE_VERSION,
		"save: versão %s != %d" % [str(save.get("version", 0)), SAVE_VERSION])
	var monetary: Dictionary = save.get("monetary", {})
	_check(not monetary.is_empty(), "save: bloco `monetary` ausente")
	var policies: Dictionary = monetary.get("policies", {})
	_check(policies.has("DEM"), "save: política da moeda DEM ausente")
	if policies.has("DEM"):
		var entry: Dictionary = policies["DEM"]
		_check(float(entry.get("emission", 0.0)) > 0.0,
			"save: emissão acumulada não pode ser 0 depois do déficit")
		_check(entry.has("rate") and entry.has("securities")
			and entry.has("last_day"),
			"save: política sem rate/securities/last_day")

	var copy = EconomySim.new()
	copy.set_goods(_json(GOODS_PATH))
	copy.set_recipes(_json(RECIPES_PATH))
	copy.set_needs(_json(NEEDS_PATH))
	copy.set_services(_json(SERVICES_PATH))
	copy.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	copy.set_age_structure(_json(AGE_STRUCTURE_PATH))
	copy.from_dict(save)
	_check(JSON.stringify(copy.to_dict(), "", true, true)
		== JSON.stringify(save, "", true, true),
		"save: round-trip v28 não é bit-exato")
	for day in 5:
		sim.spend_government("AGRIA", 3000.0)
		copy.spend_government("AGRIA", 3000.0)
		sim.tick_day()
		copy.tick_day()
	_check(JSON.stringify(sim.to_dict(), "", true, true)
		== JSON.stringify(copy.to_dict(), "", true, true),
		"save: continuação após load divergiu")
	_check(_close(float((_totals(sim)).get("emission_total", 0.0)),
		float((_totals(copy)).get("emission_total", 0.0)), EPS),
		"save: emissão divergiu depois do round-trip")

	# Migração: um save v27 (sem o bloco `monetary`) carrega com defaults.
	var old: Dictionary = save.duplicate(true)
	old["version"] = 27
	old.erase("monetary")
	var migrated = EconomySim.new()
	migrated.set_goods(_json(GOODS_PATH))
	migrated.set_recipes(_json(RECIPES_PATH))
	migrated.set_needs(_json(NEEDS_PATH))
	migrated.set_services(_json(SERVICES_PATH))
	migrated.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	migrated.set_age_structure(_json(AGE_STRUCTURE_PATH))
	migrated.from_dict(old)
	var migrated_save: Dictionary = migrated.to_dict()
	_check(int(migrated_save.get("version", 0)) == SAVE_VERSION,
		"migração: v27 deveria sair como v28 (saiu %s)"
			% str(migrated_save.get("version", 0)))
	var migrated_policy := _policy(migrated)
	_check(int(migrated_policy.get("last_day", 0)) == -1,
		"migração: taxa salva deveria nascer sem decisão")
	_check(_close(float(migrated_policy.get("rate", -1.0)),
		float(_json(MONETARY_PATH).get("defaults", {}).get("neutral_rate",
			-1.0)), 1e-12),
		"migração: taxa deveria resolver no neutro declarado (D-113)")
	var migrated_totals: Dictionary = _totals(migrated)
	_check(_close(float(migrated_totals.get("emission_total", -1.0)), 0.0,
		1e-12), "migração: emissão deveria ser 0 (nada inventado para trás)")
	_check(_close(float(migrated_totals.get("government_debt", -1.0)), 0.0,
		1e-12), "migração: dívida pública deveria ser 0")
	_check_day(migrated, "migração v27→v28")
