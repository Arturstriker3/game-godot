extends SceneTree

## G2 (D-101) — testes da ARMAZENAGEM (armazéns agregados, reserva nacional,
## pulmão região→cidade, IA privada de armazém, IPC e save v25).
##
## Uso: godot --headless --path . --script res://tools/storage_g2_test.gd
##
## Blocos:
##   (1) StorageService: lê os três JSONs da G2, aplica 113/113 produtos,
##       capacidade por país rateada por produção/população, reserva pública do
##       dia 1, ouro reportado e `only_missing` sem reescrever nada;
##   (2) conservação de DINHEIRO e de BENS na compra/liberação/guarda;
##   (3) capacidade limita a compra e a liberação só entrega o que foi retido;
##   (4) pulmão região→cidade: a colheita entra no silo e sai aos poucos;
##   (5) IA privada constrói quando o spread paga e não causa fome em paz;
##   (6) IPC e dias de reserva na camada nacional (leitura);
##   (7) round-trip v25 e migração v24→v25 (sem inventar capacidade/tonelada);
##   (8) reserva nacional em ilha/micro-mercado: cidade com comida curta não
##       perde comida para o silo do governo (compra só do excedente) e a
##       reserva libera onde falta.

const GeoScript := preload("res://core/economy/geography_service.gd")
const StorageScript := preload("res://core/economy/storage_service.gd")
const GroupNamerScript := preload("res://core/economy/group_namer.gd")

const AGRIA_CENTER := Vector2(2532.6, 571.4)
const PORTUS_CENTER := Vector2(2560.4, 543.8)
const SAVE_VERSION := 25
## Tolerância da conservação (XGS ou t) — o mundo demo roda em double.
const EPS := 1e-6

var _failures: Array[String] = []
var _checks := 0


func _init() -> void:
	_test_service()
	_test_conservation()
	_test_capacity_and_release()
	_test_pulmao()
	_test_ai_storage()
	_test_ipc()
	_test_roundtrip_and_migration()
	_test_government_reserve_keeps_food()
	if _failures.is_empty():
		print("[storage] OK: %d verificações (service, conservação, capacidade, "
			% _checks + "pulmão, IA, IPC, save v25, reserva em ilha).")
		quit(0)
	else:
		print("[storage] FALHAS (%d de %d):" % [_failures.size(), _checks])
		for failure in _failures:
			print(" - " + failure)
		quit(1)


# ---------------------------------------------------------------------------
# (1) StorageService
# ---------------------------------------------------------------------------

