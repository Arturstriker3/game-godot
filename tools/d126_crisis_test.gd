extends SceneTree

## D-126 — sobrevivência em crise.
##
## Guarda o contrato da spec aprovada em 23/11/2026:
##   (1) demissão real com rescisão (folha/vagas caem, dinheiro conservado);
##   (2) crédito de capital de giro no banco existente, com 1 renegociação e
##       multa (criação de dinheiro bancário registrada);
##   (3) liquidação de estoque com desconto (venda ao exterior);
##   (4) hibernar sem produção com custo mínimo e reabrir pagando;
##   (5) falência vira leilão do prédio (comprador em vez de sumir);
##   (6) fazenda reduz a área para a próxima safra;
##   (7) save v41 com migração v40 e determinismo.
##
##   godot --headless --path . --script res://tools/d126_crisis_test.gd

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const EFFICIENCY_PATH := "res://data/definitions/production/efficiency.json"
const CRISIS_PATH := "res://data/definitions/economy/crisis.json"

const EPS := 1e-4

var _failures: Array[String] = []


func _init() -> void:
	_test_layoff()
	_test_credit()
	_test_renegotiation()
	_test_liquidation()
	_test_mothball_reopen()
	_test_auction()
	_test_farm_area()
	_test_refinery_and_power()
	_test_power_necessity()
	_test_ai_ladder()
	_test_save_load_v41()
	_test_v40_migration()
	_test_determinism()
	_finish()


# --- (1) demissão + rescisão ------------------------------------------------
func _test_layoff() -> void:
	var sim := _world()
	var built := _build(sim, "D126-LAY", 40.0, "flour", "wheat")
	if not built:
		return
	_tick(sim)
	var money_before := _money(sim)
	var row_before := _crisis_row(sim, "D126-LAY")
	var jobs_before := float(row_before.get("jobs", 0.0))
	var result: Dictionary = sim.crisis_layoff("AGRIA", "D126-LAY", 0.5)
	var row_after := _crisis_row(sim, "D126-LAY")
	var jobs_after := float(row_after.get("jobs", 0.0))
	_check("(1) demissão paga rescisão e reduz vagas",
			bool(result.get("ok", false)) and jobs_after < jobs_before,
			"antes=%.1f depois=%.1f %s" % [jobs_before, jobs_after, result])
	_check("(1) rescisão conserva dinheiro (sem criar/destruir)",
			absf(_money(sim) - money_before) <= EPS,
			"antes=%.4f depois=%.4f" % [money_before, _money(sim)])


# --- (2) crédito de capital de giro ----------------------------------------
func _test_credit() -> void:
	var sim := _world()
	if not _build(sim, "D126-CREDIT", 20.0, "flour", "wheat"):
		return
	_tick(sim)
	var money_before := _money(sim)
	var result: Dictionary = sim.crisis_borrow("AGRIA", 500.0)
	var granted := float(result.get("granted", 0.0))
	var debt := float(result.get("debt", 0.0))
	_check("(2) crédito de giro entra no caixa e vira dívida",
			bool(result.get("ok", false)) and granted > 0.0 and debt > 0.0,
			str(result))
	_check("(2) crédito cria dinheiro bancário do valor concedido",
			absf((_money(sim) - money_before) - granted) <= EPS,
			"delta=%.4f granted=%.4f" % [_money(sim) - money_before, granted])
	var banks: Dictionary = sim.get_bank_status()
	var loans := float(banks.get("totals", {}).get("loans", 0.0))
	_check("(2) banco contabiliza o empréstimo",
			loans + EPS >= granted, "loans=%.4f granted=%.4f" % [loans, granted])


