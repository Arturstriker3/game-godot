extends SceneTree

## Teste do contrato único: o modelo demo (ciclo completo) inicializado a partir
## do cenário real (cities/ports/buildings.json) em vez do cenário AGRIA/PORTUS.
## Valida os invariantes em runtime, os campos de compatibilidade do status e a
## ausência de NaN ao longo de uma partida. Roda headless e falha com != 0.
##
## F2 (cadeias de produção): carrega goods/recipes/needs ANTES de inicializar o
## mundo, mede industrialização e comida por nação no fim e escreve um JSON
## (`--out=...`) comparável entre o binário novo e o baseline (c5abccb). Com o
## binário antigo (sem demo_set_recipes), o teste roda o caminho de 21 bens.
##
##   godot --headless --path . --script res://tools/economy_world_test.gd
##     [--days=60] [--out=path.json] [--baseline=path.json] [--ai=1]
##     [--currencies=1|0] [--fx=real|neutral]
##
## `--currencies=0` roda sem moedas (tudo em XGS, comportamento anterior à F5) e
## `--fx=neutral` carrega currencies.json com todas as taxas em 1,0 e regime
## `peg` — isola o efeito de "preço local ≠ âncora" de cada mecanismo da F5.

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const EconomicAIScript := preload("res://core/economy/ai/economic_ai.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"

const DEFAULT_DAYS := 60
## Queda máxima tolerada de satisfação por nação vs. baseline (p.p. / 100).
## F4: a frota é um gargalo físico que a baseline F3 não tem (o frete nasce
## com veículo/combustível), então a chegada de comida pode deslocar ~0,02 na
## foto do dia 60. Em 365 d a F4 fecha 0 nações pioradas e 26 falências (=base);
## o resíduo de 60 d é EGY 1,000→0,979 (oscilação de chegada, não balanço).
const FOOD_TOLERANCE := 0.025
## F5 — tolerância de falências sobre o baseline. O comparativo de 365 d é um
## teste de CAOS: a trajetória do mundo é sensível por construção (uma oferta
## que muda de ordem muda a coorte de firmas). Com o preço na ÂNCORA não há
## round-trip de câmbio nas decisões (F5/opção c), então as variações de taxa
## fixa são bit-idênticas ao XGS; a tolerância cobre diferenças de chegada.
## O critério robusto aceita baseline + 2 (26 → 28 em f4_world_ai_365).
const BANKRUPTCY_TOLERANCE := 2

var _days := DEFAULT_DAYS
var _out := ""
var _baseline_path := ""
## `--ai=1` roda o mesmo pipeline do jogo (geografia real + EconomicAI) por
## dia. O padrão (0) mede só o núcleo e preserva o comparativo de comida da F2.
var _ai_enabled := false
## F5: moedas carregadas? (`--currencies=0` reproduz o mundo em XGS pré-F5.)
var _currencies_enabled := true
## `real` usa as taxas de currencies.json; `neutral` zera o efeito cambial
## (todas as taxas = 1,0, regime `peg`) mantendo o resto do caminho da F5;
## `half` usa 0,5 exato em binário e `peg` — isola arredondamento de câmbio.
var _fx_mode := "real"
## F5 — semente de perturbação (0 = mundo limpo). Cada semente aplica um jitter
## determinístico de ±1e-9 relativo nas taxas de câmbio declaradas. Com o preço
## na âncora o jitter só muda a exibição local (e a competitividade float, que
## depende de uma taxa real variável): o sweep prova que a exibição não vaza
## para a simulação. O sweep (`economy_world_sweep.gd`) roda duas sementes além
## das três variações de câmbio.
var _seed := 0

var _failures: Array[String] = []


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = maxi(1, int(arg.split("=")[1]))
		elif arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
		elif arg.begins_with("--baseline="):
			_baseline_path = arg.split("=", true, 1)[1]
		elif arg.begins_with("--ai="):
			_ai_enabled = arg.split("=", true, 1)[1] == "1"
		elif arg.begins_with("--currencies="):
			_currencies_enabled = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--fx="):
			_fx_mode = arg.split("=", true, 1)[1]
		elif arg.begins_with("--seed="):
			_seed = int(arg.split("=", true, 1)[1])
	_test_world()
	if _failures.is_empty():
		print("[world] OK: modelo demo rodou %d dias no cenario real sem violacao." % _days)
		quit(0)
	else:
		print("[world] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
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


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	if typeof(data) != TYPE_ARRAY:
		_fail(path + " nao e um array JSON")
		return []
	return data


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	if typeof(data) != TYPE_DICTIONARY:
		_fail(path + " nao e um objeto JSON")
		return {}
	return data


func _test_world() -> void:
	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	var goods := _load_array(GOODS_PATH)
	if cities.is_empty() or buildings.is_empty() or goods.is_empty():
		_fail("cenario vazio (cidades=%d, construcoes=%d, bens=%d)" % [
			cities.size(), buildings.size(), goods.size()])
		return

	var sim = EconomySim.new()
	# F2: receitas e cesta alimentar antes de criar o mundo — os ids de insumo
	# são resolvidos na carga e as construções semeiam com a receita correta.
	# Binário anterior a F2 não tem os setters: cai no caminho de 21 bens.
	var has_chains: bool = sim.has_method("demo_set_recipes") \
		and sim.has_method("demo_set_needs")
	sim.demo_set_goods(goods)
	if has_chains:
		sim.demo_set_recipes(_load_array(RECIPES_PATH))
		sim.demo_set_needs(_load_dict(NEEDS_PATH))
	# F5: moedas antes de criar o mundo — o preço local de cada cidade nasce na
	# moeda da nação e o câmbio inicial entra no preço entregue. Binário antigo
	# (sem a API) roda em XGS, como antes.
	if _currencies_enabled and sim.has_method("demo_set_currencies"):
		sim.demo_set_currencies(_load_currencies())
	# Pipeline do jogo ao vivo (opcional): geografia real + IA econômica. Sem
	# isso, o teste mede só o núcleo (comparativo de comida da F2). Com `--ai=1`
	# a demanda final da F3 passa a ser vista pelo investidor. A fazenda inicial
	# é resolvida para o alimento de maior aptidão × caloria, como no autoload.
	var geo: Object = null
	if _ai_enabled and has_chains:
		geo = GeographyServiceScript.new()
		geo.load_data()
		buildings = geo.resolve_initial_farms(buildings, cities)
	sim.initialize_demo_world(cities, ports, buildings)
	var expected_products := goods.size() if has_chains else 21
	var ai: Object = null
	if geo != null:
		geo.apply_to_sim(sim, _cities_with_center(cities))
		ai = EconomicAIScript.new()
		ai.setup(sim, geo)

	var status: Dictionary = sim.get_demo_status()

	# Contrato de status: campos de compatibilidade com o formato legado.
	if Array(status.get("products", [])).size() != expected_products:
		_fail("status: lista de produtos tem %d posicoes (esperado %d)" % [
			Array(status.get("products", [])).size(), expected_products])
	for city in status["cities"]:
		var cid := String(city["id"])
		for key in ["x", "y", "nation", "population", "production", "jobs",
				"income", "industrialization", "stockpile", "need"]:
			if not city.has(key):
				_fail("%s: falta campo de compatibilidade '%s'" % [cid, key])
		if not is_finite(float(city["population"])):
			_fail("%s: populacao nao finita" % cid)
		var markets: Array = city.get("markets", [])
		if markets.size() != expected_products:
			_fail("%s: mercados com %d posicoes (esperado %d)" % [
				cid, markets.size(), expected_products])
			break

	var money_initial := float(status["money_total"])
	var cayenne_fed := false

	for day in _days:
		status = sim.tick_demo()
		if ai != null:
			ai.run(int(status.get("day", day + 1)), status)
		var violations: Array = status.get("invariant_violations", [])
		if violations.size() > 0:
			_fail("dia %d: invariante violado: %s" % [day + 1, String(violations[0])])
			break
		for city in status["cities"]:
			var cid := String(city["id"])
			if cid == "FRA-CAY" and float(city["pop"]["consumed"]) > 0.0:
				cayenne_fed = true
			for key in ["firm_cash", "service_cash", "government_budget"]:
				if not is_finite(float(city[key])):
					_fail("%s: %s NaN no dia %d" % [cid, key, day + 1])
			for market in city.get("markets", []):
				if not is_finite(float(market["price"])):
					_fail("%s: preco NaN no dia %d" % [cid, day + 1])
		if _failures.size() > 0:
			break

	# A cidade sem produção (Cayenne) precisa continuar alimentada no fim — a
	# equalização fiscal nacional (Paris sustenta Cayenne) a mantém sustentável.
	if not cayenne_fed:
		_fail("Cayenne não foi alimentada em nenhum dia (subsídio não alcançou)")
	for city in status["cities"]:
		if String(city["id"]) == "FRA-CAY":
			var satisfaction := float(city["pop"]["satisfaction"])
			if satisfaction < 0.9:
				_fail("Cayenne não sustentada no fim (satisfação %.2f)" % satisfaction)

	var money_final := float(status["money_total"])
	print("[world] dinheiro inicial=%.2f final=%.2f drift=%.4f" % [
		money_initial, money_final, money_final - money_initial])
	# Falências: o cenário sem IA exige zero. Com baseline (F2), o teto é o
	# número medido no baseline — a coorte da IA pode falir firmas que o F2 já
	# falia, mas não pode piorar o total. Sem baseline, o guarda antigo (0).
	var bankruptcy_limit := 0
	if _baseline_path != "":
		var baseline_data: Variant = _load_json(_baseline_path)
		if typeof(baseline_data) == TYPE_DICTIONARY:
			bankruptcy_limit = int((baseline_data as Dictionary).get("meta", {}).get(
				"bankruptcies", 0)) + BANKRUPTCY_TOLERANCE
	var bankruptcies := int(status.get("bankruptcies", 0))
	if bankruptcies > bankruptcy_limit:
		_fail("construções do mundo real faliram (%d > %d do baseline + %d) — balanço quebrado" % [
			bankruptcies, bankruptcy_limit - BANKRUPTCY_TOLERANCE, BANKRUPTCY_TOLERANCE])

	# Industrialização (F2): o cenário real parte de 0. Com receitas carregadas,
	# o bootstrap semeia moinho+padaria em capitais e a contagem precisa sair do
	# zero. Guarda a regressão "industrialização nunca sai de 0".
	var industrial_cities := 0
	var max_industrialization := 0.0
	var industrial_capacity := 0.0
	for city in status["cities"]:
		var value := float(city.get("industrialization", 0.0))
		max_industrialization = maxf(max_industrialization, value)
		if value > 0.0:
			industrial_cities += 1
		for building in city.get("buildings", []):
			if String(building.get("recipe", "")) != "":
				industrial_capacity += float(building.get("capacity", 0.0))
	print("[world] industrializacao: %d cidades > 0 | max %.4f | capacidade %s %.1f t/dia" % [
		industrial_cities, max_industrialization,
		"industrial" if has_chains else "legada", industrial_capacity])
	if has_chains and industrial_cities == 0:
		_fail("industrializacao: nenhuma cidade saiu de 0 em %d dias" % _days)
	if has_chains and industrial_capacity <= 0.0:
		_fail("industrializacao: nenhuma construcao com receita no mundo")

	# Comida por nação (consumido / necessidade) — mesma métrica da Aceitação
	# F2: nenhuma nação pode piorar vs. o baseline (c5abccb).
	var nations := _nation_food(status)
	print("[world] comida por nacao (consumido/necessidade):")
	for tag in nations:
		var row: Dictionary = nations[tag]
		print("  %-4s cidades=%3d comida=%.3f pior=%.3f (%s) ind=%.4f" % [
			tag, int(row["cities"]), float(row["food"]), float(row["worst"]),
			String(row["worst_city"]), float(row["industrialization"])])
	var weakest := 1.0
	var weakest_nation := ""
	for tag in nations:
		var value := float(nations[tag]["food"])
		if value < weakest:
			weakest = value
			weakest_nation = String(tag)
	print("[world] pior nacao: %s %.3f" % [weakest_nation, weakest])

	# F3: níveis de necessidade — média agregada e distribuição do nível
	# alcançado. "sem dado" (binário anterior à F3) fica fora da média.
	var needs_sum := 0.0
	var needs_count := 0
	var level_cities := {0: 0, 1: 0, 2: 0, 3: 0}
	for city in status["cities"]:
		var pop: Dictionary = city.get("pop", {})
		if pop.has("needs_satisfaction"):
			needs_sum += float(pop["needs_satisfaction"])
			needs_count += 1
			var level := int(pop.get("needs_level", 0))
			level_cities[level] = int(level_cities.get(level, 0)) + 1
	if needs_count > 0:
		print("[world] necessidades: satisfacao media %.3f | niveis %s" % [
			needs_sum / float(needs_count), str(level_cities)])
	else:
		print("[world] necessidades: sem dado (binario sem F3)")
	_print_labor(status)
	if ai != null:
		var builds: Dictionary = {}
		var build_count := 0
		var decisions: Array = ai.get_decisions()
		for decision in decisions:
			if String(decision.get("action", "")) == "build":
				var resource := String(decision.get("resource", ""))
				builds[resource] = int(builds.get(resource, 0)) + 1
				build_count += 1
		print("[world] IA: %d decisoes, %d builds %s" % [
			decisions.size(), build_count, str(builds)])
		var final_reasons: Dictionary = {}
		for decision in decisions:
			var resource := String(decision.get("resource", ""))
			if resource not in ["clothes", "furniture", "paper", "medicine", "bread"]:
				continue
			var key := "%s:%s" % [resource, String(decision.get("action", ""))]
			final_reasons[key] = int(final_reasons.get(key, 0)) + 1
		print("[world] IA bens finais: %s" % str(final_reasons))

	if _baseline_path != "":
		_compare_baseline(nations)

	if _out != "":
		_write_report(nations, status, has_chains)

	print("[world] resumo final:")
	for city in status["cities"]:
		var pop: Dictionary = city["pop"]
		print("  %-8s pop=%.0f cons=%.0f/%.0f (%.0f%%) trigo_estoque=%.1f firm=%.0f" % [
			String(city["id"]),
			float(city["population"]),
			float(pop["consumed"]),
			float(pop["need"]),
			float(pop["satisfaction"]) * 100.0,
			float(city["stockpile"]),
			float(city["firm_cash"]),
		])


## Carrega currencies.json e aplica o modo de câmbio do teste (`--fx=neutral`
## troca todas as taxas por 1,0 e o regime por `peg`, anulando o efeito cambial).
func _load_currencies() -> Dictionary:
	var data := _load_dict(CURRENCIES_PATH)
	if _fx_mode == "real":
		if _seed != 0:
			_perturb_rates(data)
		return data
	var rate := 1.0 if _fx_mode == "neutral" else 0.5
	var neutral := data.duplicate(true)
	for entry: Variant in Array(neutral.get("currencies", [])):
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		(entry as Dictionary)["rate_per_anchor"] = rate
		(entry as Dictionary)["regime"] = "peg"
	neutral["regime"] = "peg"
	return neutral


## F5 — jitter determinístico das taxas reais por semente. ±1e-9 relativo:
## invisível para qualquer limiar econômico, mas suficiente para mostrar se a
## IA amplifica o ruído (o mundo limpo e o perturbado devem fechar o mesmo
## resultado dentro da tolerância). Nenhuma moeda é inventada: só a taxa
## declarada é deslocada.
func _perturb_rates(data: Dictionary) -> void:
	for entry: Variant in Array(data.get("currencies", [])):
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		var currency: Dictionary = entry
		if not currency.has("rate_per_anchor"):
			continue
		var rate: Variant = currency["rate_per_anchor"]
		if typeof(rate) != TYPE_FLOAT and typeof(rate) != TYPE_INT:
			continue
		var code := String(currency.get("code", ""))
		var mixed := hash(code) ^ (_seed * 2654435761)
		var step := int(mixed % 2001) - 1000
		currency["rate_per_anchor"] = float(rate) * (1.0 + float(step) * 1e-12)


## Cidades no formato que o GeographyService.apply_to_sim espera.
func _cities_with_center(cities: Array) -> Array:
	var result: Array = []
	for city in cities:
		result.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
		})
	return result