func _test_service() -> void:
	var world := _make_world(false)
	var sim = world["sim"]
	# Cidades sintéticas: AGRIA vira PRT e PORTUS vira USA para exercitar o dado
	# real sem carregar o mundo inteiro. A PRODUÇÃO do jogo abaixo é o volume ao
	# qual a cobertura real se aplica (D-101): trigo e batata em AGRIA, trigo,
	# petróleo e gás em PORTUS.
	var cities: Array = [
		{"id": "AGRIA", "nation": "PRT", "population": 10804871.0},
		{"id": "PORTUS", "nation": "USA", "population": 340000000.0},
	]
	var buildings: Array = [
		{"city_id": "AGRIA", "resource": "wheat", "output_tons_per_day": 100.0},
		{"city_id": "AGRIA", "resource": "potato", "output_tons_per_day": 40.0},
		{"city_id": "PORTUS", "resource": "wheat", "output_tons_per_day": 300.0},
		{"city_id": "PORTUS", "resource": "oil", "output_tons_per_day": 200.0},
		{"city_id": "PORTUS", "resource": "gas", "output_tons_per_day": 90.0},
	]
	var storage := StorageScript.new()
	var report: Dictionary = storage.apply_to_sim(sim, cities, buildings)
	_check(bool(report.get("applied", false)), "service: aplicado no mundo demo")
	_check(int(report.get("product_types_applied", 0)) == 113,
		"service: 113/113 produtos mapeados (%d)"
		% int(report.get("product_types_applied", 0)))
	_check(int(report.get("product_types_total", 0)) == 113,
		"service: total de produtos do mapa = 113")
	_check(Array(report.get("product_types_skipped", [])).is_empty(),
		"service: nenhum produto sem tipo")

	# O mapa chegou ao núcleo: gás é "gas" (não "gas_storage") e ouro é "vault".
	var types: Dictionary = sim.get_storage_product_types()
	_check(String(types.get("gas", "")) == "gas",
		"service: produto gas → tipo gas (é %s)" % String(types.get("gas", "")))
	_check(String(types.get("gold", "")) == "vault",
		"service: produto gold → vault (é %s)" % String(types.get("gold", "")))
	_check(String(types.get("wheat", "")) == "silo",
		"service: produto wheat → silo")
	# O mundo demo tem 21 produtos: o mapa vale para os que existem nele.
	_check(String(types.get("iron", "")) == "bulk_yard",
		"service: produto iron → bulk_yard")
	_check(String(types.get("rice", "")) == "silo",
		"service: produto rice → silo")

	# Capacidade: total = privado + governo em cada tipo (repartição exata).
	var capacity: Dictionary = report["capacity_t"]
	var government: Dictionary = report["government_capacity_t"]
	var private: Dictionary = report["private_capacity_t"]
	for type in capacity.keys():
		var total := float(capacity[type])
		var split := float(government.get(type, 0.0)) \
			+ float(private.get(type, 0.0))
		_check(absf(total - split) <= maxf(1.0, absf(total) * 1e-9),
			"service: capacidade %s = governo + privado (%.3f vs %.3f)"
			% [String(type), total, split])

	# D-101 (escala): a capacidade é COBERTURA REAL × FLUXO DO JOGO. O dado
	# real de grãos do PRT (0,95871 Mt) NÃO entra como tonelagem absoluta: com
	# 36.500 t/ano de trigo no jogo a capacidade fica na casa das dezenas de
	# milhares de toneladas e a cobertura em ~13 meses (a faixa real), não 89.
	var coverage: Dictionary = report["coverage_months"]
	var months: Dictionary = report["months_of_game_production"]
	var silo_months := float(coverage.get("silo", 0.0))
	_check(silo_months > 6.0 and silo_months < 24.0,
		"escala: grãos cobrem a faixa real de meses (%.2f meses)" % silo_months)
	_check(absf(float(months.get("silo", 0.0)) - silo_months)
		<= maxf(1e-6, silo_months * 1e-6),
		"escala: meses do jogo = cobertura real (%.3f vs %.3f)"
		% [float(months.get("silo", 0.0)), silo_months])
	_check(float(capacity.get("silo", 0.0)) < 1e6,
		"escala: silo do mundo sintético não herda a tonelagem absoluta (%.0f t)"
		% float(capacity.get("silo", 0.0)))
	_check(float((report.get("real_data", {}) as Dictionary)
		.get("grain_mt", 0.0)) > 1.0,
		"escala: o dado real de grãos é lido e reportado")
	_check(absf(float(coverage.get("cold", 0.0))
		- float(StorageScript.STORAGE_INITIAL_MONTHS)) <= 1e-9,
		"escala: câmara fria usa a regra declarada (%.1f meses)"
		% float(coverage.get("cold", 0.0)))

	# PRT: silo de governo = 25%% da capacidade (sem rede pública publicada) e a
	# capacidade total = meses de grãos do país × fluxo do jogo / 12.
	var prt_silo := _site(sim, "AGRIA", "silo", "government")
	_check(not prt_silo.is_empty(), "service: PRT ganhou silo de governo")
	if not prt_silo.is_empty():
		var prt_private := _site(sim, "AGRIA", "silo", "private")
		var prt_total := float(prt_silo["capacity_t"]) \
			+ float(prt_private.get("capacity_t", 0.0))
		var prt_months := prt_total * 12.0 / (100.0 * 365.0)
		# 13,208404 meses = months_of_production do PRT no JSON de capacidade.
		_check(absf(prt_months - 13.208404) <= 1e-4,
			"service: silo de PRT cobre os 13,2 meses do dado (%.4f)"
			% prt_months)
		var expected_gov := prt_total \
			* float(StorageScript.STORAGE_GOVERNMENT_SHARE)
		_check(absf(float(prt_silo["capacity_t"]) - expected_gov)
			<= expected_gov * 1e-6,
			"service: silo gov de PRT = 25%% da cobertura (%.1f vs %.1f)"
			% [float(prt_silo["capacity_t"]), expected_gov])
	var prt_cold := _site(sim, "AGRIA", "cold", "private")
	_check(not prt_cold.is_empty(), "service: PRT ganhou câmara fria")
	if not prt_cold.is_empty():
		var cold_flow := 40.0 * 365.0
		var expected_cold := cold_flow \
			* float(StorageScript.STORAGE_INITIAL_MONTHS) / 12.0 \
			* (1.0 - float(StorageScript.STORAGE_GOVERNMENT_SHARE))
		_check(absf(float(prt_cold["capacity_t"]) - expected_cold)
			<= expected_cold * 1e-6,
			"service: frio privado de PRT = 2 meses × 75%% (%.1f vs %.1f)"
			% [float(prt_cold["capacity_t"]), expected_cold])

	# USA: gás do dado (1.493 TWh) dividido pela produção real dos EUA dá a
	# cobertura; a capacidade do jogo é essa cobertura × fluxo de gás do jogo.
	var usa_gas := _site(sim, "PORTUS", "gas", "private")
	_check(not usa_gas.is_empty(), "service: USA ganhou armazém de gás")
	var type_rows: Array = sim.get_storage_status().get("types", [])
	var gas_capacity := 0.0
	for row in type_rows:
		if String((row as Dictionary).get("type", "")) == "gas":
			gas_capacity = float((row as Dictionary).get("capacity_t", 0.0))
	var gas_months := float(coverage.get("gas", 0.0))
	_check(gas_months > 0.5 and gas_months < 6.0,
		"service: gás dos EUA cobre poucos meses da produção real (%.2f)"
		% gas_months)
	var gas_flow := 90.0 * 365.0
	_check(absf(gas_capacity - gas_months * gas_flow / 12.0)
		<= maxf(1e-6, gas_capacity * 1e-6),
		"service: gás = cobertura × fluxo do jogo (%.1f t)" % gas_capacity)

	var fuel_gov := _site(sim, "PORTUS", "fuel_tank", "government")
	_check(not fuel_gov.is_empty(), "service: USA ganhou tanque de governo")
	if not fuel_gov.is_empty():
		var by_product: Dictionary = fuel_gov.get("by_product", {})
		var oil_stock := float(by_product.get("oil", 0.0))
		# 125 dias de importação líquida (dado EIA) sobre o fluxo de óleo do jogo.
		var oil_flow := 200.0 * 365.0
		var expected_oil := 125.0 / 365.0 * oil_flow
		_check(absf(oil_stock - expected_oil) <= maxf(1.0, expected_oil * 1e-6),
			"service: reserva pública de petróleo dos EUA = 125 dias do fluxo "
			+ "do jogo (%.1f vs %.1f)" % [oil_stock, expected_oil])
		_check(float(fuel_gov["capacity_t"]) >= oil_stock,
			"service: capacidade do tanque público cobre a reserva (%.0f t)"
			% float(fuel_gov["capacity_t"]))
	_check(absf(float((report["initial_stock_days"] as Dictionary)
		.get("oil", 0.0)) - 125.0) <= 1e-6,
		"service: cobertura da reserva de petróleo = 125 dias")
	_check(float(report.get("gold_reserve_t", 0.0)) > 8000.0,
		"service: ouro do BC lido (%.1f t)" % float(report.get("gold_reserve_t", 0.0)))
	_check(bool(report.get("gold_applied", true)) == false,
		"service: ouro NÃO aplicado (cofre é estrutura nesta parte)")
	var gold_nations: Dictionary = report.get("gold_nations", {})
	_check(absf(float(gold_nations.get("USA", 0.0)) - 8133.5258) < 0.01,
		"service: ouro dos EUA = 8.133,53 t do IMF IFS")

	# Fallback declarado: sem dado de gás o tipo fica 0 (regra do arquivo).
	_check(float((report["capacity_source"].get("gas", {}) as Dictionary)
		.get("rule", 0.0)) <= 1e-6,
		"service: gás sem dado não cai na regra (arquivo diz 0)")

	# Rateio: capacidade só em cidade com peso (produção ou população).
	_check(float(report["capacity_unplaced_t"].get("silo", 0.0)) >= 0.0,
		"service: capacidade não acomodada é reportada")

	# `only_missing`: nada é reescrito (load de save v25 manda).
	var before: Dictionary = sim.get_storage_status().get("totals", {})
	var again: Dictionary = storage.apply_to_sim(sim, cities, buildings,
		{"only_missing": true})
	_check(int(again.get("sites_written", -1)) == 0,
		"service: only_missing não escreve site existente (%d)"
		% int(again.get("sites_written", -1)))
	var after: Dictionary = sim.get_storage_status().get("totals", {})
	_check(absf(float(before.get("capacity_t", 0.0))
		- float(after.get("capacity_t", 0.0))) <= EPS,
		"service: only_missing preserva a capacidade total")

	# Diagnóstico de escala exposto (dívida de calibração declarada).
	_check(months.has("silo") and float(months["silo"]) > 0.0,
		"service: meses de produção do jogo por tipo reportados")


