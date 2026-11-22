extends SceneTree

## §19 W3 — mercado de moradia. Guarda de aceite do pacote:
##   (1) falta de unidade  -> aluguel SOBE (passo mensal limitado);
##   (2) sobra de unidade  -> aluguel CAI;
##   (3) aluguel pago vira renda do DONO por owner_share (família -> renda de
##       capital da POP, empresa -> caixa das firmas, prefeitura -> caixa);
##   (4) dinheiro conservado (invariantes do tick nunca disparam);
##   (5) favela SÓ no último caso: falta popular com vaga em outro padrão NÃO
##       abre área informal; sem vaga nenhuma e aperto prolongado, abre;
##   (6) save/load: bloco `housing` ida-e-volta com 0 diffs;
##   (7) determinismo: threads=1 e 8 produzem o mesmo estado.
##
##   godot --headless --path . --script res://tools/housing_market_test.gd

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

## Cidade grande com os três padrões e posse mista (família + empresa): o
## aluguel tem donos para os dois lados da transferência.
const CITY := "BRA-SAO"
const TIERS := 3
const DAYS_RENT := 35     # um fechamento mensal + folga (passo de ±2%/mês)
const DAYS_SLUM := 40     # 30 dias de aperto + fechamento mensal
const TOL := 1.0e-6

var _failures: Array[String] = []


func _init() -> void:
	_test_rent_and_owners()
	_test_slum_last_resort()
	_test_save_roundtrip()
	_test_thread_determinism()
	if _failures.is_empty():
		print("[housing] OK: falta sobe, sobra cai, aluguel vira renda do dono, "
			+ "dinheiro conservado, favela só no último caso, round-trip e "
			+ "determinismo threads=1/8.")
		quit(0)
	else:
		for failure in _failures:
			print("[housing] FALHA: " + failure)
		quit(1)