# --- (2b) renegociação bancária do crédito de giro (15 d + multa 5%) --------
func _test_renegotiation() -> void:
	# Caminho do BANCO (não o atalho da crise): 15 dias de atraso disparam UMA
	# renegociação. Sem caixa, a multa de 5% é CAPITALIZADA no principal — o
	# tomador passa a dever mais e o ativo do banco cresce no mesmo valor; nenhum
	# dinheiro é criado. O status `penalty_paid` registra a multa cobrada.
	var sim := _world()
	# Um dia para o grupo ter lucro medido (base do limite de crédito).
	_tick(sim)
	var group_id := _first_group(sim)
	if group_id.is_empty():
		_fail("(2b) fixture sem grupo acionista")
		return
	sim.set_bank_capital("DEMO", 1.0e6)
	sim.set_bank_policy(0.02, 1.0, 2.0, 60, 0.5)
	var borrow: Dictionary = sim.group_borrow(group_id, 1.0e9)
	_check("(2b) empréstimo do grupo concedido",
		float(borrow.get("granted", 0.0)) > 0.0, str(borrow))
	if not float(borrow.get("granted", 0.0)) > 0.0:
		return
	# Depois de emprestar, hibernar as construções do grupo: sem receita, o
	# caixa não paga o juro e o atraso corre dia a dia.
	for row: Dictionary in sim.get_crisis_status().get("buildings", []):
		if String(row.get("owner", "")).begins_with("national:"):
			sim.crisis_mothball(String(row.get("city", "")),
				String(row.get("id", "")))
	# Estado de teste CONSERVADO: o caixa do grupo vai ao orçamento da cidade
	# (transferência; sem criar/destruir dinheiro).
	var data: Dictionary = sim.to_dict()
	var moved := 0.0
	for entry: Dictionary in data.get("groups", []):
		if String(entry.get("id", "")) == group_id:
			moved = float(entry.get("cash", 0.0))
			entry["cash"] = 0.0
	var cities: Array = data.get("cities", [])
	if not cities.is_empty():
		var city: Dictionary = cities[0]
		city["government_budget"] = float(city.get("government_budget", 0.0)) \
			+ moved
	var loaded := _load(data)
	loaded.set_bank_policy(0.02, 1.0, 2.0, 60, 0.5)
	var row := _bank_group(loaded, group_id)
	var debt_start := float(row.get("debt", 0.0))
	var rate := _loan_rate(loaded, "DEMO") \
		+ float(loaded.get_crisis_status().get("params", {}).get(
			"credit_rate_add", 0.0))
	var before := int(loaded.get_crisis_status().get("renegotiations", 0))
	var penalty_before := float(loaded.get_crisis_status().get("penalty_paid",
		0.0))
	var triggered := false
	for day: int in 40:
		loaded.tick_day()
		if int(loaded.get_crisis_status().get("renegotiations", 0)) > before:
			triggered = true
			break
	_check("(2b) 15 d de atraso disparam UMA renegociação", triggered,
		"renegociações=%d" % int(loaded.get_crisis_status().get(
			"renegotiations", 0)))
	if not triggered:
		return
	var after := _bank_group(loaded, group_id)
	var debt_after := float(after.get("debt", 0.0))
	_check("(2b) atraso zerado vira principal",
		absf(float(after.get("arrears", -1.0))) <= EPS,
		"arrears=" + str(after.get("arrears")))
	# Atraso esperado = juro diário (dívida parada) x 15 dias.
	var expected_arrears := debt_start * rate / 365.0 * 15.0
	var expected_fee := 0.05 * (debt_start + expected_arrears)
	var delta := debt_after - debt_start
	_check("(2b) multa de 5% capitalizada no principal",
		absf(delta - expected_arrears - expected_fee)
			<= maxf(1e-3, expected_fee * 0.02),
		"Δdívida=%.4f atraso=%.4f multa=%.4f" % [delta, expected_arrears,
			expected_fee])
	_check("(2b) multa registrada no status",
		float(loaded.get_crisis_status().get("penalty_paid", 0.0))
			- penalty_before > 0.0,
		"penalty=%.4f" % float(loaded.get_crisis_status().get("penalty_paid",
			0.0)))
	_check("(2b) capitalização não cria dinheiro (invariante fecha)",
		bool((loaded.get_balance_status().get("invariant", {}) as Dictionary).get(
			"ok", false)),
		str(loaded.get_balance_status().get("invariant", {})))


# --- (3) liquidação de estoque ---------------------------------------------
func _test_liquidation() -> void:
	var sim := _world()
	if not _build(sim, "D126-LIQ", 10.0, "wheat", ""):
		return
	sim.set_market_stock("AGRIA", "wheat", 10000.0)
	_tick(sim)
	var stock_before := _stock(sim, "AGRIA", "wheat")
	var price := _market_price(sim, "AGRIA", "wheat")
	var money_before := _money(sim)
	var external_before := _external_cash(sim)
	var result: Dictionary = sim.crisis_liquidate("AGRIA", "D126-LIQ", 1.0)
	var stock_after := _stock(sim, "AGRIA", "wheat")
	var value := float(result.get("value", 0.0))
	var tons := float(result.get("tons", 0.0))
	_check("(3) liquidação vende estoque com desconto",
			bool(result.get("ok", false))
				and tons > 0.0
				and stock_after < stock_before,
			str(result))
	# Mesmo caminho da exportação bruta (production.cpp): o exterior paga o
	# preço de mercado com o deságio declarado e o caixa do dono recebe.
	_check("(3) valor = toneladas x preço x (1 - deságio de 30%)",
			absf(value - tons * price * (1.0 - 0.30))
				<= maxf(1e-3, value * 1e-9),
			"valor=%.4f tons=%.4f preço=%.4f" % [value, tons, price])
	_check("(3) exterior paga a liquidação (foreign_cash cai pelo valor)",
			absf((_external_cash(sim) - external_before) + value)
				<= maxf(1e-6, value * 1e-12),
			"Δexterior=%.6f valor=%.6f" % [_external_cash(sim)
				- external_before, value])
	_check("(3) liquidação conserva dinheiro (comprador é o exterior)",
			absf(_money(sim) - money_before) <= EPS,
			"antes=%.4f depois=%.4f" % [money_before, _money(sim)])