# ---------------------------------------------------------------------------
# (2) Conservação de dinheiro e de bens
# ---------------------------------------------------------------------------

func _test_conservation() -> void:
	var world := _make_world(false)
	var sim = world["sim"]
	sim.set_storage_product_type("wheat", "silo")
	sim.set_storage_capacity("AGRIA", "silo", "government", 1000.0)
	sim.set_storage_capacity("AGRIA", "silo", "private", 1000.0)
	_inject_market(sim, "AGRIA", "wheat", 500.0)
	_set_cash(sim, "AGRIA", 1_000_000.0, 1_000_000.0)
	var wheat := _product(sim, "wheat")
	_check(wheat >= 0, "conservação: produto wheat resolvido (%d)" % wheat)

	var money0 := _money(sim)
	var wheat0 := _total_wheat(sim, "AGRIA", wheat)
	# Compra do governo: tira do mercado e paga o vendedor (caixa→caixa).
	var buy: Dictionary = sim.storage_trade("AGRIA", "silo", "government",
		"wheat", 200.0, true, false)
	_check(bool(buy.get("ok", false)), "conservação: compra ok (%s)"
		% String(buy.get("error", "")))
	_check(absf(float(buy.get("tons", 0.0)) - 200.0) <= 1e-6,
		"conservação: comprou 200 t (%.3f)" % float(buy.get("tons", 0.0)))
	_check(absf(_storage_stock(sim, "AGRIA", "silo", "government", "wheat")
		- 200.0) <= 1e-6, "conservação: silo do governo com 200 t")
	_check(absf(_market(sim, "AGRIA", wheat) - 300.0) <= 1e-6,
		"conservação: mercado perdeu 200 t (%.3f)"
		% _market(sim, "AGRIA", wheat))
	_check(absf(_money(sim) - money0) <= maxf(EPS, absf(money0) * 1e-9),
		"conservação: compra não cria nem destrói dinheiro (%.6f)"
		% (_money(sim) - money0))
	_check(absf(_total_wheat(sim, "AGRIA", wheat) - wheat0) <= 1e-6,
		"conservação: compra não cria nem destrói trigo (%.6f)"
		% (_total_wheat(sim, "AGRIA", wheat) - wheat0))

	# Liberação paga: o comprador (firma) paga o dono (governo).
	var release: Dictionary = sim.storage_trade("AGRIA", "silo", "government",
		"wheat", 50.0, false, false)
	_check(bool(release.get("ok", false)), "conservação: liberação ok")
	_check(float(release.get("value", 0.0)) > 0.0,
		"conservação: liberação paga o dono (%.2f XGS)"
		% float(release.get("value", 0.0)))
	_check(absf(_storage_stock(sim, "AGRIA", "silo", "government", "wheat")
		- 150.0) <= 1e-6, "conservação: silo ficou com 150 t")
	_check(absf(_money(sim) - money0) <= maxf(EPS, absf(money0) * 1e-9),
		"conservação: liberação paga não muda o total de dinheiro")
	_check(absf(_total_wheat(sim, "AGRIA", wheat) - wheat0) <= 1e-6,
		"conservação: liberação paga não muda o total de trigo")

	# Socorro (`relief`): o mercado recebe a tonelada e ninguém paga.
	var relief: Dictionary = sim.storage_trade("AGRIA", "silo", "government",
		"wheat", 50.0, false, true)
	_check(bool(relief.get("ok", false)), "conservação: socorro ok")
	_check(absf(float(relief.get("value", -1.0))) <= 1e-9,
		"conservação: socorro não movimenta dinheiro")
	_check(absf(float(relief.get("free_tons", 0.0)) - 50.0) <= 1e-6,
		"conservação: socorro entrega 50 t de graça")
	_check(absf(_money(sim) - money0) <= maxf(EPS, absf(money0) * 1e-9),
		"conservação: socorro não muda o total de dinheiro")
	_check(absf(_total_wheat(sim, "AGRIA", wheat) - wheat0) <= 1e-6,
		"conservação: socorro não muda o total de trigo")

	# Guarda privada: a firma compra do próprio mercado (mesma caixa).
	var firm_buy: Dictionary = sim.storage_trade("AGRIA", "silo", "private",
		"wheat", 100.0, true, false)
	_check(bool(firm_buy.get("ok", false)), "conservação: compra privada ok")
	_check(absf(_money(sim) - money0) <= maxf(EPS, absf(money0) * 1e-9),
		"conservação: compra privada não muda o total de dinheiro")
	_check(absf(_total_wheat(sim, "AGRIA", wheat) - wheat0) <= 1e-6,
		"conservação: compra privada não muda o total de trigo")

	# Obra: vira receita do setor de construção (dinheiro conservado).
	var build: Dictionary = sim.storage_build("AGRIA", "silo", "private", 500.0)
	_check(bool(build.get("started", false)), "conservação: obra iniciada (%s)"
		% String(build.get("error", "")))
	_check(absf(float(build.get("capacity_added", 0.0)) - 500.0) <= 1e-6,
		"conservação: obra entrega 500 t de capacidade")
	_check(absf(float(build.get("cost", 0.0)) - 500.0 * 35.0) <= 1e-6,
		"conservação: custo da obra = 500 t × 35 XGS/t (%.2f)"
		% float(build.get("cost", 0.0)))
	_check(absf(_money(sim) - money0) <= maxf(EPS, absf(money0) * 1e-9),
		"conservação: obra não cria nem destrói dinheiro (%.6f)"
		% (_money(sim) - money0))
	_check(absf(_site_capacity(sim, "AGRIA", "silo", "private") - 1500.0)
		<= 1e-6, "conservação: capacidade privada = 1000 + 500 t")