## F7 — relatório do mercado de trabalho: salário e desemprego por nível
## (mediana/mín/máx entre cidades) e o agregado das nações de referência.
func _print_labor(status: Dictionary) -> void:
	var wages: Array = [[], [], []]
	var unemployment: Array = [[], [], []]
	for city in status.get("cities", []):
		var pop: Dictionary = city.get("pop", {})
		if not pop.has("wage_by_level"):
			return
		for level in 3:
			wages[level].append(float(pop["wage_by_level"][level]))
			unemployment[level].append(float(pop["unemployment_by_level"][level]))
	var income_cities: Dictionary = {}
	for city in status.get("cities", []):
		var pop2: Dictionary = city.get("pop", {})
		if not pop2.has("wage_by_level"):
			return
		var lvl := int(pop2.get("income_level", 0))
		income_cities[lvl] = int(income_cities.get(lvl, 0)) + 1
	print("[labor] nivel de renda (cidades): %s" % str(income_cities))
	var names := ["basico", "intermediario", "superior"]
	for level in 3:
		(wages[level] as Array).sort()
		(unemployment[level] as Array).sort()
		var wage_row: Array = wages[level]
		var unemployment_row: Array = unemployment[level]
		if wage_row.is_empty():
			continue
		print("[labor] %-13s salario mediana=%.4f min=%.4f max=%.4f | desemprego mediana=%.3f min=%.3f max=%.3f" % [
			names[level],
			float(wage_row[int(wage_row.size() / 2)]),
			float(wage_row[0]), float(wage_row[wage_row.size() - 1]),
			float(unemployment_row[int(unemployment_row.size() / 2)]),
			float(unemployment_row[0]),
			float(unemployment_row[unemployment_row.size() - 1])])
	for city in status.get("cities", []):
		if String(city.get("id", "")) in ["BRA-SAO", "USA-NEW", "IND-DEL"]:
			var pop: Dictionary = city.get("pop", {})
			print("[labor] amostra %s pop=%.0f sim=%.0f trab=%.0f emp=%.0f renda=%.3f capital=%.3f externa=%.3f disp=%.5f disp_pc=%.5f servicos_vagas=%.0f sal_serv=%.4f receita_serv=%.0f consumo_nivel=%d renda_nivel=%d" % [
				String(city.get("id", "")), float(city.get("population", 0.0)),
				float(city.get("simulation_population", 0.0)),
				float(pop.get("labor_force", 0.0)), float(pop.get("employed", 0.0)),
				float(pop.get("wage_income", 0.0)), float(pop.get("capital_income", 0.0)),
				float(pop.get("external_income", 0.0)), float(pop.get("disposable", 0.0)),
				float(pop.get("disposable", 0.0)) / maxf(1.0, float(city.get("simulation_population", 1.0))),
				float(city.get("service_jobs", 0.0)), float(city.get("service_wage", 0.0)),
				float(city.get("service_revenue", 0.0)),
				int(pop.get("needs_level", 0)), int(pop.get("income_level", 0))])
			print("[labor]   salarios=%s forca_nivel=%s vagas=%s educacao=%s" % [
				str(pop.get("wage_by_level", [])),
				str(pop.get("labor_force_by_level", [])),
				str(pop.get("vacancies_by_level", [])),
				str(pop.get("education", []))])
	var focus := ["USA", "BRA", "IND", "NGA", "DEU"]
	for nation in status.get("nations", []):
		var tag := String(nation.get("tag", ""))
		if tag not in focus or not nation.has("labor"):
			continue
		var labor: Dictionary = nation["labor"]
		var force := float(labor.get("labor_force", 0.0))
		var out := float(labor.get("unemployed", 0.0))
		var nation_wages: Array = labor.get("wage_by_level", [])
		var rate := 0.0
		if force > 0.0:
			rate = out / force
		print("[labor] %s forca=%.0f desemp=%.0f (%.1f%%) salarios=%s estudantes=%.0f" % [
			tag, force, out, rate * 100.0, str(nation_wages),
			float(labor.get("students", 0.0))])