# --- (4) hibernar e reabrir -------------------------------------------------
func _test_mothball_reopen() -> void:
	var sim := _world()
	if not _build(sim, "D126-MOTH", 20.0, "flour", "wheat", 5.0):
		return
	_tick(sim)
	var row := _crisis_row(sim, "D126-MOTH")
	var eff_before := float(row.get("production_efficiency", 1.0))
	var money_before := _money(sim)
	var result: Dictionary = sim.crisis_mothball("AGRIA", "D126-MOTH")
	row = _crisis_row(sim, "D126-MOTH")
	_check("(4) hibernar mantém o prédio e corta a eficiência (retenção 60%)",
			bool(result.get("ok", false)) and bool(row.get("mothballed", false))
				and float(row.get("production_efficiency", 1.0))
					<= eff_before * 0.60 + EPS,
			str(result) + " row=" + str(row))
	_check("(4) hibernar não move dinheiro",
			absf(_money(sim) - money_before) <= EPS,
			"antes=%.4f depois=%.4f" % [money_before, _money(sim)])
	money_before = _money(sim)
	result = sim.crisis_reopen("AGRIA", "D126-MOTH")
	row = _crisis_row(sim, "D126-MOTH")
	_check("(4) reabrir paga o custo e volta a operar",
			bool(result.get("ok", false))
				and not bool(row.get("mothballed", true)),
			str(result))
	_check("(4) reabertura conserva dinheiro",
			absf(_money(sim) - money_before) <= EPS,
			"antes=%.4f depois=%.4f" % [money_before, _money(sim)])


# --- (5) falência vira leilão ----------------------------------------------
func _test_auction() -> void:
	var sim := _world()
	if not _build(sim, "D126-AUC", 5.0, "flour", "wheat", 2.0):
		return
	_tick(sim)
	var money_before := _money(sim)
	var result: Dictionary = sim.crisis_auction("AGRIA", "D126-AUC")
	var row := _crisis_row(sim, "D126-AUC")
	var owner := String(row.get("owner", ""))
	_check("(5) leilão transfere o prédio (não apaga) com desconto",
			bool(result.get("ok", false)) and owner != "private"
				and owner != "",
			str(result) + " owner=" + owner)
	_check("(5) leilão conserva dinheiro", absf(_money(sim) - money_before) <= EPS,
			"antes=%.4f depois=%.4f" % [money_before, _money(sim)])


# --- (6) fazenda reduz a área ----------------------------------------------
func _test_farm_area() -> void:
	var sim := _world()
	# AGRIA do fixture não tem plano de safra de trigo; o plano declarado é
	# dado de teste (não um número de simulação do jogo).
	sim.set_crop_plan("AGRIA", "wheat", 10000.0, 3.0,
		[{"plant_start": 1, "plant_end": 60, "harvest_start": 61,
		"harvest_end": 365, "share": 1.0}])
	if not _build(sim, "D126-FARM", 30.0, "wheat", ""):
		return
	_tick(sim)
	sim.crisis_set_stress("AGRIA", "D126-FARM", 35)
	var before := int(sim.get_crisis_status().get("farm_area_reductions", 0))
	sim.crisis_step()
	var row := _crisis_row(sim, "D126-FARM")
	var after := int(sim.get_crisis_status().get("farm_area_reductions", 0))
	_check("(6) fazenda com prejuízo reduz a área da próxima safra",
			after > before and bool(row.get("farm_area_reduced", false)),
			"antes=%d depois=%d row=%s" % [before, after, str(row)])
	var save: Dictionary = sim.to_dict()
	var scale := _plan_area_scale(save, "AGRIA", "wheat")
	_check("(6) área reduzida entra no save (próxima safra)",
			scale < 1.0 and scale >= 0.10, "area_scale=%.4f" % scale)