# ---------------------------------------------------------------------------
# (3) Capacidade limita; liberação só entrega o retido
# ---------------------------------------------------------------------------

func _test_capacity_and_release() -> void:
	var world := _make_world(false)
	var sim = world["sim"]
	sim.set_storage_product_type("wheat", "silo")
	sim.set_storage_capacity("AGRIA", "silo", "government", 100.0)
	_inject_market(sim, "AGRIA", "wheat", 500.0)
	_set_cash(sim, "AGRIA", 1_000_000.0, 1_000_000.0)

	var buy: Dictionary = sim.storage_trade("AGRIA", "silo", "government",
		"wheat", 500.0, true, false)
	_check(bool(buy.get("ok", false)), "capacidade: compra aceita")
	_check(absf(float(buy.get("tons", 0.0)) - 100.0) <= 1e-6,
		"capacidade: limitou a compra a 100 t (pediu 500, entrou %.2f)"
		% float(buy.get("tons", 0.0)))
	_check(absf(_storage_stock(sim, "AGRIA", "silo", "government", "wheat")
		- 100.0) <= 1e-6, "capacidade: estoque guardado = capacidade")
	var full: Dictionary = sim.storage_trade("AGRIA", "silo", "government",
		"wheat", 50.0, true, false)
	_check(not bool(full.get("ok", true)),
		"capacidade: cheio recusa a compra (%s)" % String(full.get("error", "")))
	_check(absf(_storage_stock(sim, "AGRIA", "silo", "government", "wheat")
		- 100.0) <= 1e-6, "capacidade: recusa não estoura o estoque")

	# Liberar mais do que existe: só sai o retido.
	var over: Dictionary = sim.storage_trade("AGRIA", "silo", "government",
		"wheat", 500.0, false, true)
	_check(bool(over.get("ok", false)), "capacidade: liberação limitada ok")
	_check(absf(float(over.get("tons", 0.0)) - 100.0) <= 1e-6,
		"capacidade: liberou só as 100 t retidas (%.2f)"
		% float(over.get("tons", 0.0)))
	_check(absf(_storage_stock(sim, "AGRIA", "silo", "government", "wheat"))
		<= 1e-6, "capacidade: silo esvaziou")
	var empty: Dictionary = sim.storage_trade("AGRIA", "silo", "government",
		"wheat", 10.0, false, true)
	_check(not bool(empty.get("ok", true)), "capacidade: vazio recusa liberação")

	# Tipo errado para o produto e tipo só de estrutura: recusa declarada.
	var wrong: Dictionary = sim.storage_trade("AGRIA", "cold", "government",
		"wheat", 10.0, true, false)
	_check(not bool(wrong.get("ok", true)),
		"capacidade: trigo não entra em câmara fria (%s)"
		% String(wrong.get("error", "")))
	sim.set_storage_capacity("AGRIA", "vault", "government", 100.0)
	var vault: Dictionary = sim.storage_trade("AGRIA", "vault", "government",
		"gold", 10.0, true, false)
	_check(not bool(vault.get("ok", true)),
		"capacidade: cofre é estrutura, não guarda estoque (%s)"
		% String(vault.get("error", "")))


# ---------------------------------------------------------------------------
# (4) Pulmão região→cidade
# ---------------------------------------------------------------------------