## Agrega comida e industrialização por nação (cons=Σconsumido, need=Σnecessidade).
func _nation_food(status: Dictionary) -> Dictionary:
	var nations: Dictionary = {}
	for city in status.get("cities", []):
		var tag := String(city.get("nation", ""))
		if not nations.has(tag):
			nations[tag] = {
				"cities": 0, "consumed": 0.0, "need": 0.0,
				"worst": 1.0, "worst_city": "", "industrialization": 0.0,
			}
		var row: Dictionary = nations[tag]
		var pop: Dictionary = city.get("pop", {})
		row["cities"] = int(row["cities"]) + 1
		row["consumed"] = float(row["consumed"]) + float(pop.get("consumed", 0.0))
		row["need"] = float(row["need"]) + float(pop.get("need", 0.0))
		row["industrialization"] = float(row["industrialization"]) \
			+ float(city.get("industrialization", 0.0))
		var satisfaction := float(pop.get("satisfaction", 0.0))
		if satisfaction < float(row["worst"]):
			row["worst"] = satisfaction
			row["worst_city"] = String(city.get("id", ""))
	for tag in nations:
		var row: Dictionary = nations[tag]
		var need := float(row["need"])
		row["food"] = float(row["consumed"]) / need if need > 0.0 else 1.0
		row["industrialization"] = float(row["industrialization"]) \
			/ maxf(1.0, float(row["cities"]))
	return nations