# --- (9) refinaria e usina pelas mesmas regras ------------------------------
func _test_refinery_and_power() -> void:
	var sim := _world()
	if not _build(sim, "D126-REF", 30.0, "gasoline", "oil", 20.0, 12.0,
			"gasoline_a"):
		return
	_tick(sim)
	var lay: Dictionary = sim.crisis_layoff("AGRIA", "D126-REF", 0.5)
	_check("(9) refinaria demite pela mesma regra (rescisão paga)",
		bool(lay.get("ok", false))
			and float(lay.get("severance_paid", 0.0)) > 0.0, str(lay))
	var moth: Dictionary = sim.crisis_mothball("AGRIA", "D126-REF")
	_check("(9) refinaria hiberna pela mesma regra",
		bool(moth.get("ok", false))
			and bool(_crisis_row(sim, "D126-REF").get("mothballed", false)),
		str(moth))
	# Usina: mesmo gatilho de perda (loss_days), mesa de demissão/hibernação e
	# leilão por manutenção impaga. A usina-alvo é a EXCEDENTE (óleo): a de
	# carvão cobre sozinha o pico da rede e não é candidata (ver teste (11)).
	var power := _crisis_grid_world()
	var plants: Array = power.get_crisis_status().get("power_plants", [])
	if plants.is_empty():
		_fail("(9) fixture sem usina")
		return
	var plant_id := "PWR-AGRIA-oil"
	if _plant_row(power, plant_id).is_empty():
		_fail("(9) fixture sem usina excedente de óleo")
		return
	# Um dia roda o despacho: a rede precisa ter demanda medida para a regra
	# de necessidade valer (o estado de stress vem pelo save, como nos prédios).
	_tick(power)
	var jobs_before := float(_plant_row(power, plant_id).get("jobs", 0.0))
	var layoffs_before := int(power.get_crisis_status().get("layoffs", 0))
	power = _load(_plant_stress(power, plant_id, 25))
	power.crisis_step()
	var plant_row := _plant_row(power, plant_id)
	_check("(9) usina demite pela mesma regra",
		int(power.get_crisis_status().get("layoffs", 0)) > layoffs_before
			and float(plant_row.get("jobs", 1.0)) < jobs_before,
		"jobs=%.1f %s" % [float(plant_row.get("jobs", -1.0)),
			str(power.get_crisis_status().get("layoffs", 0))])
	var mothballs_before := int(power.get_crisis_status().get("mothballs", 0))
	power = _load(_plant_stress(power, plant_id, 55))
	power.crisis_step()
	plant_row = _plant_row(power, plant_id)
	_check("(9) usina hiberna pela mesma regra",
		int(power.get_crisis_status().get("mothballs", 0)) > mothballs_before
			and bool(plant_row.get("mothballed", false)), str(plant_row))
	# Manutenção impaga além da carência: vai a leilão. Com o Estado dono e sem
	# orçamento (estado de teste), não há comprador: a usina é removida (regra
	# antiga), e o contador de leilões sem comprador sobe.
	var unsold_before := int(power.get_crisis_status().get(
		"auctions_unsold", 0))
	var auctions_before := int(power.get_crisis_status().get("auctions", 0))
	power = _load(_plant_unpaid(power, plant_id))
	power.crisis_step()
	var unsold := int(power.get_crisis_status().get("auctions_unsold", 0)) \
		> unsold_before
	var bought := int(power.get_crisis_status().get("auctions", 0)) \
		> auctions_before
	_check("(9) usina sem manutenção vai a leilão (compra ou remoção)",
		unsold or bought, "sem_comprador=%s comprado=%s" % [unsold, bought])
	_check("(9) usina sem comprador sai do parque",
		not unsold or _plant_row(power, plant_id).is_empty(),
		str(_plant_row(power, plant_id)))