func _test_pulmao() -> void:
	var world := _make_world(false)
	var sim = world["sim"]
	sim.set_storage_product_type("wheat", "silo")
	# Silo PRIVADO na cidade (é o silo regional na arquitetura: fica no polo).
	sim.set_storage_capacity("AGRIA", "silo", "private", 20000.0)
	# Safra declarada o ano inteiro, começando no dia 1: a colheita entra todo
	# dia no silo (janela que abre no dia 1 entrega a primeira parcela hoje; uma
	# janela que abre depois só entrega quando ela chega). A área é escolhida
	# para a colheita SUPERAR o consumo da cidade (100 mil hab. × 0,0003 t/dia =
	# 30 t/dia): 4.000 ha × 3 t/ha = 12.000 t/ano = 32,9 t/dia.
	sim.set_crop_plan("AGRIA", "wheat", 4000.0, 3.0,
		[{"plant_start": 1, "plant_end": 1, "harvest_start": 1,
		"harvest_end": 365, "share": 1.0}])
	var wheat := _product(sim, "wheat")
	# Sem silo o mercado receberia tudo; com o pulmão, entra aos poucos.
	for _day in 20:
		sim.tick_day()
	var row := _site(sim, "AGRIA", "silo", "private")
	_check(not row.is_empty(), "pulmão: silo privado existe")
	if not row.is_empty():
		var stock := float(row.get("stock_t", 0.0))
		var inflow := float(row.get("inflow_t", 0.0))
		var released := float(row.get("released_today_t", 0.0))
		var received := float(row.get("received_today_t", 0.0))
		_check(stock > 0.0, "pulmão: silo regional recebeu a colheita (%.1f t)"
			% stock)
		_check(inflow > 0.0,
			"pulmão: estoque marcado como pulmão (inflow %.1f t)" % inflow)
		_check(received > 0.0, "pulmão: colheita desviada do mercado hoje (%.1f t)"
			% received)
		_check(released > 0.0, "pulmão: liberou ao mercado da cidade (%.1f t)"
			% released)
		_check(released < received,
			"pulmão: libera aos poucos, não no dia da colheita (%.2f de %.2f t/dia)"
			% [released, received])
	_check(int(row.get("access_paid_today", 0)) >= 0,
		"pulmão: custo de acesso (estrada B5) é cobrado e reportado")

	# Sem silo, a colheita vai direto ao mercado (A/B do mecanismo): mesmo
	# consumo nos dois mundos, então o mercado do mundo COM silo fica mais
	# vazio (a colheita ficou retida e só escoa 1/60 por dia).
	var plain := _make_world(false)
	var sim2 = plain["sim"]
	sim2.set_crop_plan("AGRIA", "wheat", 4000.0, 3.0,
		[{"plant_start": 1, "plant_end": 1, "harvest_start": 1,
		"harvest_end": 365, "share": 1.0}])
	for _day in 20:
		sim2.tick_day()
	var market_plain := _market(sim2, "AGRIA", wheat)
	_check(market_plain > _market(sim, "AGRIA", wheat),
		"pulmão: com silo o mercado fica mais vazio que sem silo (%.1f vs %.1f t)"
		% [_market(sim, "AGRIA", wheat), market_plain])


# ---------------------------------------------------------------------------
# (5) IA privada de armazém
# ---------------------------------------------------------------------------

func _test_ai_storage() -> void:
	var world := _make_world(true)
	var sim = world["sim"]
	sim.set_storage_product_type("wheat", "silo")
	# Safra do jogo: é o `s_annual` que a IA vê para dimensionar o silo (sem
	# produção daquele tipo a IA não abre obra — alvo = 2 meses de produção).
	sim.set_crop_plan("AGRIA", "wheat", 400.0, 3.0,
		[{"plant_start": 1, "plant_end": 60, "harvest_start": 61,
		"harvest_end": 365, "share": 1.0}])
	# Mundo "em paz": PORTUS menor não drena a comida de AGRIA, então a
	# satisfação alimentar da cidade pode ser medida de verdade (antes esta
	# verificação lia 1,0 sempre — o status não tem `satisfaction` na ficha e o
	# helper caía no padrão; corrigido em D-101).
	_set_population(sim, "PORTUS", 50000.0)
	# Preço alto e caixa: a obra de silo paga o payback declarado (6 anos).
	_inject_market(sim, "AGRIA", "wheat", 2000.0)
	_set_price(sim, "AGRIA", "wheat", 90.0)
	_set_cash(sim, "AGRIA", 2_000_000.0, 2_000_000.0)
	var money0 := _money(sim)
	var satisfaction_min := 1.0
	var bankruptcies := 0
	for _day in 120:
		var status: Dictionary = sim.tick_day()
		bankruptcies = int(status.get("bankruptcies", bankruptcies))
		var sat_now := _food_satisfaction(sim, "AGRIA")
		if sat_now < 0.85:
			var cs: Dictionary = sim.get_city_status("AGRIA")
			print("[dbg] d=%d sat=%.3f need=%.1f consumed=%.1f market=%.1f gov=%.1f priv=%.1f"
				% [_day, sat_now, float(cs.get("need", 0.0)),
				float(cs.get("consumed", 0.0)),
				_market(sim, "AGRIA", _product(sim, "wheat")),
				_storage_stock(sim, "AGRIA", "silo", "government", "wheat"),
				_storage_stock(sim, "AGRIA", "silo", "private", "wheat")])
		satisfaction_min = minf(satisfaction_min, sat_now)
	var log_entries: Array = sim.get_command_log()
	var builds := 0
	for entry in log_entries:
		if String((entry as Dictionary).get("type", "")) == "storage_build":
			builds += 1
	_check(builds > 0, "IA: emitiu obra de armazém (%d comando(s))" % builds)
	var built := 0.0
	for row in sim.get_storage_status().get("sites", {}).values():
		var site: Dictionary = row
		if String(site.get("owner", "")) == "private" \
				and float(site.get("capacity_t", 0.0)) > 0.0:
			built += float(site.get("capacity_t", 0.0))
	_check(built > 0.0, "IA: armazém privado construído (%.1f t)" % built)
	var base_world := _make_world(false)
	var base_sim = base_world["sim"]
	base_sim.set_storage_product_type("wheat", "silo")
	_set_population(base_sim, "PORTUS", 50000.0)
	base_sim.set_crop_plan("AGRIA", "wheat", 400.0, 3.0,
		[{"plant_start": 1, "plant_end": 60, "harvest_start": 61,
		"harvest_end": 365, "share": 1.0}])
	_inject_market(base_sim, "AGRIA", "wheat", 2000.0)
	_set_price(base_sim, "AGRIA", "wheat", 90.0)
	_set_cash(base_sim, "AGRIA", 2000000.0, 2000000.0)
	var base_min := 1.0
	for _day in 120:
		base_sim.tick_day()
		base_min = minf(base_min, _food_satisfaction(base_sim, "AGRIA"))
	_check(satisfaction_min >= 0.85,
		"IA: paz — satisfação alimentar nunca caiu abaixo de 0,85 (mín %.3f)"
		% satisfaction_min)
	_check(satisfaction_min + 1e-9 >= base_min,
		"IA: a armazenagem não deixa a cidade com mais fome que o mundo sem IA"
		+ " (mín %.3f vs %.3f)" % [satisfaction_min, base_min])
	_check(bankruptcies == 0, "IA: nenhuma falência em 120 d (%d)" % bankruptcies)
	_check(absf(_money(sim) - money0) <= maxf(1.0, absf(money0) * 1e-6),
		"IA: dinheiro conservado em 120 d (drift %.3f)" % (_money(sim) - money0))