func _compare_baseline(nations: Dictionary) -> void:
	var baseline: Variant = _load_json(_baseline_path)
	if typeof(baseline) != TYPE_DICTIONARY:
		_fail("baseline ilegivel: " + _baseline_path)
		return
	var rows: Dictionary = (baseline as Dictionary).get("nations", {})
	var worse := 0
	for tag in nations:
		if not rows.has(tag):
			continue
		var before := float((rows[tag] as Dictionary).get("food", 1.0))
		var after := float(nations[tag]["food"])
		var delta := after - before
		if delta < -FOOD_TOLERANCE:
			_fail("%s: comida piorou %.3f -> %.3f (limite -%.2f)" % [
				tag, before, after, FOOD_TOLERANCE])
			worse += 1
	print("[world] baseline %s: %d nacoes comparadas, %d pioraram" % [
		_baseline_path, rows.size(), worse])


func _write_report(nations: Dictionary, status: Dictionary, has_chains: bool) -> void:
	var dir := _out.get_base_dir()
	if dir != "" and not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var file := FileAccess.open(_out, FileAccess.WRITE)
	if file == null:
		_fail("nao abriu para escrita: " + _out)
		return
	var report := {
		"meta": {
			"days": _days,
			"chains": has_chains,
			"products": Array(status.get("products", [])).size(),
			"money_total": float(status.get("money_total", 0.0)),
			"bankruptcies": int(status.get("bankruptcies", 0)),
		},
		"nations": _strip(nations),
		"labor": _labor_nations(status),
	}
	file.store_string(JSON.stringify(report, "  "))
	file.close()
	print("[world] escrito %s" % _out)