# --- (11) necessidade de pico: necessária não hiberna, excedente sim --------
func _test_power_necessity() -> void:
	# (a) Usina despachada ABAIXO da capacidade com receita cobrindo o custo
	# variável (o caixa do despacho: combustível + O&M + salários proporcionais)
	# não soma loss_days. A folha parada não é prejuízo de operação. O preço é
	# parâmetro de teste (price_scale) só para a receita ficar acima do custo
	# variável; o mecanismo medido é o contador.
	var partial := _crisis_grid_world()
	partial.set_power_params({"price_scale": 60.0})
	_tick(partial)
	var coal_row := _plant_row(partial, "PWR-AGRIA-coal")
	_check("(11) despacho parcial com receita >= custo variavel nao soma loss_days",
		int(coal_row.get("loss_days", -1)) == 0
			and not bool(coal_row.get("mothballed", true)),
		str(coal_row))
	# (b) Com o mesmo loss_days forçado, a usina NECESSÁRIA ao pico × reserva
	# não é candidata; a EXCEDENTE hiberna. Um dia roda o despacho para a rede
	# ter demanda medida (o estado vem do save, como nos testes de prédio).
	var base := _crisis_grid_world()
	_tick(base)
	var gate := _load(_plant_stress_many(base,
		["PWR-AGRIA-coal", "PWR-AGRIA-oil"], 55))
	var mothballs_before := int(gate.get_crisis_status().get("mothballs", 0))
	gate.crisis_step()
	coal_row = _plant_row(gate, "PWR-AGRIA-coal")
	var oil_row := _plant_row(gate, "PWR-AGRIA-oil")
	_check("(11) usina necessaria ao pico NAO hiberna",
		not coal_row.is_empty()
			and not bool(coal_row.get("mothballed", true)),
		str(coal_row))
	_check("(11) usina excedente hiberna",
		not oil_row.is_empty() and bool(oil_row.get("mothballed", false))
			and int(gate.get_crisis_status().get("mothballs", 0))
				> mothballs_before,
		str(oil_row))
	# (c) A demanda sobe (população de simulação maior): a usina hibernada
	# passa a ser necessária e reabre depois da histerese, pagando o custo.
	var raised: Dictionary = gate.to_dict()
	for city: Dictionary in raised.get("cities", []):
		city["sim_population"] = 2000000.0
	var grown := _load(raised)
	var reopens_before := int(grown.get_crisis_status().get("reopens", 0))
	for _day in 12:
		_tick(grown)
	oil_row = _plant_row(grown, "PWR-AGRIA-oil")
	_check("(11) usina hibernada reabre quando a demanda sobe",
		not oil_row.is_empty() and not bool(oil_row.get("mothballed", true))
			and int(grown.get_crisis_status().get("reopens", 0))
				> reopens_before,
		str(oil_row) + " reopens=" + str(
			grown.get_crisis_status().get("reopens", 0)))


# --- (10) escada automática da IA num cenário de crise ----------------------
func _test_ai_ladder() -> void:
	var sim := _world()
	# Planta grande com custo diário acima do caixa da cidade: o crédito de
	# giro (etapa 3) tem o que cobrir e as etapas seguem uma a uma.
	if not _build(sim, "D126-LAD", 400.0, "flour", "wheat", 100.0):
		return
	sim.set_market_stock("AGRIA", "wheat", 1000.0)
	sim.set_market_stock("AGRIA", "flour", 1000.0)
	_tick(sim)
	var before: Dictionary = sim.get_crisis_status()
	# Etapa 1 (cortar) é a regra existente de `loss_days` (produção), anterior à
	# fase de crise: com prejuízo abaixo do gatilho de demissão nada reage.
	sim.crisis_set_stress("AGRIA", "D126-LAD", 5)
	var status: Dictionary = sim.crisis_step()
	_check("(10) corte antes da demissão (nenhuma ação de crise)",
		int(status.get("layoffs", 0)) == int(before.get("layoffs", 0))
			and int(_crisis_row(sim, "D126-LAD").get("crisis_stage", 0)) == 0,
		str(_crisis_row(sim, "D126-LAD")))
	# Etapa 2: demitir (paga rescisão do caixa da cidade).
	sim.crisis_set_stress("AGRIA", "D126-LAD", 20)
	status = sim.crisis_step()
	_check("(10) demissão entra na escada (loss_days=20)",
		_key(status, "layoffs") > _key(before, "layoffs"),
		"layoffs=%.0f -> %.0f" % [_key(before, "layoffs"),
			_key(status, "layoffs")])
	before = status
	# Etapa 3: crédito de giro. A cidade fica sem caixa (transferência ao
	# orçamento) para o limite de giro ter o que cobrir; o save preserva a
	# demissão e a fase.
	var drained: Dictionary = sim.to_dict()
	_drain_private_cash(drained)
	sim = _load(drained)
	sim.crisis_set_stress("AGRIA", "D126-LAD", 30)
	status = sim.crisis_step()
	_check("(10) crédito de giro entra na escada (loss_days=30)",
		_key(status, "loans") > _key(before, "loans"),
		"loans=%.2f -> %.2f" % [_key(before, "loans"), _key(status, "loans")])
	before = status
	# Etapa 4: liquidar estoque (o comprador é o exterior).
	sim.crisis_set_stress("AGRIA", "D126-LAD", 40)
	status = sim.crisis_step()
	_check("(10) liquidação entra na escada (loss_days=40)",
		_key(status, "liquidations") > _key(before, "liquidations"),
		"liquidações=%.0f -> %.0f" % [_key(before, "liquidations"),
			_key(status, "liquidations")])
	before = status
	# Etapa 5: hibernar sem produção, pagando a manutenção mínima.
	sim.crisis_set_stress("AGRIA", "D126-LAD", 50)
	status = sim.crisis_step()
	_check("(10) hibernação entra na escada (loss_days=50)",
		_key(status, "mothballs") > _key(before, "mothballs"),
		"hibernações=%.0f -> %.0f" % [_key(before, "mothballs"),
			_key(status, "mothballs")])
	var row := _crisis_row(sim, "D126-LAD")
	_check("(10) hibernação mantém o prédio (etapa 5)",
		bool(row.get("mothballed", false)), str(row))
	# Etapa 6: manutenção impaga até a carência. O save carrega o estado de
	# teste (60 dias sem pagar), sem inventar regra.
	var data: Dictionary = sim.to_dict()
	_drain_private_cash(data)
	for entry: Dictionary in data.get("buildings", []):
		if String(entry.get("id", "")) == "D126-LAD":
			entry["mothball_unpaid_days"] = 60
	var loaded := _load(data)
	var auctions_before := int(loaded.get_crisis_status().get("auctions", 0))
	var unsold_before := int(loaded.get_crisis_status().get("auctions_unsold",
		0))
	loaded.crisis_step()
	var after: Dictionary = loaded.get_crisis_status()
	var bought := int(after.get("auctions", 0)) > auctions_before
	var unsold := int(after.get("auctions_unsold", 0)) > unsold_before
	row = _crisis_row(loaded, "D126-LAD")
	_check("(10) leilão fecha a escada (compra ou sem comprador)",
		bought or unsold, "comprado=%s sem_comprador=%s" % [bought, unsold])
	if bought:
		_check("(10) comprador assume o prédio (não some)",
			not row.is_empty() and String(row.get("owner", "")) != "private",
			str(row))