# ---------------------------------------------------------------------------
# (6) IPC e camada nacional
# ---------------------------------------------------------------------------

func _test_ipc() -> void:
	var world := _make_world(false)
	var sim = world["sim"]
	sim.set_storage_product_type("wheat", "silo")
	sim.set_storage_capacity("AGRIA", "silo", "government", 500.0)
	_inject_market(sim, "AGRIA", "wheat", 400.0)
	_set_cash(sim, "AGRIA", 1_000_000.0, 1_000_000.0)
	sim.storage_trade("AGRIA", "silo", "government", "wheat", 300.0, true, false)
	for _day in 3:
		sim.tick_day()
	var status: Dictionary = sim.get_storage_status()
	var nations: Dictionary = status.get("nations", {})
	_check(nations.has("DEMO"), "IPC: camada nacional tem a nação DEMO")
	var row: Dictionary = nations.get("DEMO", {})
	var ipc := float(row.get("ipc", 0.0))
	_check(ipc > 0.5 and ipc < 2.0, "IPC: índice no intervalo plausível (%.4f)"
		% ipc)
	var avg: Dictionary = row.get("avg_price", {})
	_check(avg.has("wheat"), "IPC: preço médio ponderado do trigo publicado")
	_check(float(row.get("government_stock_t", 0.0)) >= 299.0,
		"IPC: reserva do governo publicada (%.1f t)"
		% float(row.get("government_stock_t", 0.0)))
	var cover: Dictionary = row.get("government_days_cover", {})
	_check(cover.has("wheat"), "IPC: dias de consumo cobertos por produto")
	var totals: Dictionary = status.get("totals", {})
	_check(int(totals.get("sites", 0)) >= 1, "IPC: contagem de sites publicada")
	var types: Array = status.get("types", [])
	_check(types.size() == 8, "IPC: 8 tipos de armazém na ficha (%d)"
		% types.size())


# ---------------------------------------------------------------------------
# (7) Save v25 e migração v24→v25
# ---------------------------------------------------------------------------

func _test_roundtrip_and_migration() -> void:
	var world := _make_world(false)
	var sim = world["sim"]
	sim.set_storage_product_type("wheat", "silo")
	sim.set_storage_capacity("AGRIA", "silo", "government", 1000.0)
	sim.set_storage_capacity("PORTUS", "silo", "private", 2500.0)
	_inject_market(sim, "AGRIA", "wheat", 500.0)
	_set_cash(sim, "AGRIA", 1_000_000.0, 1_000_000.0)
	sim.storage_trade("AGRIA", "silo", "government", "wheat", 400.0, true, false)
	for _day in 5:
		sim.tick_day()
	var save: Dictionary = sim.to_dict()
	_check(int(save.get("version", 0)) == SAVE_VERSION,
		"save: versão %d" % int(save.get("version", 0)))
	_check(save.has("storage"), "save: bloco `storage` presente")
	var sites_save: Array = (save.get("storage", {}) as Dictionary).get("sites", [])
	_check(sites_save.size() >= 2, "save: %d sites persistidos" % sites_save.size())

	var loaded := EconomySim.new()
	_attach_chains(loaded)
	loaded.from_dict(save)
	var reloaded: Dictionary = loaded.to_dict()
	_check(int(reloaded.get("version", 0)) == SAVE_VERSION,
		"save: round-trip mantém v25")
	var sites_after: Array = ((reloaded.get("storage", {}) as Dictionary)
		.get("sites", []))
	_check(sites_after.size() == sites_save.size(),
		"save: round-trip mantém os sites (%d → %d)"
		% [sites_save.size(), sites_after.size()])
	var capacity_before := _site_capacity(sim, "AGRIA", "silo", "government")
	var capacity_after := _site_capacity(loaded, "AGRIA", "silo", "government")
	_check(absf(capacity_before - capacity_after) <= 1e-6,
		"save: round-trip mantém a capacidade (%.3f vs %.3f)"
		% [capacity_before, capacity_after])
	var stock_before := _storage_stock(sim, "AGRIA", "silo", "government", "wheat")
	var stock_after := _storage_stock(loaded, "AGRIA", "silo", "government", "wheat")
	_check(absf(stock_before - stock_after) <= 1e-6,
		"save: round-trip mantém o estoque guardado (%.3f vs %.3f)"
		% [stock_before, stock_after])

	# Migração v24→v25: sem o bloco, nada de capacidade/tonelada inventada; o
	# StorageService reaplica o que é DEFINIÇÃO (capacidade do dado).
	var v24: Dictionary = sim.to_dict()
	v24["version"] = 24
	(v24 as Dictionary).erase("storage")
	var migrated := EconomySim.new()
	_attach_chains(migrated)
	migrated.from_dict(v24)
	var migrated_save: Dictionary = migrated.to_dict()
	_check(int(migrated_save.get("version", 0)) == SAVE_VERSION,
		"migração: v24 → v25 (%d)" % int(migrated_save.get("version", 0)))
	var migrated_sites: Array = ((migrated_save.get("storage", {}) as Dictionary)
		.get("sites", []))
	_check(migrated_sites.is_empty(),
		"migração: v24 não inventa armazém (%d)" % migrated_sites.size())

	var storage := StorageScript.new()
	var report: Dictionary = storage.apply_to_sim(migrated, [
		{"id": "AGRIA", "nation": "PRT", "population": 10804871.0},
		{"id": "PORTUS", "nation": "USA", "population": 340000000.0},
	], [
		{"city_id": "AGRIA", "resource": "wheat", "output_tons_per_day": 100.0},
	], {"only_missing": true, "initial_stock": false})
	_check(int(report.get("sites_written", 0)) > 0,
		"migração: service reaplica a capacidade do dado (%d sites)"
		% int(report.get("sites_written", 0)))
	_check(_site_capacity(migrated, "AGRIA", "silo", "government") > 0.0,
		"migração: silo do PRT voltou com capacidade")

	# Depois da migração o dia 1 conserva dinheiro e não viola invariante.
	var money0 := _money(migrated)
	var violations := 0
	for _day in 3:
		var status: Dictionary = migrated.tick_day()
		violations += Array(status.get("invariant_violations", [])).size()
	_check(violations == 0, "migração: 3 dias sem violação (v25)")
	_check(absf(_money(migrated) - money0) <= maxf(1.0, absf(money0) * 1e-6),
		"migração: dinheiro conservado após v24 → v25 (drift %.3f)"
		% (_money(migrated) - money0))


