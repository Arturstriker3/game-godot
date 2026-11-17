extends SceneTree

## Teste do pacote W1 da §19 — solo urbano e zoneamento.
##
## Guarda de regressão do que a spec exige do W1:
##   1. ocupação da zona pressiona o preço da terra, que sobe mês a mês
##      (convexo perto de 1) e é maior onde a zona está cheia;
##   2. sem espaço COMERCIAL, abrir negócio de serviço fica PENDENTE (não
##      falha) e sinaliza demanda de zona para a prefeitura;
##   3. sem espaço INDUSTRIAL, nova fábrica urbana fica pendente e sinaliza;
##   4. expansão de zona como obra §15 (tipo infra_urbana) concluída aumenta a
##      capacidade da zona e consome a reserva;
##   5. adensamento sobe a densidade permitida da zona (custo menor);
##   6. save/load sem diferença e mundo idêntico com threads=1 e 8.
##
## Roda no cenário demo (AGRIA/PORTUS) com solo urbano CRAFTADO: as áreas são
## minúsculas de propósito para o limite de zona morder. Nenhum número de
## cidade real é inventado no jogo; aqui é fixture de teste.
##
##   godot --headless --path . --script res://tools/urban_land_test.gd

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"

const ZONE_RES := 0
const ZONE_COM := 1
const ZONE_IND := 2

var _failures: Array[String] = []


func _init() -> void:
	_test_land_price()
	_test_service_zone_gate()
	_test_factory_zone_gate()
	_test_expand_and_densify()
	_test_determinism_and_roundtrip()
	if _failures.is_empty():
		print("[urban-w1] OK: preco x ocupacao, zona como limite pendente, "
			+ "expansao/adensamento §15, save/load e threads 1/8.")
		quit(0)
	else:
		for failure in _failures:
			print("[urban-w1] FALHA: " + failure)
		quit(1)


func _fail(msg: String) -> void:
	_failures.append(msg)