# --- (7) save v41 round-trip -----------------------------------------------
func _test_save_load_v41() -> void:
	var sim := _world()
	if not _build(sim, "D126-SAVE", 20.0, "flour", "wheat"):
		return
	_tick(sim)
	sim.crisis_layoff("AGRIA", "D126-SAVE", 0.5)
	sim.crisis_mothball("AGRIA", "D126-SAVE")
	var save: Dictionary = sim.to_dict()
	_check("(7) save v41", int(save.get("version", 0)) == 41,
			"version=%d" % int(save.get("version", 0)))
	var loaded := _load(save)
	var row := _crisis_row(loaded, "D126-SAVE")
	_check("(7) save/load preserva o estado de crise",
			bool(row.get("mothballed", false))
				and int(row.get("crisis_stage", 0)) >= 2,
			str(row))
	_check("(7) save/load preserva o dinheiro",
			absf(_money(loaded) - _money(sim)) <= EPS,
			"%.4f vs %.4f" % [_money(loaded), _money(sim)])
	var save2: Dictionary = loaded.to_dict()
	_check("(7) segundo save idêntico (round-trip estável)",
			JSON.stringify(save) == JSON.stringify(save2), "")


func _test_v40_migration() -> void:
	var sim := _world()
	_tick(sim)
	var save: Dictionary = sim.to_dict()
	save["version"] = 40
	save.erase("crisis")
	var loaded := _load(save)
	_check("(7) migração v40->v41 carrega estado neutro",
			loaded != null and int(loaded.get_status().get("day", -1)) >= 1,
			"")
	var status: Dictionary = loaded.get_crisis_status()
	_check("(7) v40 sem crise em curso",
			int(status.get("mothballs", -1)) == 0
				and int(status.get("layoffs", -1)) == 0,
			str(status.get("mothballs", -1)))


# --- (8) determinismo -------------------------------------------------------
func _test_determinism() -> void:
	var a := _deterministic_run()
	var b := _deterministic_run()
	_check("(8) mesma sequência = mesmo save", a == b and a != "",
			"len=%d/%d" % [a.length(), b.length()])


func _deterministic_run() -> String:
	var sim := _world()
	if not _build(sim, "D126-DET", 20.0, "flour", "wheat"):
		return ""
	_tick(sim)
	sim.crisis_layoff("AGRIA", "D126-DET", 0.5)
	sim.crisis_set_stress("AGRIA", "D126-DET", 35)
	sim.crisis_step()
	return JSON.stringify(sim.to_dict(), "", true, true)