# ---------------------------------------------------------------------------
# (8) Reserva nacional em ilha/micro-mercado (D-101)
# ---------------------------------------------------------------------------

## PORTUS é a "ilha pequena": sem fazenda própria (produção local zero), comida
## curta (satisfação < 0,95) e um silo do GOVERNO com capacidade. A safra do dia
## cai no mercado e o preço recua na colheita — era exatamente aí que a regra
## antiga (10 % do estoque do mercado, sem piso de satisfação nem excedente)
## levava comida da mesa para a reserva. Agora o silo do governo não cresce em
## cidade faminta e a reserva já guardada volta ao mercado.
func _test_government_reserve_keeps_food() -> void:
	var world := _make_world(true)
	var sim = world["sim"]
	sim.set_storage_product_type("wheat", "silo")
	_zero_farms(sim, "PORTUS")
	# População acima do que a produção local + o mercado da ilha cobrem: a
	# satisfação alimentar fica abaixo do piso de compra (como nas ilhas
	# pequenas da D-101).
	_set_population(sim, "PORTUS", 2000000.0)
	sim.set_storage_capacity("PORTUS", "silo", "government", 5000.0)
	sim.set_storage_capacity("PORTUS", "silo", "private", 0.0)
	# Janela de colheita quase o ano inteiro: `harvest_active` é verdadeiro nos
	# dias medidos, como numa ilha que colhe pouco e sempre.
	sim.set_crop_plan("PORTUS", "wheat", 400.0, 1.0,
		[{"plant_start": 1, "plant_end": 1, "harvest_start": 2,
		"harvest_end": 365, "share": 1.0}])
	_inject_market(sim, "PORTUS", "wheat", 200.0)
	_set_cash(sim, "PORTUS", 200000.0, 200000.0)
	var wheat := _product(sim, "wheat")
	var gov_start := _storage_stock(sim, "PORTUS", "silo", "government",
		"wheat")
	var hungry_days := 0
	var bought := 0.0
	var released := 0.0
	for _day in 45:
		sim.tick_day()
		if _food_satisfaction(sim, "PORTUS") < 0.95:
			hungry_days += 1
		var site := _site(sim, "PORTUS", "silo", "government")
		bought += float(site.get("bought_today_t", 0.0))
		released += float(site.get("released_today_t", 0.0))
	_check(hungry_days >= 30,
		"ilha: cenário é de comida curta (%d/45 dias com satisfação < 0,95)"
		% hungry_days)
	var gov_after_buy := _storage_stock(sim, "PORTUS", "silo", "government",
		"wheat")
	_check(gov_after_buy <= gov_start + 1e-6,
		"ilha: o silo do governo NÃO cresceu na cidade faminta (%.3f → %.3f t)"
		% [gov_start, gov_after_buy])
	_check(bought <= 1e-6,
		"ilha: nenhuma compra do governo para a reserva (%.3f t compradas)"
		% bought)

	# Libera onde falta: a reserva já guardada volta ao mercado (sem pagamento)
	# enquanto a satisfação estiver abaixo do piso de compra.
	sim.set_storage_stock("PORTUS", "silo", "government", "wheat", 100.0)
	var seeded := _storage_stock(sim, "PORTUS", "silo", "government", "wheat")
	var market_before := _market(sim, "PORTUS", wheat)
	var released_now := 0.0
	for _day in 10:
		sim.tick_day()
		released_now += float(_site(sim, "PORTUS", "silo", "government").get(
			"released_today_t", 0.0))
	var gov_final := _storage_stock(sim, "PORTUS", "silo", "government",
		"wheat")
	_check(seeded >= 99.0 and gov_final < seeded * 0.99,
		"ilha: reserva libera onde falta (%.1f → %.1f t em 10 d)"
		% [seeded, gov_final])
	_check(released_now > 0.0,
		"ilha: liberação medida no site do governo (%.1f t)" % released_now)
	_check(_market(sim, "PORTUS", wheat) >= market_before,
		"ilha: o mercado não perdeu comida para o silo (%.1f t)"
		% _market(sim, "PORTUS", wheat))


# ---------------------------------------------------------------------------
# Apoio
# ---------------------------------------------------------------------------

## Mundo demo (AGRIA/PORTUS) com geografia declarada. `ai` liga a IA do núcleo;
## a construção é INSTANTÂNEA porque o teste mede a decisão, não o cronograma.
func _make_world(ai: bool) -> Dictionary:
	var sim = EconomySim.new()
	sim.set_instant_construction(true)
	sim.initialize_scenario()
	var geo = GeoScript.new()
	geo.apply_to_sim(sim, [
		{"id": "AGRIA", "center": AGRIA_CENTER},
		{"id": "PORTUS", "center": PORTUS_CENTER},
	])
	if ai:
		sim.set_ai_enabled(true)
		GroupNamerScript.name_groups(sim)
	return {"sim": sim, "geo": geo}


