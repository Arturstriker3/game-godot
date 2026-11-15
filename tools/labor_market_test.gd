extends SceneTree

## F7 — teste do mercado de trabalho, qualificação e renda.
##
## Valida o contrato do núcleo com um mundo SINTÉTICO controlado (uma cidade e
## uma construção de grande porte) em vez do cenário real: o objetivo é isolar
## cada regra, não medir o mundo (isso é o `economy_world_test`).
##
##   1. vaga exige nível: vaga qualificada sem trabalhador qualificado fica
##      aberta e NÃO vira emprego do nível;
##   2. escassez sobe o salário do nível e sobra derruba;
##   3. piso de salário mínimo nunca é furado;
##   4. qualificação é gradual (coorte de estudo, sem salto instantâneo);
##   5. a renda da cidade define o nível de consumo (basic/life/luxury).
##
##   godot --headless --path . --script res://tools/labor_market_test.gd

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"

const CITY_ID := "TST-C"
const CITY_NATION := "TST"
const CITY_POPULATION := 100000.0
## Construção única de teste: porte grande (30 mil vagas) para que a demanda
## por nível supere com folga a oferta de um perfil educacional controlado.
const BUILDING_CAPACITY := 4000.0

var _failures: Array[String] = []


func _init() -> void:
	_test_vacancy_requires_level()
	_test_tightness_moves_wages()
	_test_minimum_wage_floor()
	_test_qualification_is_gradual()
	_test_income_defines_consumption()

	if _failures.is_empty():
		print("[labor] OK: vagas por nível, salário dinâmico, piso, qualificação e renda validados.")
		quit(0)
	else:
		print("[labor] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


## Mundo mínimo: bens/receitas/necessidades reais, uma cidade e (opcionalmente)
## uma construção de grande porte. O índice de renda externa entra ANTES do
## perfil (o setter reaplica o perfil derivado da renda e apagaria as frações
## controladas).
func _new_world(income_index: float, profile: Dictionary,
		with_building: bool = true) -> Object:
	var sim = EconomySim.new()
	sim.demo_set_goods(_load_json_array(GOODS_PATH))
	sim.demo_set_recipes(_load_json_array(RECIPES_PATH))
	sim.demo_set_needs(_load_json_dict(NEEDS_PATH))
	var buildings: Array = []
	if with_building:
		buildings = [{
			"id": "TST-FARM",
			"city_id": CITY_ID,
			"resource": "wheat",
			"output_tons_per_day": BUILDING_CAPACITY,
			"aptitude": 1.0,
		}]
	sim.initialize_demo_world(
		[{
			"id": CITY_ID,
			"name": "Teste",
			"nation": CITY_NATION,
			"population": CITY_POPULATION,
			"x": 100.0,
			"y": 100.0,
		}],
		[],
		buildings)
	sim.demo_set_external_income_index(CITY_ID, income_index)
	sim.demo_set_labor_profile(CITY_ID, profile)
	return sim


func _profile(education: Array, wages: Array) -> Dictionary:
	return {
		"working_age_share": 0.60,
		"education": education,
		"participation": [0.90, 0.90, 0.90],
		"wage_by_level": wages,
	}


func _pop(status: Dictionary) -> Dictionary:
	for city in status.get("cities", []):
		if String(city.get("id", "")) == CITY_ID:
			# O status vem do cache compartilhado do nó: copiar evita que uma
			# leitura "antes" mude depois de um tick.
			return (city.get("pop", {}) as Dictionary).duplicate(true)
	return {}


func _wages(status: Dictionary) -> Array:
	var pop := _pop(status)
	var wages: Array = (pop.get("wage_by_level", []) as Array).duplicate()
	if wages.size() < 3:
		var labor: Dictionary = pop.get("labor", {})
		wages = (labor.get("wage_by_level", []) as Array).duplicate()
	return wages


## 1) Uma vaga qualificada não é preenchida por trabalhador de nível inferior.
##    Com toda a força no básico, as vagas de intermediário/superior continuam
##    abertas e o emprego do nível superior fica em zero — enquanto há básicos
##    desempregados ao mesmo tempo.
func _test_vacancy_requires_level() -> void:
	var sim = _new_world(1.0, _profile([1.0, 0.0, 0.0], [0.030, 0.040, 0.050]))
	var status: Dictionary = sim.tick_demo()
	var pop := _pop(status)
	var vacancies: Array = pop.get("vacancies_by_level", [])
	var employed: Array = pop.get("employed_by_level", [])
	var unemployed: Array = pop.get("unemployed_by_level", [])
	if vacancies.size() < 3 or employed.size() < 3:
		_fail("vaga por nivel: status sem vacancies_by_level/employed_by_level")
		return
	if float(vacancies[2]) <= 0.0:
		_fail("vaga por nivel: construcao de 4000 t/dia sem vaga superior")
	if float(employed[2]) > 1e-6:
		_fail("vaga por nivel: superior empregado (%.2f) sem nenhum trabalhador superior" %
			float(employed[2]))
	if float(vacancies[1]) <= 0.0:
		_fail("vaga por nivel: construcao sem vaga intermediaria")
	if float(unemployed[0]) <= 0.0:
		_fail("vaga por nivel: esperava basicos desempregados no mundo de teste")
	print("[labor] vaga por nivel: vagas=%s empregados=%s desempregados=%s" % [
		str(vacancies), str(employed), str(unemployed)])


## 2) Sem trabalhadores superiores/intermediários, o salário desses níveis sobe
##    (escassez); com sobra de básicos, o salário básico cai. O passo é diário e
##    limitado pela rigidez — nada de salto num dia.
func _test_tightness_moves_wages() -> void:
	var sim = _new_world(1.0, _profile([1.0, 0.0, 0.0], [0.030, 0.040, 0.050]))
	var wages_initial := _wages(sim.get_demo_status())
	for day in 20:
		sim.tick_demo()
	var wages_final := _wages(sim.get_demo_status())
	if wages_initial.size() < 3 or wages_final.size() < 3:
		_fail("salario dinamico: status sem wage_by_level")
		return
	if float(wages_final[2]) <= float(wages_initial[2]):
		_fail("escassez nao subiu salario superior (%.5f -> %.5f)" % [
			float(wages_initial[2]), float(wages_final[2])])
	if float(wages_final[0]) >= float(wages_initial[0]):
		_fail("sobra nao derrubou salario basico (%.5f -> %.5f)" % [
			float(wages_initial[0]), float(wages_final[0])])
	# Rigidez: em 1 dia a variação fica pequena (passo = salário × rigidez).
	var sim_day = _new_world(1.0, _profile([1.0, 0.0, 0.0], [0.030, 0.040, 0.050]))
	var before := _wages(sim_day.get_demo_status())
	sim_day.tick_demo()
	var after := _wages(sim_day.get_demo_status())
	if before.size() < 3 or after.size() < 3:
		_fail("salario dinamico: status sem wage_by_level no passo diario")
		return
	var daily_move := absf(float(after[2]) - float(before[2]))
	if daily_move > float(before[2]) * 0.25:
		_fail("ajuste diario grande demais (%.5f de %.5f)" % [
			daily_move, float(before[2])])
	print("[labor] salario dinamico: basico %.5f -> %.5f | superior %.5f -> %.5f" % [
		float(wages_initial[0]), float(wages_final[0]),
		float(wages_initial[2]), float(wages_final[2])])


## 3) O piso de salário mínimo é soberano: nenhum nível fica abaixo dele.
func _test_minimum_wage_floor() -> void:
	var sim = _new_world(1.0, _profile([1.0, 0.0, 0.0], [0.010, 0.012, 0.014]))
	sim.demo_set_minimum_wage(CITY_NATION, 0.050)
	for day in 10:
		sim.tick_demo()
	var wages := _wages(sim.get_demo_status())
	if wages.size() < 3:
		_fail("piso salarial: status sem wage_by_level")
		return
	for level in 3:
		if float(wages[level]) < 0.05 - 1e-9:
			_fail("piso salarial: nivel %d em %.5f < 0.05" % [level, float(wages[level])])
	print("[labor] piso salarial: salarios=%s" % str(wages))


## 4) Qualificação gradual: o prêmio abre matrícula, mas ninguém se forma no dia
##    seguinte (coorte de 3 anos) e a escola não esvazia a produção.
func _test_qualification_is_gradual() -> void:
	var sim = _new_world(1.0, _profile([0.95, 0.05, 0.0], [0.020, 0.040, 0.060]))
	var status: Dictionary = sim.get_demo_status()
	var pop := _pop(status)
	var education_before: Array = pop.get("education", [])
	if education_before.size() < 3:
		_fail("qualificacao: status sem education")
		return
	for day in 30:
		status = sim.tick_demo()
	pop = _pop(status)
	var studying: Array = pop.get("studying", [])
	var education_after: Array = pop.get("education", [])
	if studying.size() < 1 or float(studying[0]) <= 0.0:
		_fail("qualificacao: premio salarial nao abriu matricula (estudando=0)")
		return
	var advanced := float(education_after[1]) - float(education_before[1])
	var share := advanced / maxf(1.0, float(education_before[0]) + float(education_before[1]))
	if share > 0.02:
		_fail("qualificacao instantanea: %.4f do nivel mudou em 30 dias" % share)
	if share < 0.0:
		_fail("qualificacao: nivel intermediario diminuiu (%.4f)" % advanced)
	print("[labor] qualificacao: estudantes=%.1f avancaram=%.2f (%.4f)" % [
		float(studying[0]), advanced, share])


## 5) A renda define o nível de consumo: cidade rica alcança life/luxury, cidade
##    sem renda fica sem acesso. Sem construção (só renda externa + serviços), o
##    contraste é o da RENDA, não o do estoque produzido.
func _test_income_defines_consumption() -> void:
	var poor = _new_world(0.0, _profile([0.9, 0.09, 0.01], [0.020, 0.030, 0.040]), false)
	var rich = _new_world(5.0, _profile([0.9, 0.09, 0.01], [0.020, 0.030, 0.040]), false)
	for day in 12:
		poor.tick_demo()
		rich.tick_demo()
	var poor_level := int(_pop(poor.get_demo_status()).get("income_level", 0))
	var rich_level := int(_pop(rich.get_demo_status()).get("income_level", 0))
	if rich_level <= poor_level:
		_fail("renda nao separou consumo: pobre=%d rico=%d" % [poor_level, rich_level])
	if rich_level < 2:
		_fail("cidade rica nao alcancou life (nivel %d)" % rich_level)
	if poor_level > 1:
		_fail("cidade sem renda passou do basic (nivel %d)" % poor_level)
	print("[labor] nivel de consumo: sem renda=%d rica=%d" % [poor_level, rich_level])


func _fail(message: String) -> void:
	_failures.append(message)
	print("[labor] FALHOU: " + message)


func _load_json_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("dados: nao abriu " + path)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


func _load_json_dict(path: String) -> Dictionary:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("dados: nao abriu " + path)
		return {}
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_DICTIONARY else {}