# --- helpers ---------------------------------------------------------------
func _world() -> Object:
	var sim = EconomySim.new()
	sim.set_sim_threads(1)
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	var params: Variant = _json(EFFICIENCY_PATH)
	if params is Dictionary and sim.has_method("set_efficiency_params"):
		sim.set_efficiency_params(params)
	var crisis: Variant = _json(CRISIS_PATH)
	if crisis is Dictionary and sim.has_method("set_crisis_params"):
		sim.set_crisis_params(crisis)
	# O jogo carrega bank_credit.json (enabled=true); o teste liga a mecânica
	# explicitamente para o crédito de giro ter contraparte bancária.
	if sim.has_method("set_bank_credit"):
		sim.set_bank_credit(true)
	sim.initialize_scenario()
	sim.set_ai_enabled(false)
	sim.set_instant_construction(true)
	return sim


## Mundo com uma usina a gás de 50 MW (mesmo caminho do dado de energia). O
## fixture não traz parque; o dado é de teste, não de simulação do jogo.
func _power_world() -> Object:
	var sim := _world()
	var nations := {"DEMO": {
		"access_pct": 100.0, "access_urban_pct": 100.0,
		"access_rural_pct": 100.0, "loss_pct": 4.0,
	}}
	var cities := {"AGRIA": {
		"access_pct": 100.0, "loss_pct": 4.0,
		"park": {"gas": 50.0}, "cap": {}, "solar_month": [],
	}}
	sim.set_power_definitions(nations, cities, {}, true)
	sim.set_power_city("AGRIA", 0, 0.0, 100.0)
	return sim


## Mundo de teste da correção D-126: carvão de 500 MW cobre sozinho o pico da
## rede e é a usina NECESSÁRIA; óleo de 50 MW (mais caro no mérito) é a
## EXCEDENTE. Os parques são dado de teste, não número de simulação do jogo.
func _crisis_grid_world() -> Object:
	var sim := _world()
	var nations := {"DEMO": {
		"access_pct": 100.0, "access_urban_pct": 100.0,
		"access_rural_pct": 100.0, "loss_pct": 4.0,
	}}
	var cities := {"AGRIA": {
		"access_pct": 100.0, "loss_pct": 4.0,
		"park": {"coal": 500.0, "oil": 50.0}, "cap": {}, "solar_month": [],
	}}
	sim.set_power_definitions(nations, cities, {}, true)
	sim.set_power_city("AGRIA", 0, 0.0, 100.0)
	return sim


func _build(sim: Object, id: String, jobs: float, output: String,
		input: String, capacity: float = 10.0, opex: float = 10.0,
		recipe_override: String = "") -> bool:
	var output_index := _product(sim, output)
	if output_index < 0:
		_fail("produto %s inexistente" % output)
		return false
	var recipe := recipe_override
	if recipe.is_empty():
		recipe = "" if input == "" else output
	var built: Dictionary = sim.build("AGRIA", id, input if input != "" else output,
			_product(sim, input) if input != "" else -1, output_index,
			capacity, jobs, 2.0, opex, 0.0, "private", 1000.0, recipe)
	if not bool(built.get("created", false)):
		_fail("construção %s não criada: %s" % [id, built])
		return false
	return true


func _tick(sim: Object) -> void:
	sim.tick_day()


func _load(save: Dictionary) -> Object:
	var sim = EconomySim.new()
	sim.set_sim_threads(1)
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	var crisis: Variant = _json(CRISIS_PATH)
	if crisis is Dictionary and sim.has_method("set_crisis_params"):
		sim.set_crisis_params(crisis)
	sim.from_dict(save)
	return sim


func _crisis_row(sim: Object, id: String) -> Dictionary:
	for row: Variant in sim.get_crisis_status().get("buildings", []):
		var building: Dictionary = row
		if String(building.get("id", "")) == id:
			return building
	return {}


func _plan_area_scale(save: Dictionary, city_id: String, product: String) -> float:
	for row: Variant in save.get("agriculture", {}).get("plans", []):
		var plan: Dictionary = row
		if String(plan.get("city", "")) == city_id \
				and String(plan.get("product", "")) == product:
			return float(plan.get("area_scale", 1.0))
	return 1.0


func _product(sim: Object, product: String) -> int:
	return Array(sim.get_status().get("products", [])).find(product)


func _money(sim: Object) -> float:
	return float(sim.get_status().get("money_total", 0.0))


func _stock(sim: Object, city_id: String, product: String) -> float:
	var index := _product(sim, product)
	var markets: Array = sim.get_city_status(city_id).get("markets", [])
	if index < 0 or index >= markets.size():
		return 0.0
	return float((markets[index] as Dictionary).get("stock", 0.0))