## Cadeias de dados que o núcleo exige antes do `from_dict` de um save.
func _attach_chains(sim: Object) -> void:
	sim.set_goods(_load_array("res://data/definitions/economy/goods.json"))
	sim.set_recipes(_load_array("res://data/definitions/economy/recipes.json"))
	sim.set_needs(_load_dict("res://data/definitions/economy/needs.json"))
	sim.set_services(_load_dict("res://data/definitions/economy/services.json"))
	sim.set_currencies(_load_dict("res://data/definitions/economy/currencies.json"))


func _load_array(path: String) -> Array:
	var data: Variant = _load(path)
	return data if typeof(data) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load(path)
	return data if typeof(data) == TYPE_DICTIONARY else {}


func _load(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


## Ficha do site (cidade, tipo, dono). A chave do núcleo usa o ÍNDICE do tipo;
## procurar pelo nome é o contrato estável deste teste.
func _site(sim: Object, city: String, type: String, owner: String) -> Dictionary:
	for row in sim.get_storage_status().get("sites", {}).values():
		var site: Dictionary = row
		if String(site.get("city", "")) == city \
				and String(site.get("type", "")) == type \
				and String(site.get("owner", "")) == owner:
			return site
	return {}


func _site_capacity(sim: Object, city: String, type: String,
		owner: String) -> float:
	return float(_site(sim, city, type, owner).get("capacity_t", 0.0))


func _storage_stock(sim: Object, city: String, type: String, owner: String,
		product: String) -> float:
	var row := _site(sim, city, type, owner)
	var by_product: Dictionary = row.get("by_product", {})
	return float(by_product.get(product, 0.0))


func _market(sim: Object, city: String, product: int) -> float:
	# O status diário NÃO traz o mercado de cada cidade (é detalhe pesado); a
	# leitura por cidade é `get_city_status(id).markets`.
	var markets: Array = (sim.get_city_status(city) as Dictionary).get(
		"markets", [])
	if product >= 0 and product < markets.size():
		return float((markets[product] as Dictionary).get("stock", 0.0))
	return 0.0


func _total_wheat(sim: Object, city: String, product: int) -> float:
	var total := _market(sim, city, product)
	for key in sim.get_storage_status().get("sites", {}).keys():
		var row: Dictionary = sim.get_storage_status()["sites"][key]
		if String(row.get("city", "")) != city:
			continue
		var by_product: Dictionary = row.get("by_product", {})
		total += float(by_product.get("wheat", 0.0))
	return total


## Índice de um produto pelo id (o mundo demo vem do próprio cenário).
func _product(sim: Object, product: String) -> int:
	var products: Array = sim.get_products()
	for index in products.size():
		if String(products[index]) == product:
			return index
	return -1


func _money(sim: Object) -> float:
	return float(sim.get_status().get("money_total", 0.0))


## Satisfação ALIMENTAR (a mesma que a IA lê: `pop.satisfaction`, que é
## consumido/necessidade do dia). A ficha da cidade traz o bloco `pop`; quando
## ele não vem, a conta é refeita com `consumed/need`.
func _food_satisfaction(sim: Object, city: String) -> float:
	var info: Dictionary = sim.get_city_status(city)
	if info.is_empty():
		return 1.0
	var pop: Dictionary = info.get("pop", {})
	if pop.has("satisfaction"):
		return float(pop.get("satisfaction", 1.0))
	var need := float(info.get("need", 0.0))
	if need <= 0.0:
		return 1.0
	return clampf(float(info.get("consumed", 0.0)) / need, 0.0, 1.0)


## Define a população da cidade no retrato do dia (população, população
## simulada e POP — os três campos que o save carrega).
func _set_population(sim: Object, city_id: String, people: float) -> void:
	var snapshot: Dictionary = sim.to_dict()
	for city in Array(snapshot.get("cities", [])):
		if String((city as Dictionary).get("id", "")) == city_id:
			city["population"] = people
			city["simulation_population"] = people
			var pop: Dictionary = city.get("pop", {})
			pop["population"] = people
	sim.from_dict(snapshot)


## Zera a produção das construções de uma cidade no retrato do dia (a "ilha"
## sem fazenda própria). `capacity` é o campo do save, e o núcleo relê no
## `from_dict` — nenhuma tonelada é criada ou destruída.
func _zero_farms(sim: Object, city_id: String) -> void:
	var snapshot: Dictionary = sim.to_dict()
	for building in Array(snapshot.get("buildings", [])):
		if String((building as Dictionary).get("city", "")) == city_id:
			building["capacity"] = 0.0
	sim.from_dict(snapshot)


func _inject_market(sim: Object, city_id: String, product: String,
		amount: float) -> void:
	var index := _product(sim, product)
	var snapshot: Dictionary = sim.to_dict()
	for city in snapshot["cities"]:
		if String(city["id"]) == city_id and index >= 0:
			(city["markets"][index] as Dictionary)["stock"] = amount
	sim.from_dict(snapshot)


func _set_price(sim: Object, city_id: String, product: String,
		price: float) -> void:
	var index := _product(sim, product)
	var snapshot: Dictionary = sim.to_dict()
	for city in snapshot["cities"]:
		if String(city["id"]) == city_id and index >= 0:
			(city["markets"][index] as Dictionary)["price"] = price
	sim.from_dict(snapshot)


func _set_cash(sim: Object, city_id: String, firm: float,
		government: float) -> void:
	var snapshot: Dictionary = sim.to_dict()
	for city in snapshot["cities"]:
		if String(city["id"]) == city_id:
			city["firm_cash"] = firm
			city["government_budget"] = government
	sim.from_dict(snapshot)


func _check(condition: bool, message: String) -> void:
	_checks += 1
	if not condition:
		_failures.append(message)