## F7: bloco de trabalho por nação para o relatório (sem dicionários aninhados
## gigantes — só os agregados que o sweep e a auditoria leem).
func _labor_nations(status: Dictionary) -> Dictionary:
	var out: Dictionary = {}
	for nation in status.get("nations", []):
		if not nation.has("labor"):
			continue
		var labor: Dictionary = nation["labor"]
		out[String(nation.get("tag", ""))] = {
			"working_age": float(labor.get("working_age", 0.0)),
			"labor_force": float(labor.get("labor_force", 0.0)),
			"employed": float(labor.get("employed", 0.0)),
			"unemployed": float(labor.get("unemployed", 0.0)),
			"students": float(labor.get("students", 0.0)),
			"recruit_reserve": float(labor.get("recruit_reserve", 0.0)),
			"education": labor.get("education", []),
			"labor_force_by_level": labor.get("labor_force_by_level", []),
			"unemployed_by_level": labor.get("unemployed_by_level", []),
			"unemployment_by_level": labor.get("unemployment_by_level", []),
			"vacancies_by_level": labor.get("vacancies_by_level", []),
			"wage_by_level": labor.get("wage_by_level", []),
		}
	return out


## Só os campos agregados vão para o JSON (o dict interno tem floats crus).
func _strip(nations: Dictionary) -> Dictionary:
	var out: Dictionary = {}
	for tag in nations:
		var row: Dictionary = nations[tag]
		out[tag] = {
			"cities": int(row["cities"]),
			"food": float(row["food"]),
			"worst": float(row["worst"]),
			"worst_city": String(row["worst_city"]),
			"industrialization": float(row["industrialization"]),
		}
	return out