func _market_price(sim: Object, city_id: String, product: String) -> float:
	var index := _product(sim, product)
	var markets: Array = sim.get_city_status(city_id).get("markets", [])
	if index < 0 or index >= markets.size():
		return 0.0
	return float((markets[index] as Dictionary).get("price", 0.0))


## Posição do setor externo (`foreign_cash`) pelo balanço: é o comprador da
## exportação bruta e da liquidação de crise.
func _external_cash(sim: Object) -> float:
	var world: Dictionary = sim.get_balance_status().get("world", {})
	var external: Dictionary = world.get("external", {})
	return float(external.get("cash", 0.0))


func _first_group(sim: Object) -> String:
	var groups: Array = sim.get_groups()
	if groups.is_empty():
		return ""
	return String((groups[0] as Dictionary).get("id", ""))


func _bank_group(sim: Object, group_id: String) -> Dictionary:
	for row: Variant in sim.get_bank_status().get("groups", []):
		var group: Dictionary = row
		if String(group.get("id", "")) == group_id:
			return group
	return {}


func _credit_city(sim: Object, city_id: String) -> Dictionary:
	for row: Variant in sim.get_crisis_status().get("cities", []):
		var city: Dictionary = row
		if String(city.get("city", "")) == city_id:
			return city
	return {}


func _loan_rate(sim: Object, nation: String) -> float:
	for row: Variant in sim.get_bank_status().get("nations", []):
		var entry: Dictionary = row
		if String(entry.get("nation", "")) == nation:
			return float(entry.get("loan_rate", 0.0))
	return 0.0


func _key(status: Dictionary, key: String) -> float:
	return float(status.get(key, 0.0))


func _plant_row(sim: Object, plant_id: String) -> Dictionary:
	for row: Variant in sim.get_crisis_status().get("power_plants", []):
		var plant: Dictionary = row
		if String(plant.get("id", "")) == plant_id:
			return plant
	return {}


## Save com o estado de teste de uma usina (dias de prejuízo forçados), no
## mesmo formato que o núcleo grava — o teste não inventa regra, só a entrada.
func _plant_stress(sim: Object, plant_id: String, loss_days: int) -> Dictionary:
	var data: Dictionary = sim.to_dict()
	for entry: Dictionary in (data.get("power", {}) as Dictionary).get(
			"plants", []):
		if String(entry.get("id", "")) == plant_id:
			entry["loss_days"] = loss_days
	return data


## Mesmo estado de teste para várias usinas (o mecanismo é por rede: o teste
## precisa da necessária e da excedente no mesmo dia).
func _plant_stress_many(sim: Object, plant_ids: Array,
		loss_days: int) -> Dictionary:
	var data: Dictionary = sim.to_dict()
	for entry: Dictionary in (data.get("power", {}) as Dictionary).get(
			"plants", []):
		if plant_ids.has(String(entry.get("id", ""))):
			entry["loss_days"] = loss_days
	return data


## Usina hibernada com 60 dias de manutenção impaga e a cidade sem orçamento
## (estado de teste): o leilão é a regra, não um atalho do teste.
func _plant_unpaid(sim: Object, plant_id: String) -> Dictionary:
	var data: Dictionary = sim.to_dict()
	for entry: Dictionary in (data.get("power", {}) as Dictionary).get(
			"plants", []):
		if String(entry.get("id", "")) == plant_id:
			entry["mothball_unpaid_days"] = 60
	for city: Dictionary in data.get("cities", []):
		city["government_budget"] = 0.0
	return data


## Move o caixa das firmas para o orçamento (estado de teste conservado).
func _drain_private_cash(data: Dictionary) -> void:
	for city: Dictionary in data.get("cities", []):
		var cash := float(city.get("firm_cash", 0.0))
		city["firm_cash"] = 0.0
		city["government_budget"] = float(city.get("government_budget", 0.0)) \
			+ cash


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _check(name: String, ok: bool, detail: String = "") -> void:
	if ok:
		print("[d126] ok: " + name)
	else:
		_fail("%s (%s)" % [name, detail])


func _fail(message: String) -> void:
	_failures.append(message)


func _finish() -> void:
	if _failures.is_empty():
		print("[d126] OK: demissão, crédito de giro, renegociação com multa, "
			+ "liquidação via exterior, hibernação/reabertura, leilão (prédio, "
			+ "refinaria e usina), área da fazenda, escada da IA, save v41 e "
			+ "determinismo.")
		quit(0)
	else:
		print("[d126] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)