func _fail(msg: String) -> void:
	_failures.append(msg)


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("nao abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_DICTIONARY else {}


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_ARRAY else []


## Mundo real (834 cidades) + estado urbano, pelo mesmo caminho do jogo. `limit`
## corta a lista de cidades (teste rápido de determinismo).
func _make_world(limit: int = 0) -> EconomySim:
	var sim := EconomySim.new()
	sim.set_goods(_load_array(GOODS_PATH))
	sim.set_recipes(_load_array(RECIPES_PATH))
	sim.set_needs(_load_dict(NEEDS_PATH))
	sim.set_services(_load_dict(SERVICES_PATH))
	sim.set_currencies(_load_dict(CURRENCIES_PATH))
	sim.set_sector_employment(_load_dict(SECTOR_EMPLOYMENT_PATH))
	var cities := _load_array(CITIES_PATH)
	if limit > 0 and cities.size() > limit:
		cities = cities.slice(0, limit)
	sim.initialize_world(cities, _load_array(PORTS_PATH),
		_load_array(BUILDINGS_PATH))
	sim.set_urban(
		_load_dict(URBAN_LAND_PATH),
		_load_dict(HOUSING_CITY_PATH),
		_load_dict(HOUSING_PATH),
		_load_dict(MUNICIPAL_FINANCE_PATH))
	return sim


## Tica e falha se qualquer dia violar um invariante do núcleo (dinheiro não
## conservado, caixa negativo, preço fora da banda). Devolve false na falha.
func _tick(sim: EconomySim, days: int, label: String) -> bool:
	for day in days:
		sim.tick_day()
		var violations: Array = sim.get_status().get("invariant_violations", [])
		if violations.size() > 0:
			_fail("%s: invariante violado no dia %d: %s" % [
				label, day + 1, String(violations[0])])
			return false
	return true


func _housing(sim: EconomySim, city_id: String) -> Dictionary:
	return sim.get_urban_city(city_id)


## Renda de capital da POP no dia corrente (consumida de `capital_income_next`
## no fechamento de ontem) — o status do dia não publica esse campo.
func _city_capital_income(sim: EconomySim, city_id: String) -> float:
	var snapshot: Dictionary = sim.to_dict()
	for city in snapshot.get("cities", []):
		if String(city.get("id", "")) == city_id:
			return float(city["pop"].get("capital_income", 0.0))
	return 0.0


func _rent_values(sim: EconomySim, city_id: String) -> Array[float]:
	var rent: Array = _housing(sim, city_id).get("rent", [])
	var out: Array[float] = []
	for tier in TIERS:
		out.append(float(rent[tier]))
	return out


## Multiplica as unidades do save: `factor` em todos os padrões ou só num
## `tier` (a demanda/domicílios NÃO muda — é o choque de oferta do teste).
func _scale_units(sim: EconomySim, city_id: String, factor: float,
		tier_only: int = -1) -> void:
	var snapshot: Dictionary = sim.to_dict()
	for entry in snapshot.get("urban", []):
		if String(entry.get("city", "")) != city_id:
			continue
		var units: Array = entry["units"]
		for density in 3:
			var row: Array = units[density]
			for tier in TIERS:
				if tier_only >= 0 and tier != tier_only:
					continue
				row[tier] = float(row[tier]) * factor
	sim.from_dict(snapshot)


func _test_rent_and_owners() -> void:
	var sim := _make_world()
	if not _tick(sim, 5, "aquecimento"):
		return
	var base := _housing(sim, CITY)
	if base.is_empty():
		_fail("%s sem estado urbano" % CITY)
		return
	var rent_base := _rent_values(sim, CITY)
	var money_before := float(sim.get_status().get("money_total", 0.0))

	# --- (1) FALTA: oferta cai a 40% dos domicílios, sem vaga em padrão algum.
	_scale_units(sim, CITY, 0.4)
	if not _tick(sim, DAYS_RENT, "falta"):
		return
	var short := _housing(sim, CITY)
	var rent_short := _rent_values(sim, CITY)
	# A filtragem joga o excedente dos padrões altos para baixo: o aperto aparece
	# no POPULAR (o padrão de baixo absorve a falta). Os padrões de cima não
	# podem CAIR — só o de baixo precisa subir.
	if rent_short[0] <= rent_base[0] + TOL:
		_fail("falta: aluguel popular não subiu (%.4f -> %.4f)" % [
			rent_base[0], rent_short[0]])
	for tier in range(1, TIERS):
		if rent_short[tier] < rent_base[tier] - TOL:
			_fail("falta: aluguel do padrão %d caiu (%.4f -> %.4f)" % [
				tier, rent_base[tier], rent_short[tier]])

	# --- (3) aluguel vira renda do DONO por owner_share.
	var paid_month := float(short.get("rent_paid_month", 0.0))
	if paid_month <= 0.0:
		_fail("falta: nenhum aluguel foi pago no mês")
	var to_house := float(short.get("rent_to_households", 0.0))
	var to_firms := float(short.get("rent_to_firms", 0.0))
	var to_gov := float(short.get("rent_to_government", 0.0))
	var owner: Array = short.get("owner_share", [])
	# Manutenção de 10% fica com o setor de serviços; o resto vai aos donos.
	if absf((to_house + to_firms + to_gov) - paid_month * 0.9) > paid_month * 1e-3:
		_fail("donos: repasse %.2f != 90%% do aluguel pago %.2f" % [
			to_house + to_firms + to_gov, paid_month])
	var house_share := float(owner[0]) / maxf(1e-9,
		float(owner[0]) + float(owner[1]) + float(owner[2]))
	if absf(to_house - paid_month * 0.9 * house_share) > paid_month * 1e-3:
		_fail("donos: renda de capital da família %.2f != %.2f" % [
			to_house, paid_month * 0.9 * house_share])
	if float(owner[1]) > 0.0 and to_firms <= 0.0:
		_fail("donos: aluguel da empresa dona não entrou no caixa das firmas")
	# O crédito chega à renda de capital da POP no dia seguinte (o status do
	# dia lê `pop.capital_income`, consumido da fila `capital_income_next`).
	var rent_day := float(short.get("rent_paid", 0.0))
	if not _tick(sim, 1, "capital"):
		return
	var capital := _city_capital_income(sim, CITY)
	if rent_day > 0.0 and capital + 1e-9 < rent_day * 0.9 * house_share:
		_fail("donos: renda de capital do dia (%.2f) abaixo do aluguel das "
			% capital + "famílias (%.2f)" % [rent_day * 0.9 * house_share])

	# --- (2) SOBRA: oferta triplicada no mesmo mundo -> aluguel cai.
	_scale_units(sim, CITY, 3.0 / 0.4)
	if not _tick(sim, DAYS_RENT, "sobra"):
		return
	var glut := _housing(sim, CITY)
	var rent_glut := _rent_values(sim, CITY)
	for tier in TIERS:
		if rent_glut[tier] >= rent_short[tier] - TOL:
			_fail("sobra: aluguel do padrão %d não caiu (%.3f -> %.3f)" % [
				tier, rent_short[tier], rent_glut[tier]])
	if float(glut.get("vacancy", [0.0])[0]) <= 0.0:
		_fail("sobra: vacância do padrão popular não ficou positiva")

	# --- (4) dinheiro conservado: o status já checa por dia; confirma o total.
	var money_after := float(sim.get_status().get("money_total", 0.0))
	if absf(money_after - money_before) > 0.01:
		_fail("conservação: dinheiro total %.2f -> %.2f" % [
			money_before, money_after])


func _test_slum_last_resort() -> void:
	var sim := _make_world()
	if not _tick(sim, 5, "aquecimento"):
		return
	var base := _housing(sim, CITY)
	var informal_base := float(base.get("informal_units", 0.0))

	# --- (5a) falta POPULAR com vaga nos outros padrões: NÃO é o último caso.
	# Popular desabastecido + padrão médio com folga (2× as unidades): há vaga
	# em outro padrão, então a favela não é o último caso.
	_scale_units(sim, CITY, 0.02, 0)
	_scale_units(sim, CITY, 2.0, 1)
	if not _tick(sim, DAYS_SLUM, "falta-popular-com-vaga"):
		return
	var popular := _housing(sim, CITY)
	if float(popular.get("vacancy", [0.0, 0.0, 0.0])[1]) <= 0.0:
		_fail("favela: cenário sem vaga no padrão médio (teste inválido)")
	if float(popular.get("informal_units", 0.0)) > informal_base + 1.0:
		_fail("favela: abriu área informal com vaga disponível (%.0f -> %.0f)" % [
			informal_base, float(popular.get("informal_units", 0.0))])
	if float(popular.get("housing_stress", 0.0)) <= 0.5:
		_fail("favela: aperto popular não foi publicado (stress=%.2f)" % float(
			popular.get("housing_stress", 0.0)))

	# --- (5b) nenhuma vaga e aperto prolongado: aí sim, área informal.
	_scale_units(sim, CITY, 0.02)
	# 70 dias: o aperto precisa durar 30 dias ANTES de um fechamento mensal
	# (o stress de (5a) foi zerado, e os fechamentos caem a cada 30 dias).
	if not _tick(sim, 70, "ultimo-caso"):
		return
	var slum := _housing(sim, CITY)
	if float(slum.get("informal_units", 0.0)) <= informal_base + 1.0:
		_fail("favela: último caso não abriu área informal (%.0f)" % float(
			slum.get("informal_units", 0.0)))
	if float(slum.get("informal_km2", 0.0)) <= float(base.get("informal_km2", 0.0)):
		_fail("favela: área informal (km²) não cresceu")


func _test_save_roundtrip() -> void:
	var sim := _make_world(40)
	if not _tick(sim, 10, "roundtrip"):
		return
	var before: Dictionary = sim.to_dict()
	sim.from_dict(before)
	var after: Dictionary = sim.to_dict()
	if JSON.stringify(before.get("housing", [])) != JSON.stringify(
			after.get("housing", [])):
		_fail("round-trip: bloco `housing` mudou na ida-e-volta")
	if JSON.stringify(before.get("urban", [])) != JSON.stringify(
			after.get("urban", [])):
		_fail("round-trip: bloco `urban` mudou na ida-e-volta")
	# Continua rodando depois do load sem violar invariante.
	if not _tick(sim, 5, "roundtrip-pos-load"):
		return


func _test_thread_determinism() -> void:
	var one := _make_world(12)
	var eight := _make_world(12)
	one.set_sim_threads(1)
	eight.set_sim_threads(8)
	if not _tick(one, 12, "threads=1"):
		return
	if not _tick(eight, 12, "threads=8"):
		return
	var a: Dictionary = one.to_dict()
	var b: Dictionary = eight.to_dict()
	if JSON.stringify(a) != JSON.stringify(b):
		_fail("determinismo: estado final difere entre threads=1 e 8")
	if JSON.stringify(a.get("housing", [])) != JSON.stringify(
			b.get("housing", [])):
		_fail("determinismo: bloco `housing` difere entre threads=1 e 8")