func _load(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("nao abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _land_data() -> Dictionary:
	# AGRIA: metrópole (faixa >5 M → ocupação alvo 0,85) com COM minúscula
	# (0,8 m²: nem um trabalhador de serviço cabe). A zona IND cabe 132
	# trabalhadores — menos do que a fábrica de teste (1.000 t/dia) exige.
	# PORTUS: mesma moradia numa cidade pequena (faixa <300 mil → alvo 0,55)
	# com área urbana grande (zona folgada).
	return {"cities": {
		"AGRIA": {
			"built_up_km2": 0.04,
			"zoning_split": {"residential": 0.5, "commercial": 0.00002,
				"industrial": 0.498},
			"height_class": "low",
			"urban_population": 6000000,
			"expandable_reserve_km2": 0.01,
		},
		"PORTUS": {
			"built_up_km2": 400.0,
			"zoning_split": {"residential": 0.5, "commercial": 0.25,
				"industrial": 0.25},
			"height_class": "low",
			"urban_population": 200000,
			"expandable_reserve_km2": 10.0,
		},
	}}


func _housing_data() -> Dictionary:
	# Mesmo estoque e mesmo preço/aluguel nas duas cidades: o que difere é só a
	# OCUPAÇÃO da zona (área por unidade).
	var entry := {
		"housing_stock_by_tier": {"popular": 180.0, "medium": 0.0, "high": 0.0},
		"rent_per_m2_usd": 10.0,
		"price_per_m2_usd": 1000.0,
		"homeownership_rate": 0.7,
	}
	return {"cities": {"AGRIA": entry, "PORTUS": entry.duplicate(true)}}


func _new_sim(threads: int = 1) -> EconomySim:
	var sim := EconomySim.new()
	sim.set_goods(_load(GOODS_PATH))
	sim.set_recipes(_load(RECIPES_PATH))
	sim.set_needs(_load(NEEDS_PATH))
	if sim.has_method("set_services"):
		sim.set_services(_load(SERVICES_PATH))
	sim.set_instant_construction(false)
	if sim.has_method("set_sim_threads"):
		sim.set_sim_threads(threads)
	sim.initialize_scenario()
	sim.set_urban(_land_data(), _housing_data(), {"nations": {}},
		{"nations": {}})
	return sim


func _urban(sim: EconomySim, city_id: String) -> Dictionary:
	return sim.get_urban_city(city_id)


func _set_government_budget(sim: EconomySim, city_id: String, budget: float) -> void:
	var snapshot: Dictionary = sim.to_dict()
	for city in snapshot["cities"]:
		if String(city["id"]) == city_id:
			city["government_budget"] = budget
	sim.from_dict(snapshot)


func _seed_materials(sim: EconomySim, city_id: String, tons: float) -> void:
	for good in ["construction_material", "steel_structure", "electrical_material"]:
		sim.set_market_stock(city_id, good, tons)


func _projects(sim: EconomySim) -> Array:
	return sim.to_dict().get("construction_projects", [])


## 1. Ocupação -> preço da terra (mensal, convexo, maior na zona cheia).
func _test_land_price() -> void:
	var sim := _new_sim()
	var agria: Dictionary = _urban(sim, "AGRIA")
	var occ_res := float(agria["zone_occupancy"][ZONE_RES])
	# Q-015-C: a capacidade RES é calibrada no carregamento para a ocupação
	# nascer no alvo da faixa de população urbana (AGRIA declara 6 M → 0,85).
	if absf(occ_res - 0.85) > 1e-6:
		_fail("ocupacao RES AGRIA %.6f != 0.85 (alvo da faixa >5 mi)" % occ_res)
	var com_capacity := float(agria["zone_capacity"][ZONE_COM])
	if com_capacity >= 1.0:
		_fail("COM AGRIA deveria caber menos de 1 trabalhador (%.6f)" % com_capacity)
	# IND medida em trabalhadores: 0,01992 km² × FAR 1 ÷ 150 m²/trabalhador.
	var ind_capacity := float(agria["zone_capacity"][ZONE_IND])
	if absf(ind_capacity - 132.8) > 0.1:
		_fail("IND AGRIA capacidade em trabalhadores %.3f != 132.8 (150 m2/trab)" % ind_capacity)
	if float(agria["land_price"][ZONE_RES]) != 0.0:
		_fail("preco da terra deveria comecar em 0 (ausencia explicita)")

	for day in 30:
		sim.tick_day()
	var month1: Dictionary = _urban(sim, "AGRIA")
	var portus: Dictionary = _urban(sim, "PORTUS")
	var price_m1 := float(month1["land_price"][ZONE_RES])
	if price_m1 <= 0.0:
		_fail("preco da terra nao se moveu no 1o mes")
	for day in 30:
		sim.tick_day()
	var month2: Dictionary = _urban(sim, "AGRIA")
	var price_m2 := float(month2["land_price"][ZONE_RES])
	if price_m2 <= price_m1:
		_fail("preco da terra nao subiu do mes 1 (%.3f) para o mes 2 (%.3f)" % [
			price_m1, price_m2])
	var portus_price := float(portus["land_price"][ZONE_RES])
	if portus_price <= 0.0:
		_fail("PORTUS (zona folgada) ficou sem preco de terra")
	if price_m2 <= portus_price * 2.0:
		_fail("zona cheia (%.1f) deveria custar bem mais que a folgada (%.1f)" % [
			price_m2, portus_price])
	# Preço positivo nas três zonas (a base vem do imóvel medio da fonte).
	for zone in 3:
		if float(month2["land_price"][zone]) <= 0.0:
			_fail("zona %d sem preco de terra no mes 2" % zone)
	print("[urban-w1] preco RES AGRIA %.1f -> %.1f XGS/m2; PORTUS %.1f (ocupacao %.4f)" % [
		price_m1, price_m2, portus_price,
		float(portus["zone_occupancy"][ZONE_RES])])


## 2. Sem espaço COMERCIAL: a abertura fica pendente e sinaliza demanda.
func _test_service_zone_gate() -> void:
	var sim := _new_sim()
	var sectors: Array = sim.get_service_sectors()
	var detail: Dictionary = sim.get_services_detail("AGRIA")
	var used := {}
	for business in detail.get("businesses", []):
		used[String(business.get("sector", ""))] = true
	var sector_id := ""
	for sector in sectors:
		var candidate := String(sector.get("id", ""))
		if candidate != "" and not used.has(candidate):
			sector_id = candidate
			break
	if sector_id == "":
		_fail("nenhum setor de servico livre em AGRIA para o teste de zona")
		return
	var before_count := sim.get_service_business_count()
	var before_demand := float(_urban(sim, "AGRIA")["zone_demand"][ZONE_COM])
	# Capacidade minúscula: capital irrelevante; o que barra é a zona COM.
	sim.set_service_capacity("AGRIA", sector_id, 0.001)
	var after_count := sim.get_service_business_count()
	var after_demand := float(_urban(sim, "AGRIA")["zone_demand"][ZONE_COM])
	if after_count != before_count:
		_fail("zona COM cheia: negocio abriu mesmo assim (%d -> %d)" % [
			before_count, after_count])
	if after_demand != before_demand + 1.0:
		_fail("zona COM cheia: demanda sinalizada %.1f -> %.1f (esperado +1)" % [
			before_demand, after_demand])
	print("[urban-w1] setor %s barrado pela zona COM: pendente + sinal (%.1f)" % [
		sector_id, after_demand])


## 3. Sem espaço INDUSTRIAL: nova fábrica urbana fica pendente e sinaliza.
func _test_factory_zone_gate() -> void:
	var sim := _new_sim()
	var before_demand := float(_urban(sim, "AGRIA")["zone_demand"][ZONE_IND])
	# 1.000 empregos declarados contra 132 trabalhadores de capacidade na zona
	# IND de AGRIA: a planta não entra (a zona é medida em trabalhadores).
	var result: Dictionary = sim.build("AGRIA", "T-FACTORY-W1", "bread",
		21, 23, 1.0, 1000.0, 0.0, 0.0, 1.0, "private", 100.0)
	if bool(result.get("created", false)):
		_fail("zona IND cheia: fabrica criada mesmo assim")
	if not String(result.get("error", "")).contains("zona industrial"):
		_fail("zona IND cheia: erro inesperado (%s)" % String(
			result.get("error", "")))
	if not bool(result.get("zone_pending", false)):
		_fail("zona IND cheia: decisao nao foi marcada como pendente")
	var after_demand := float(_urban(sim, "AGRIA")["zone_demand"][ZONE_IND])
	if after_demand != before_demand + 1.0:
		_fail("zona IND cheia: demanda sinalizada %.1f -> %.1f (esperado +1)" % [
			before_demand, after_demand])
	if not _projects(sim).is_empty():
		_fail("zona IND cheia: obra empilhada apesar do limite")
	print("[urban-w1] fabrica barrada pela zona IND: pendente + sinal (%.1f)" % [
		after_demand])


## 4/5. Expansão e adensamento como obra §15 (tipo infra_urbana).
func _test_expand_and_densify() -> void:
	var sim := _new_sim()
	_set_government_budget(sim, "AGRIA", 10000000.0)
	_seed_materials(sim, "AGRIA", 1000.0)
	var before: Dictionary = _urban(sim, "AGRIA")
	var area_before := float(before["area_km2"][ZONE_IND])
	var capacity_before := float(before["zone_capacity"][ZONE_IND])
	var reserve_before := float(before["reserve_km2"])
	var cost_km2_before := float(before["expansion_cost_per_km2"])
	var expanded: Dictionary = sim.urban_expand("AGRIA", ZONE_IND, 0.002)
	if not bool(expanded.get("ok", false)):
		_fail("expansao: nao abriu (%s)" % String(expanded.get("error", "")))
		return
	if float(expanded.get("cost", 0.0)) <= 0.0:
		_fail("expansao: custo zerado")
	var project: Dictionary = _projects(sim)[0] if not _projects(sim).is_empty() else {}
	if int(project.get("urban_action", -1)) != 1 \
			or int(project.get("urban_zone", -1)) != ZONE_IND:
		_fail("expansao: projeto sem marcador de infra urbana (%s)" % str(project))
	for day in 60:
		sim.tick_day()
		if _projects(sim).is_empty():
			break
	if not _projects(sim).is_empty():
		_fail("expansao: obra nao concluiu em 60 dias (dias=%s, motivo=%s)" % [
			str(_projects(sim)[0].get("days_elapsed")),
			String(_projects(sim)[0].get("pause_reason", ""))])
		return
	var violations: Array = sim.get_status().get("invariant_violations", [])
	if not violations.is_empty():
		_fail("expansao: invariante violado (%s)" % String(violations[0]))
	var after: Dictionary = _urban(sim, "AGRIA")
	var area_after := float(after["area_km2"][ZONE_IND])
	var capacity_after := float(after["zone_capacity"][ZONE_IND])
	if absf(area_after - (area_before + 0.002)) > 1e-6:
		_fail("expansao: area %.6f -> %.6f (esperado +0.002)" % [
			area_before, area_after])
	if capacity_after <= capacity_before:
		_fail("expansao: capacidade IND nao aumentou (%.4f -> %.4f)" % [
			capacity_before, capacity_after])
	if float(after["reserve_km2"]) >= reserve_before:
		_fail("expansao: reserva nao foi consumida (%.4f -> %.4f)" % [
			reserve_before, float(after["reserve_km2"])])
	# Custo crescente (spec §19): a cidade maior e com menos reserva paga
	# MAIS pelo próximo km² que pagava antes da obra.
	var cost_km2_after := float(after.get("expansion_cost_per_km2", 0.0))
	if cost_km2_after <= cost_km2_before:
		_fail("custo crescente: km2 %.1f -> %.1f (esperado subir com raio/reserva)" % [
			cost_km2_before, cost_km2_after])

	# Adensamento: sobe LOW -> MID (custo menor que a expansão por km²) e a
	# capacidade residencial cresce; no máximo o teto HIGH.
	var capacity_before_densify := float(after["zone_capacity"][ZONE_RES])
	var densified: Dictionary = sim.urban_densify("AGRIA", ZONE_RES)
	if not bool(densified.get("ok", false)):
		_fail("adensamento: nao abriu (%s)" % String(densified.get("error", "")))
		return
	var cost_low_mid := float(densified.get("cost", 0.0))
	for day in 60:
		sim.tick_day()
		if _projects(sim).is_empty():
			break
	if not _projects(sim).is_empty():
		_fail("adensamento: obra nao concluiu em 60 dias")
		return
	var dense: Dictionary = _urban(sim, "AGRIA")
	if int(dense["max_density"][ZONE_RES]) != 1:
		_fail("adensamento: densidade RES %d != 1 (MID)" % int(
			dense["max_density"][ZONE_RES]))
	if float(dense["zone_capacity"][ZONE_RES]) <= capacity_before_densify:
		_fail("adensamento: capacidade RES nao cresceu")
	var again: Dictionary = sim.urban_densify("AGRIA", ZONE_RES)
	if not bool(again.get("ok", false)):
		_fail("adensamento 2 (MID->HIGH): nao abriu (%s)" % String(
			again.get("error", "")))
		return
	# Custo crescente por NÍVEL: MID→HIGH (aço/elevadores) custa mais que
	# LOW→MID (reforço de redes) na mesma área.
	var cost_mid_high := float(again.get("cost", 0.0))
	if cost_low_mid <= 0.0 or cost_mid_high <= cost_low_mid:
		_fail("adensamento por nivel: LOW->MID %.1f deveria ser menor que MID->HIGH %.1f" % [
			cost_low_mid, cost_mid_high])
	for day in 60:
		sim.tick_day()
		if _projects(sim).is_empty():
			break
	var third: Dictionary = sim.urban_densify("AGRIA", ZONE_RES)
	if bool(third.get("ok", false)) or not String(third.get("error", "")).contains(
			"densidade"):
		_fail("adensamento no teto HIGH deveria ser recusado (%s)" % String(
			third.get("error", "")))
	print("[urban-w1] expansao IND %.4f -> %.4f (cap %.3f -> %.3f); RES densidade %d" % [
		area_before, area_after, capacity_before, capacity_after,
		int(dense["max_density"][ZONE_RES])])


## 6. Determinismo threads=1/8 e round-trip do save (0 diferenças).
func _test_determinism_and_roundtrip() -> void:
	var one := _new_sim(1)
	var eight := _new_sim(8)
	for day in 60:
		one.tick_day()
		eight.tick_day()
	var save_one: String = JSON.stringify(one.to_dict(), "", true)
	var save_eight: String = JSON.stringify(eight.to_dict(), "", true)
	if save_one != save_eight:
		_fail("threads 1 e 8 divergiram no save depois de 60 dias")
	if save_one != JSON.stringify(one.to_dict(), "", true):
		_fail("to_dict nao e estavel entre chamadas")

	var before: Dictionary = _urban(one, "AGRIA")
	var snapshot: Dictionary = one.to_dict()
	var restored := _new_sim(1)
	restored.from_dict(snapshot)
	var after: Dictionary = _urban(restored, "AGRIA")
	if JSON.stringify(before, "", true) != JSON.stringify(after, "", true):
		_fail("save/load: estado urbano divergiu (AGRIA)")
	var before_portus: Dictionary = _urban(one, "PORTUS")
	var after_portus: Dictionary = _urban(restored, "PORTUS")
	if JSON.stringify(before_portus, "", true) != JSON.stringify(after_portus, "", true):
		_fail("save/load: estado urbano divergiu (PORTUS)")
	if int(one.to_dict().get("version", 0)) != 14:
		_fail("save version != 14")
	print("[urban-w1] threads 1/8 identicos; save/load urbano identico (v14)")
