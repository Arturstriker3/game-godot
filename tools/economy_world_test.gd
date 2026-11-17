extends SceneTree

## Teste do contrato único: o modelo demo (ciclo completo) inicializado a partir
## do cenário real (cities/ports/buildings.json) em vez do cenário AGRIA/PORTUS.
## Valida os invariantes em runtime, os campos de compatibilidade do status e a
## ausência de NaN ao longo de uma partida. Roda headless e falha com != 0.
##
## F2 (cadeias de produção): carrega goods/recipes/needs ANTES de inicializar o
## mundo, mede industrialização e comida por nação no fim e escreve um JSON
## (`--out=...`) comparável entre o binário novo e o baseline (c5abccb). Com o
## binário antigo (sem set_recipes), o teste roda o caminho de 21 bens.
##
##   godot --headless --path . --script res://tools/economy_world_test.gd
##     [--days=60] [--out=path.json] [--baseline=path.json] [--ai=1]
##     [--currencies=1|0] [--fx=real|neutral|half|float] [--instant=1]
##     [--urban=1] [--threads=N] [--urban-cities=ID1,ID2] [--urban-out=path.json]
##
## `--currencies=0` roda sem moedas (tudo em XGS, comportamento anterior à F5) e
## `--fx=neutral` carrega currencies.json com todas as taxas em 1,0 e regime
## `peg` — isola o efeito de "preço local ≠ âncora" de cada mecanismo da F5.
##
## `--fx=float` é SONDA de sensibilidade do câmbio, não variante de aceite:
## currencies.json não declara `regime` (todas as moedas ficam `peg` e o
## caminho cambial fica inerte), então a sonda força `regime: float` no topo e
## em cada moeda — as TAXAS continuam as declaradas. Se a trajetória não muda,
## o câmbio não alcança a simulação. O relatório grava `meta.fx` (o modo usado)
## e `meta.trajectory_hash` (sha256 de nations+labor+sectors+ownership).

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const GroupNamerScript := preload("res://core/economy/group_namer.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
## §19 (W0): mesmos caminhos que o autoload do jogo usa para a camada urbana.
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

const DEFAULT_DAYS := 60
## Queda máxima tolerada de satisfação por nação vs. baseline (p.p. / 100).
## F4: a frota é um gargalo físico que a baseline F3 não tem (o frete nasce
## com veículo/combustível), então a chegada de comida pode deslocar a foto de
## curto prazo. Medido (Q-016, 2026-11-17, `--ai=1 --urban=1 --threads=1`):
## **EGY 1,000→0,857 em 60 d e →0,546 em 120 d** (EGY-CAI com estoque de trigo
## e caixa das firmas zerados), fechando em **1,000 em 365 d** — é o transiente
## de importação, não balanço, e o mesmo transiente existe SEM §19 (`--urban=0`:
## EGY 0,694 em 60 d). A nota antiga (0,979) subestimava o vale. O critério de
## aceite do sweep é o de 365 d; a corrida de 60/120 d só é comparável ao
## baseline de 365 d no fim.
const FOOD_TOLERANCE := 0.025
## F5 — tolerância de falências sobre o baseline. O comparativo de 365 d é um
## teste de CAOS: a trajetória do mundo é sensível por construção (uma oferta
## que muda de ordem muda a coorte de firmas). Com o preço na ÂNCORA não há
## round-trip de câmbio nas decisões (F5/opção c), então as variações de taxa
## fixa são bit-idênticas ao XGS — mas ATENÇÃO (Q-016): `currencies.json` não
## declara `regime` (todas as moedas ficam `peg`), então essa igualdade é
## estrutural e não cobre o caminho de competitividade float. Quem o mede é a
## sonda `--fx=float` (trajetória TEM de diferir; ver o sweep paralelo). A
## tolerância cobre diferenças de chegada.
## O critério robusto aceita baseline + 2 (26 → 28 em f4_world_ai_365).
const BANKRUPTCY_TOLERANCE := 2
## F9 — nações âncora da auditoria de propriedade (estatal / nacional / estrangeiro).
const OWNERSHIP_TAGS := ["USA", "DEU", "BRA", "IND", "NGA", "CHN"]
## Tolerância absoluta do drift de dinheiro no fim da corrida (R$). O núcleo
## reverte qualquer tick que viole o invariante, então o esperado é 0.
const MONEY_DRIFT_TOLERANCE := 0.01

var _days := DEFAULT_DAYS
var _out := ""
var _baseline_path := ""
## `--ai=1` roda o mesmo pipeline do jogo (geografia real + IA do núcleo) por
## dia. O padrão (0) mede só o núcleo e preserva o comparativo de comida da F2.
var _ai_enabled := false
## F5: moedas carregadas? (`--currencies=0` reproduz o mundo em XGS pré-F5.)
var _currencies_enabled := true
## `real` usa as taxas de currencies.json; `neutral` zera o efeito cambial
## (todas as taxas = 1,0, regime `peg`) mantendo o resto do caminho da F5;
## `half` usa 0,5 exato em binário e `peg` — isola arredondamento de câmbio;
## `float` é a SONDA de sensibilidade (regime `float` forçado, taxas do dado).
var _fx_mode := "real"
## F5 — semente de perturbação (0 = mundo limpo). Cada semente aplica um jitter
## determinístico de ±1e-9 relativo nas taxas de câmbio declaradas. Com o preço
## na âncora o jitter só muda a exibição local (e a competitividade float, que
## depende de uma taxa real variável): o sweep prova que a exibição não vaza
## para a simulação. O sweep (`economy_world_sweep.gd`) roda duas sementes além
## das três variações de câmbio.
var _seed := 0
## F8: `--services=0` roda o mundo sem o catálogo de serviços (comportamento
## F7). Serve para isolar o efeito da F8 num A/B do mesmo binário.
var _services_enabled := true
## Q-014/R3: `--instant=1` roda com construção instantânea (emula o mundo
## PRÉ-§15) para isolar o efeito do cronograma de obra. Diagnóstico: o padrão
## continua sendo obra física.
var _instant_construction := false
## F9: âncoras de emprego por setor (sector_employment.json), a fonte das
## referências comparadas no relatório. Sem o arquivo, o teste imprime "sem
## dado" — nenhum alvo é inventado aqui.
var _sector_data: Dictionary = {}
## Q-015: workers do núcleo (0 = automático, escolhido pelo núcleo). Só muda o
## paralelismo; nenhuma regra de simulação depende disso.
var _threads := 0
## Q-015: retrato urbano para o A/B de threads. `--urban-cities` é CSV de ids e
## `--urban-out` o JSON de saída; sem os dois, nada é coletado.
var _urban_cities := ""
var _urban_out := ""
## §19: `--urban=1` carrega solo/moradia/prefeitura como o autoload do jogo. O
## padrão (0) preserva o mundo sem dado urbano das baselines históricas.
var _urban_enabled := false

## Q-016: metadados de câmbio que vão para `meta.fx` no relatório (`mode`,
## `seed`, `enabled`, `currencies`, `floating`, `rate_sum`), preenchidos em
## `_load_currencies()`. O padrão é o mundo sem moedas: `--currencies=0` não
## chama o carregador e o relatório declara isso em vez de fingir câmbio.
var _fx_meta: Dictionary = {
	"mode": "off", "enabled": false, "currencies": 0, "floating": 0, "rate_sum": 0.0,
}

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
		elif arg.begins_with("--services="):
			_services_enabled = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--instant="):
			_instant_construction = arg.split("=", true, 1)[1] == "1"
		elif arg.begins_with("--urban="):
			_urban_enabled = arg.split("=", true, 1)[1] == "1"
		elif arg.begins_with("--threads="):
			_threads = maxi(0, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--urban-cities="):
			_urban_cities = arg.split("=", true, 1)[1]
		elif arg.begins_with("--urban-out="):
			_urban_out = arg.split("=", true, 1)[1]
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
	# Q-015: workers do núcleo antes de criar o mundo. Binário anterior à Q-015
	# não tem o setter e roda serial (0 = automático, decidido pelo núcleo).
	if _threads > 0 and sim.has_method("set_sim_threads"):
		sim.set_sim_threads(_threads)
	print("[world] threads=%d" % _threads)
	print("[world] urbano=%d" % int(_urban_enabled))
	# F2: receitas e cesta alimentar antes de criar o mundo — os ids de insumo
	# são resolvidos na carga e as construções semeiam com a receita correta.
	# Binário anterior a F2 não tem os setters: cai no caminho de 21 bens.
	var has_chains: bool = sim.has_method("set_recipes") \
		and sim.has_method("set_needs")
	sim.set_goods(goods)
	if _instant_construction and sim.has_method("set_instant_construction"):
		sim.set_instant_construction(true)
	if has_chains:
		sim.set_recipes(_load_array(RECIPES_PATH))
		sim.set_needs(_load_dict(NEEDS_PATH))
		# F8: serviços (catálogo). Binário anterior à F8 não tem o método.
		if _services_enabled and sim.has_method("set_services"):
			sim.set_services(_load_dict(SERVICES_PATH))
	# F5: moedas antes de criar o mundo — o preço local de cada cidade nasce na
	# moeda da nação e o câmbio inicial entra no preço entregue. Binário antigo
	# (sem a API) roda em XGS, como antes.
	if _currencies_enabled and sim.has_method("set_currencies"):
		sim.set_currencies(_load_currencies())
	# F9: estrutura de emprego por setor (âncoras ILO + coeficientes agrícolas).
	# Assim como as moedas, entra ANTES de criar o mundo; binário anterior à F9
	# não tem o setter e mantém o espelho compilado das mesmas âncoras.
	_sector_data = _load_dict(SECTOR_EMPLOYMENT_PATH)
	if sim.has_method("set_sector_employment") and not _sector_data.is_empty():
		sim.set_sector_employment(_sector_data)
	# Pipeline do jogo ao vivo (opcional): geografia real + IA econômica. Sem
	# isso, o teste mede só o núcleo (comparativo de comida da F2). Com `--ai=1`
	# a demanda final da F3 passa a ser vista pelo investidor. A fazenda inicial
	# é resolvida para o alimento de maior aptidão × caloria, como no autoload.
	var geo: Object = null
	if _ai_enabled and has_chains:
		geo = GeographyServiceScript.new()
		geo.load_data()
		buildings = geo.resolve_initial_farms(buildings, cities)
	sim.initialize_world(cities, ports, buildings)
	# §19 (W0): solo, moradia e prefeitura por cidade, como o autoload faz
	# (casamento por id da cidade). Fora deste bloco o runner mantém o mundo
	# sem dado urbano, que é o caminho das baselines históricas.
	if _urban_enabled:
		if not sim.has_method("set_urban"):
			_fail("binario sem set_urban (rode o build)")
			return
		sim.set_urban(_load_dict(URBAN_LAND_PATH),
			_load_dict(HOUSING_CITY_PATH), _load_dict(HOUSING_PATH),
			_load_dict(MUNICIPAL_FINANCE_PATH))
	var expected_products := goods.size() if has_chains else 21
	if geo != null:
		geo.apply_to_sim(sim, _cities_with_center(cities))
		# A IA econômica (privado, governo, grupos) roda no núcleo, na virada do
		# dia (A5). Grupos nomeados sem `places`: este runner não carrega a
		# cultura das cidades.
		sim.set_ai_enabled(true)
		GroupNamerScript.name_groups(sim)

	var status: Dictionary = sim.get_status()

	# Q-015: retrato urbano do início (só com `--urban-cities` e `--urban-out`).
	var urban_ids := _urban_ids()
	var urban_enabled: bool = not urban_ids.is_empty() and _urban_out != ""
	var urban_start: Dictionary = {}
	var urban_start_day := int(status.get("day", _days))
	if urban_enabled:
		urban_start = _urban_snapshot(sim, urban_ids)

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
		var markets: Array = sim.get_city_status(cid).get("markets", [])
		if markets.size() != expected_products:
			_fail("%s: mercados com %d posicoes (esperado %d)" % [
				cid, markets.size(), expected_products])
			break

	var money_initial := float(status["money_total"])
	var cayenne_fed := false

	for day in _days:
		status = sim.tick_day()
		var violations: Array = status.get("invariant_violations", [])
		if violations.size() > 0:
			_fail("dia %d: invariante violado: %s" % [day + 1, String(violations[0])])
			break
		# O status diário publica só os escalares leves por cidade: o consumo da
		# POP e os preços de mercado vêm do bloco `light` (arrays planos
		# cidade×produto, os MESMOS doubles do bloco pesado), sem remontar um
		# dicionário por cidade a cada dia do laço.
		var light: Dictionary = status.get("light", {})
		var prices: PackedFloat64Array = light.get("m_price", PackedFloat64Array())
		var consumed_by_city: PackedFloat64Array = light.get(
			"p_consumed", PackedFloat64Array())
		var products_per_city := int(light.get("product_count", 0))
		for slot in status["cities"].size():
			var city: Dictionary = status["cities"][slot]
			var cid := String(city["id"])
			if cid == "FRA-CAY" and slot < consumed_by_city.size() \
					and float(consumed_by_city[slot]) > 0.0:
				cayenne_fed = true
			for key in ["firm_cash", "service_cash", "government_budget"]:
				if not is_finite(float(city[key])):
					_fail("%s: %s NaN no dia %d" % [cid, key, day + 1])
			for offset in products_per_city:
				var at: int = int(slot) * products_per_city + int(offset)
				if at < prices.size() and not is_finite(float(prices[at])):
					_fail("%s: preco NaN no dia %d" % [cid, day + 1])
					break
		if _failures.size() > 0:
			break

	# Q-015: retrato urbano do fim, mesmo formato do início.
	var urban_end: Dictionary = {}
	var urban_end_day := int(status.get("day", _days))
	if urban_enabled:
		urban_end = _urban_snapshot(sim, urban_ids)

	# A cidade sem produção (Cayenne) precisa continuar alimentada no fim — a
	# equalização fiscal nacional (Paris sustenta Cayenne) a mantém sustentável.
	if not cayenne_fed:
		_fail("Cayenne não foi alimentada em nenhum dia (subsídio não alcançou)")
	for city in status["cities"]:
		if String(city["id"]) == "FRA-CAY":
			var satisfaction := float(sim.get_city_status("FRA-CAY").get(
				"pop", {}).get("satisfaction", 0.0))
			if satisfaction < 0.9:
				_fail("Cayenne não sustentada no fim (satisfação %.2f)" % satisfaction)

	var money_final := float(status["money_total"])
	print("[world] dinheiro inicial=%.2f final=%.2f drift=%.4f" % [
		money_initial, money_final, money_final - money_initial])
	# F9: conservação explícita no fim da corrida (os invariantes por tick já
	# revertem violações; este guarda pega drift acumulado silencioso).
	if absf(money_final - money_initial) > MONEY_DRIFT_TOLERANCE:
		_fail("dinheiro não conservado em %d dias: %.2f -> %.2f" % [
			_days, money_initial, money_final])
	# Falências: o cenário sem IA exige zero. Com baseline (F2), o teto é o
	# número medido no baseline — a coorte da IA pode falir firmas que o F2 já
	# falia, mas não pode piorar o total. Sem baseline, o guarda antigo (0).
	var bankruptcy_limit := 0
	if _baseline_path != "":
		var baseline_data: Variant = _load_json(_baseline_path)
		if typeof(baseline_data) == TYPE_DICTIONARY:
			# F9 grava meta.bankruptcies como DICIONÁRIO por variação e
			# meta.bankruptcy_limit; f4 grava como int. Normaliza para o teto
			# de falências de construções (28 no caso da F9), como o sweep.
			var meta: Dictionary = (baseline_data as Dictionary).get("meta", {})
			var baseline_bk: int = 0
			if typeof(meta.get("bankruptcy_limit", 0)) == TYPE_INT:
				baseline_bk = int(meta["bankruptcy_limit"]) - BANKRUPTCY_TOLERANCE
			elif typeof(meta.get("bankruptcies", 0)) == TYPE_INT:
				baseline_bk = int(meta["bankruptcies"])
			elif typeof(meta.get("bankruptcies", 0)) == TYPE_DICTIONARY:
				for key in meta["bankruptcies"]:
					baseline_bk = maxi(baseline_bk, int(meta["bankruptcies"][key]))
			bankruptcy_limit = baseline_bk + BANKRUPTCY_TOLERANCE
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
		# Construções são detalhe pesado: vêm sob demanda pelo id da cidade.
		for building in sim.get_city_status(String(city.get("id", ""))).get(
				"buildings", []):
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
	var nations := _nation_food(sim, status)
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
		var pop: Dictionary = sim.get_city_status(String(city.get("id", ""))).get(
			"pop", {})
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
	_print_labor(sim, status)
	var sectors := _sector_employment(sim, status)
	_print_sector_employment(sectors)
	# F9: participação estatal / privada nacional / estrangeira nas nações
	# âncora (receita e emprego dos ativos), direto do núcleo.
	var ownership := _ownership_summary(sim, OWNERSHIP_TAGS)
	_print_ownership(ownership)
	if geo != null:
		# A IA roda no núcleo: as decisões chegam como comandos aplicados (A3).
		var command_log: Array = sim.get_command_log()
		var by_type: Dictionary = {}
		var builds: Dictionary = {}
		var build_count := 0
		for entry: Dictionary in command_log:
			var type := String(entry.get("type", ""))
			by_type[type] = int(by_type.get(type, 0)) + 1
			if type != "build":
				continue
			var slots: Array = entry.get("s", [])
			var resource := String(slots[2]) if slots.size() > 2 else ""
			builds[resource] = int(builds.get(resource, 0)) + 1
			build_count += 1
		print("[world] IA: %d comandos %s, %d builds %s" % [
			command_log.size(), str(by_type), build_count, str(builds)])

	if _baseline_path != "":
		_compare_baseline(nations)

	if _out != "":
		_write_report(nations, status, has_chains, sectors, ownership)

	if urban_enabled:
		_write_urban_report(urban_ids, urban_start, urban_start_day, urban_end,
			urban_end_day)

	print("[world] resumo final:")
	for city in status["cities"]:
		# POP é detalhe pesado: vem sob demanda pelo id da cidade.
		var pop: Dictionary = sim.get_city_status(String(city["id"])).get("pop", {})
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
## `--fx=float` só troca o REGIME (`peg` → `float`) e mantém as taxas do dado.
## Em todos os caminhos preenche `_fx_meta` para o relatório.
func _load_currencies() -> Dictionary:
	var data := _load_dict(CURRENCIES_PATH)
	if _fx_mode == "float":
		# SONDA (Q-016): currencies.json não traz fonte de regime cambial
		# (`regime: null` nas 136 moedas), então o núcleo marca tudo como `peg`
		# e a taxa só aparece em `price_local`. Forçar `float` no topo e em cada
		# entrada exercita o caminho que hoje está inerte. Nenhuma taxa, cidade
		# ou nação é inventada: só o regime muda.
		var probe: Dictionary = data.duplicate(true)
		probe["regime"] = "float"
		for entry: Variant in Array(probe.get("currencies", [])):
			if typeof(entry) == TYPE_DICTIONARY:
				(entry as Dictionary)["regime"] = "float"
		_fx_meta = _build_fx_meta(probe)
		return probe
	if _fx_mode == "real":
		if _seed != 0:
			_perturb_rates(data)
		_fx_meta = _build_fx_meta(data)
		return data
	var rate := 1.0 if _fx_mode == "neutral" else 0.5
	var neutral := data.duplicate(true)
	for entry: Variant in Array(neutral.get("currencies", [])):
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		(entry as Dictionary)["rate_per_anchor"] = rate
		(entry as Dictionary)["regime"] = "peg"
	neutral["regime"] = "peg"
	_fx_meta = _build_fx_meta(neutral)
	return neutral


## Q-016: resumo do câmbio efetivamente entregue ao núcleo. `floating` conta as
## moedas que o núcleo vai tratar como flutuantes; `rate_sum` soma só as taxas
## numéricas declaradas (moeda sem taxa não vira 1,0 nem 0 — fica fora).
func _build_fx_meta(data: Dictionary) -> Dictionary:
	var entries: Array = Array(data.get("currencies", []))
	var floating := 0
	var rate_sum := 0.0
	for entry: Variant in entries:
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		var currency: Dictionary = entry
		var regime: Variant = currency.get("regime", "")
		if typeof(regime) == TYPE_STRING and String(regime) == "float":
			floating += 1
		var rate: Variant = currency.get("rate_per_anchor", null)
		if typeof(rate) == TYPE_FLOAT or typeof(rate) == TYPE_INT:
			rate_sum += float(rate)
	return {
		"mode": _fx_mode,
		"seed": _seed,
		"enabled": true,
		"currencies": entries.size(),
		"floating": floating,
		"rate_sum": rate_sum,
	}


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
func _print_labor(sim: Object, status: Dictionary) -> void:
	var wages: Array = [[], [], []]
	var unemployment: Array = [[], [], []]
	# POP é detalhe pesado: vem sob demanda pelo id da cidade.
	for city in status.get("cities", []):
		var pop: Dictionary = sim.get_city_status(String(city.get("id", ""))).get(
			"pop", {})
		if not pop.has("wage_by_level"):
			return
		for level in 3:
			wages[level].append(float(pop["wage_by_level"][level]))
			unemployment[level].append(float(pop["unemployment_by_level"][level]))
	var income_cities: Dictionary = {}
	for city in status.get("cities", []):
		var pop2: Dictionary = sim.get_city_status(String(city.get("id", ""))).get(
			"pop", {})
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
		if String(city.get("id", "")) in ["BRA-SAO", "USA-NEW", "IND-DEL", "DEU-BER", "NGA-LAG"]:
			var pop: Dictionary = sim.get_city_status(String(city.get("id", ""))).get(
				"pop", {})
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
			print("[labor]   caixas firma=%.0f servico=%.0f governo=%.0f gdp=%.0f poupanca=%.0f" % [
				float(city.get("firm_cash", 0.0)), float(city.get("service_cash", 0.0)),
				float(city.get("government_budget", 0.0)), float(city.get("gdp", 0.0)),
				float(pop.get("savings", 0.0))])
			print("[labor]   estratos=%s disp_estrato=%s fatias=%s" % [
				str(pop.get("income_level_by_stratum", [])),
				str(pop.get("stratum_disposable", [])),
				str(pop.get("income_level_share", []))])
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
		var shares: Array = labor.get("income_level_share", [])
		if shares.size() >= 4:
			print("[labor] %s fatias basic/life/luxury=%.1f%%/%.1f%%/%.1f%% produtividade=%.3f" % [
				tag, float(shares[1]) * 100.0, float(shares[2]) * 100.0,
				float(shares[3]) * 100.0,
				float(labor.get("productivity_index", 1.0))])


## Agrega comida e industrialização por nação (cons=Σconsumido, need=Σnecessidade).
func _nation_food(sim: Object, status: Dictionary) -> Dictionary:
	var nations: Dictionary = {}
	for city in status.get("cities", []):
		var tag := String(city.get("nation", ""))
		if not nations.has(tag):
			nations[tag] = {
				"cities": 0, "consumed": 0.0, "need": 0.0,
				"worst": 1.0, "worst_city": "", "industrialization": 0.0,
			}
		var row: Dictionary = nations[tag]
		# POP é detalhe pesado: vem sob demanda pelo id da cidade.
		var pop: Dictionary = sim.get_city_status(String(city.get("id", ""))).get(
			"pop", {})
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
	var raw: Variant = (baseline as Dictionary).get("nations", {})
	var rows: Dictionary = {}
	if typeof(raw) == TYPE_DICTIONARY:
		rows = raw
	elif typeof(raw) == TYPE_ARRAY:
		for entry in raw:
			var row: Dictionary = entry as Dictionary
			var tag := String(row.get("tag", ""))
			if tag != "":
				rows[tag] = {"food": float(row.get("worst_food", 1.0))}
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


func _write_report(nations: Dictionary, status: Dictionary, has_chains: bool,
		sectors: Dictionary, ownership: Dictionary) -> void:
	var dir := _out.get_base_dir()
	if dir != "" and not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var file := FileAccess.open(_out, FileAccess.WRITE)
	if file == null:
		_fail("nao abriu para escrita: " + _out)
		return
	var payload := {
		"nations": _strip(nations),
		"labor": _labor_nations(status),
		"sectors": _strip_sectors(sectors),
		"ownership": ownership,
	}
	var report := {
		"meta": {
			"days": _days,
			"chains": has_chains,
			"products": Array(status.get("products", [])).size(),
			"money_total": float(status.get("money_total", 0.0)),
			"bankruptcies": int(status.get("bankruptcies", 0)),
			"service_businesses": int(status.get("service_businesses", 0)),
			"service_bankruptcies": int(status.get("service_bankruptcies", 0)),
			"fx": _fx_meta,
			"trajectory_hash": _trajectory_hash(payload),
		},
		"nations": payload["nations"],
		"labor": payload["labor"],
		"sectors": payload["sectors"],
		"ownership": payload["ownership"],
	}
	file.store_string(JSON.stringify(report, "  "))
	file.close()
	print("[world] fx=%s floating=%d/%d sonda=%s" % [
		String(_fx_meta.get("mode", "off")), int(_fx_meta.get("floating", 0)),
		int(_fx_meta.get("currencies", 0)),
		String(report["meta"]["trajectory_hash"])])
	print("[world] escrito %s" % _out)


## Q-016: hash estável do CONTEÚDO de trajetória (nations+labor+sectors+
## ownership, sem o `meta`). Duas corridas idênticas fecham o mesmo hash; se o
## caminho cambial é inerte, a sonda `--fx=float` fecha o hash das variantes
## `peg` e o sweep fica tautológico — é isso que o portão passa a detectar.
func _trajectory_hash(payload: Dictionary) -> String:
	var context := HashingContext.new()
	if context.start(HashingContext.HASH_SHA256) != OK:
		_fail("trajectory_hash: sha256 indisponivel")
		return ""
	context.update(JSON.stringify(payload).to_utf8_buffer())
	return context.finish().hex_encode()


## Q-015: ids do `--urban-cities` (CSV), sem vazios e sem repetição.
func _urban_ids() -> Array[String]:
	var ids: Array[String] = []
	for part in _urban_cities.split(",", false):
		var id := part.strip_edges()
		if id != "" and id not in ids:
			ids.append(id)
	return ids


## Q-015: retrato urbano de cada id. Dicionário vazio = sem dado (a cidade não
## existe no núcleo): a ausência é reportada, nunca preenchida com número.
func _urban_snapshot(sim: Object, ids: Array[String]) -> Dictionary:
	var out: Dictionary = {}
	if not sim.has_method("get_urban_city"):
		_fail("urbano: binario sem get_urban_city")
		return out
	for id in ids:
		var data: Dictionary = sim.get_urban_city(id)
		if data.is_empty():
			print("[urban] sem dado: " + id)
			continue
		out[id] = data
	return out


## Q-015: JSON do retrato urbano (início × fim) — campos crus do núcleo.
func _write_urban_report(ids: Array[String], start: Dictionary, start_day: int,
		end: Dictionary, end_day: int) -> void:
	var dir := _urban_out.get_base_dir()
	var absolute_dir := dir
	if dir.begins_with("res://") or dir.begins_with("user://"):
		absolute_dir = ProjectSettings.globalize_path(dir)
	if absolute_dir != "" and not DirAccess.dir_exists_absolute(absolute_dir):
		DirAccess.make_dir_recursive_absolute(absolute_dir)
	var file := FileAccess.open(_urban_out, FileAccess.WRITE)
	if file == null:
		_fail("nao abriu para escrita: " + _urban_out)
		return
	var report := {
		"days": _days,
		"threads": _threads,
		"urban": _urban_enabled,
		"cities": ids,
		"start": {"day": start_day, "cities": start},
		"end": {"day": end_day, "cities": end},
	}
	file.store_string(JSON.stringify(report, "  "))
	file.close()
	print("[world] urbano escrito %s" % _urban_out)
	for id: String in ids:
		if not end.has(id):
			continue
		var row: Dictionary = end[id]
		var gov: Dictionary = row.get("gov", {})
		var area_total := 0.0
		for value: Variant in Array(row.get("area_km2", [])):
			area_total += float(value)
		print("[urban] %-8s cash=%.0f rent_burden=%.3f informal=%.0f km2=%.2f" % [
			id, float(gov.get("cash", 0.0)), float(row.get("rent_burden", 0.0)),
			float(row.get("informal_units", 0.0)), area_total])


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
			"income_level_share": labor.get("income_level_share", []),
			"productivity_index": float(labor.get("productivity_index", 1.0)),
		}
	return out


## F9: estrutura do emprego por nação. `sector_workers` (status) traz o emprego
## decomposto em agropecuária (0), indústria/mineração (1) e serviços residuais
## (2); a informalidade (3) é SOBREPOSIÇÃO — parte dos três primeiros, não um
## quarto setor — e por isso não soma com eles. A parcela é
## `sector_workers[i] / pop.employed`; `in_kind_food` é o autoconsumo em espécie
## do dia (t-eq), a comida que não passou por mercado.
func _sector_employment(sim: Object, status: Dictionary) -> Dictionary:
	var rows: Dictionary = {}
	for city in status.get("cities", []):
		var workers: Array = city.get("sector_workers", [])
		if workers.size() != 4:
			continue
		var tag := String(city.get("nation", ""))
		# POP é detalhe pesado: vem sob demanda pelo id da cidade.
		var pop: Dictionary = sim.get_city_status(String(city.get("id", ""))).get(
			"pop", {})
		var row: Dictionary = rows.get(tag, {
			"cities": 0, "agri": 0.0, "industry": 0.0, "services": 0.0,
			"informal": 0.0, "employed": 0.0, "labor_force": 0.0, "in_kind": 0.0,
		})
		row["cities"] = int(row["cities"]) + 1
		row["agri"] = float(row["agri"]) + float(workers[0])
		row["industry"] = float(row["industry"]) + float(workers[1])
		row["services"] = float(row["services"]) + float(workers[2])
		row["informal"] = float(row["informal"]) + float(workers[3])
		row["employed"] = float(row["employed"]) + float(pop.get("employed", 0.0))
		row["labor_force"] = float(row["labor_force"]) + float(
			pop.get("labor_force", 0.0))
		row["in_kind"] = float(row["in_kind"]) + float(
			city.get("in_kind_food", 0.0))
		rows[tag] = row
	for tag in rows:
		var row: Dictionary = rows[tag]
		var employed := float(row["employed"])
		for key in ["agri", "industry", "services", "informal"]:
			row["share_" + key] = float(row[key]) / employed if employed > 0.0 else 0.0
	return rows


## Referência OIT de uma nação-âncora: só existe para as nações do
## `sector_employment.json`. Serviços não é âncora observada — é o resíduo
## `1 - agricultura - indústria` da MESMA fonte, e é assim que aparece aqui.
func _sector_reference(tag: String) -> Dictionary:
	var anchors: Array = _sector_data.get("anchors", [])
	for entry: Variant in anchors:
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		var anchor: Dictionary = entry
		if String(anchor.get("tag", "")) != tag:
			continue
		var agri := float(anchor.get("agriculture", 0.0))
		var industry := float(anchor.get("industry", 0.0))
		return {
			"agri": agri,
			"industry": industry,
			"services": maxf(0.0, 1.0 - agri - industry),
			"informal": float(anchor.get("informal", 0.0)),
			"income_index": float(anchor.get("income_index", 0.0)),
		}
	return {}


func _sector_anchor_tags() -> Array:
	var tags: Array = []
	for entry: Variant in _sector_data.get("anchors", []):
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		var tag := String((entry as Dictionary).get("tag", ""))
		if tag != "" and tag not in tags:
			tags.append(tag)
	return tags


func _print_sector_employment(sectors: Dictionary) -> void:
	if sectors.is_empty():
		print("[world] emprego setorial: sem dado (binario anterior a F9)")
		return
	var total := {
		"agri": 0.0, "industry": 0.0, "services": 0.0, "informal": 0.0,
		"employed": 0.0, "in_kind": 0.0,
	}
	for tag in sectors:
		var row: Dictionary = sectors[tag]
		for key in total:
			total[key] = float(total[key]) + float(row[key])
	var employed := float(total["employed"])
	print("[world] emprego setorial (sector_workers/empregados): %d nacoes | agro %.1f%% industria %.1f%% servicos %.1f%% informal %.1f%% (sobreposicao) | autoconsumo %.0f t-eq/dia" % [
		sectors.size(), _share(total["agri"], employed) * 100.0,
		_share(total["industry"], employed) * 100.0,
		_share(total["services"], employed) * 100.0,
		_share(total["informal"], employed) * 100.0,
		float(total["in_kind"])])
	for tag in _sector_anchor_tags():
		if not sectors.has(tag):
			continue
		var row: Dictionary = sectors[tag]
		var reference := _sector_reference(tag)
		print("  %-4s cidades=%3d empregados=%9.0f agro=%5.1f%% (ref %5.1f) industria=%5.1f%% (ref %5.1f) servicos=%5.1f%% (ref %5.1f) informal=%5.1f%% (ref %5.1f) autoconsumo=%7.1f t-eq" % [
			String(tag), int(row["cities"]), float(row["employed"]),
			float(row["share_agri"]) * 100.0, float(reference.get("agri", 0.0)) * 100.0,
			float(row["share_industry"]) * 100.0,
			float(reference.get("industry", 0.0)) * 100.0,
			float(row["share_services"]) * 100.0,
			float(reference.get("services", 0.0)) * 100.0,
			float(row["share_informal"]) * 100.0,
			float(reference.get("informal", 0.0)) * 100.0,
			float(row["in_kind"])])


func _share(value: Variant, total: float) -> float:
	return float(value) / total if total > 0.0 else 0.0


## F9 — participação por tipo de dono. O núcleo agrega, por nação do ativo,
## receita e emprego em três fatias: [estatal, privado nacional, estrangeiro].
## `privado nacional` inclui os grupos nacionais e o privado agregado legado
## (dono sem grupo) — o núcleo não distingue os dois no status.
func _ownership_summary(sim: Object, tags: Array) -> Dictionary:
	if not sim.has_method("get_investment_status"):
		_fail("propriedade: binário sem get_investment_status")
		return {}
	var investment: Dictionary = sim.get_investment_status()
	var nations: Dictionary = investment.get("nations", {})
	var out: Dictionary = {}
	for tag: String in tags:
		if not nations.has(tag):
			_fail("propriedade: %s não aparece no status de investimento" % tag)
			continue
		var row: Dictionary = nations[tag]
		var production: Array = row.get("production_share", [])
		var employment: Array = row.get("employment_share", [])
		var assets: Array = row.get("assets", [])
		if production.size() != 3 or employment.size() != 3 or assets.size() != 3:
			_fail("propriedade: %s com fatias incompletas" % tag)
			continue
		var production_total := float(row.get("production_total", 0.0))
		var employment_total := float(row.get("employment_total", 0.0))
		var assets_total := int(row.get("assets_total", 0))
		if not is_finite(production_total) or not is_finite(employment_total):
			_fail("propriedade: %s com total não finito" % tag)
			continue
		if assets_total <= 0:
			_fail("propriedade: %s sem ativos no status" % tag)
		for slot in 3:
			var value := float(production[slot])
			if not is_finite(value) or value < 0.0 or value > 1.000001:
				_fail("propriedade: %s produção fora de [0,1] no slot %d (%.6f)" % [
					tag, slot, value])
			var jobs := float(employment[slot])
			if not is_finite(jobs) or jobs < 0.0 or jobs > 1.000001:
				_fail("propriedade: %s emprego fora de [0,1] no slot %d (%.6f)" % [
					tag, slot, jobs])
		# As três fatias de cada métrica fecham em 100% (tolerância de arredondamento)
		# quando há base; sem base (total 0) todas são 0 por definição do núcleo.
		if production_total > 0.0:
			var production_sum := float(production[0]) + float(production[1]) \
				+ float(production[2])
			if absf(production_sum - 1.0) > 1e-6:
				_fail("propriedade: %s produção soma %.6f (esperado 1)" % [
					tag, production_sum])
		if employment_total > 0.0:
			var employment_sum := float(employment[0]) + float(employment[1]) \
				+ float(employment[2])
			if absf(employment_sum - 1.0) > 1e-6:
				_fail("propriedade: %s emprego soma %.6f (esperado 1)" % [
					tag, employment_sum])
		out[tag] = {
			"production_total": production_total,
			"employment_total": employment_total,
			"assets_total": assets_total,
			"confidence": float(row.get("confidence", 1.0)),
			"production_share": [
				float(production[0]), float(production[1]), float(production[2])],
			"employment_share": [
				float(employment[0]), float(employment[1]), float(employment[2])],
			"assets": [int(assets[0]), int(assets[1]), int(assets[2])],
		}
	return out


func _print_ownership(ownership: Dictionary) -> void:
	if ownership.is_empty():
		return
	print("[world] propriedade (estatal / privado nacional / estrangeiro):")
	for tag: String in OWNERSHIP_TAGS:
		if not ownership.has(tag):
			continue
		var row: Dictionary = ownership[tag]
		var production: Array = row["production_share"]
		var employment: Array = row["employment_share"]
		var assets: Array = row["assets"]
		print("  %-4s receita=%12.0f empregos=%9.0f confianca=%.2f | producao est=%5.1f%% nac=%5.1f%% estr=%5.1f%% | emprego est=%5.1f%% nac=%5.1f%% estr=%5.1f%% | ativos %d/%d/%d" % [
			tag, float(row["production_total"]), float(row["employment_total"]),
			float(row["confidence"]),
			float(production[0]) * 100.0, float(production[1]) * 100.0,
			float(production[2]) * 100.0,
			float(employment[0]) * 100.0, float(employment[1]) * 100.0,
			float(employment[2]) * 100.0,
			int(assets[0]), int(assets[1]), int(assets[2])])


func _strip_sectors(sectors: Dictionary) -> Dictionary:
	var out: Dictionary = {}
	for tag in sectors:
		var row: Dictionary = sectors[tag]
		out[tag] = {
			"cities": int(row["cities"]),
			"agri": float(row["agri"]),
			"industry": float(row["industry"]),
			"services": float(row["services"]),
			"informal": float(row["informal"]),
			"employed": float(row["employed"]),
			"labor_force": float(row["labor_force"]),
			"in_kind": float(row["in_kind"]),
			"share_agri": float(row["share_agri"]),
			"share_industry": float(row["share_industry"]),
			"share_services": float(row["share_services"]),
			"share_informal": float(row["share_informal"]),
			"reference": _sector_reference(String(tag)),
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
