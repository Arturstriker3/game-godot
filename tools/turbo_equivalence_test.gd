extends SceneTree

## Equivalência do modo TURBO (`core/economy/turbo_config.gd` +
## `core/economy/tick_scheduler.gd`) contra o relógio normal do jogo.
##
## Quatro mundos IDÊNTICOS (mesmo cenário, mesma IA, mesma camada urbana, mesma
## geografia aplicada por sim), todos com 60 dias:
##
##   A) normal: 60 × `sim.tick_day()` na main thread.
##   B) turbo: `TickScheduler` com `days_per_task = 7`. Tem de publicar 9 vezes
##      (ceil(60/7) = 9), não 60: a publicação acontece por LOTE, não por dia.
##      O último lote roda só os dias que faltam para fechar exatamente 60.
##   C) turbo com `days_per_task = 1` e um `price_shock("wheat", 1.1)` logo
##      depois do 30º dia concluído.
##   D) normal com o MESMO `price_shock` no mesmo ponto.
##
## Comparações (falha -> `[turbo_equiv] FALHA: ...` + exit 1):
##   A vs B: SHA-256 de `JSON.stringify(to_dict(), "  ", true, true)` e de
##           `get_status()` iguais.
##   C vs D: os mesmos dois shas E o SHA do `get_command_log()`.
##   Em C: o `price_shock` está no log com `apply_day == 30` e nenhum dos dois
##          caminhos deixa comando pendente no fim.
##   Sanidade: dia == 60 nos quatro e `money_total` de A igual ao de B.
##
## Antes dos mundos roda o teste UNITÁRIO da config, que não depende do
## processo: `TurboConfigScript.new([...])` recebe os args por PARÂMETRO.
##
## Uso: godot --headless --path . --script res://tools/turbo_equivalence_test.gd
## Saída: 0 ok, 1 falha. Não abre janela, não toca no editor.

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const GroupNamerScript := preload("res://core/economy/group_namer.gd")
const TickSchedulerScript := preload("res://core/economy/tick_scheduler.gd")
const TurboConfigScript := preload("res://core/economy/turbo_config.gd")

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

## Dias simulados em cada mundo — todos os quatro chegam ao mesmo dia.
const DAYS := 60
## Dias por tarefa do lote do turbo (o jogo normal usa 1).
const BATCH := 7
## Dia do choque de preço nos pares C/D.
const COMMAND_DAY := 30
## Publicações esperadas em B: um lote por tarefa (o último menor).
const BATCH_PUBLISHES := 9
## Teto de polls do laço turbo: 60 dias não deveriam passar disso nem de longe.
const MAX_POLLS := 2_000_000

var _failures: Array[String] = []


func _init() -> void:
	_test_turbo_config()

	var world := _load_world()
	if _failures.is_empty():
		_run_worlds(world)

	if _failures.is_empty():
		print("[turbo_equiv] OK: 4 mundos de %d dias, turbo == normal." % DAYS)
		quit(0)
	else:
		for failure in _failures:
			print("[turbo_equiv] FALHA: " + failure)
		quit(1)


func _fail(msg: String) -> void:
	_failures.append(msg)


# --- teste unitário da config ------------------------------------------------

## `TurboConfigScript` recebe os args por parâmetro: nada aqui depende da linha
## de comando do processo. Só o AMBIENTE é lido do `OS` (por isso o par dele).
func _test_turbo_config() -> void:
	var plain = TurboConfigScript.new([])
	_expect(plain.enabled == false, "config vazia: enabled deveria ser false")
	_expect(plain.days_per_frame == 4, "config vazia: days_per_frame deveria ser 4, veio %d" % plain.days_per_frame)

	var turbo = TurboConfigScript.new(["--turbo"])
	_expect(turbo.enabled == true, "--turbo nao ligou o modo")
	_expect(turbo.days_per_frame == 4, "--turbo deveria manter o lote padrao 4, veio %d" % turbo.days_per_frame)

	var batch10 = TurboConfigScript.new(["--turbo-days-per-frame=10"])
	_expect(batch10.enabled == true, "--turbo-days-per-frame nao ligou o modo")
	_expect(batch10.days_per_frame == 10, "days_per_frame deveria ser 10, veio %d" % batch10.days_per_frame)
	_expect(batch10.render_every == 10, "render_every deveria seguir o lote (10), veio %d" % batch10.render_every)

	var split = TurboConfigScript.new(["--turbo-days-per-frame=5", "--turbo-render-every=3"])
	_expect(split.render_every == 5,
		"render_every nao pode ser menor que o lote: esperado 5, veio %d" % split.render_every)

	var bench = TurboConfigScript.new(["--bench-days=50", "--bench-speed=2"])
	_expect(bench.enabled == false, "--bench-days nao deveria ligar o turbo")
	_expect(bench.bench_days == 50, "bench_days deveria ser 50, veio %d" % bench.bench_days)
	_expect(bench.bench_speed == 2, "bench_speed deveria ser 2, veio %d" % bench.bench_speed)

	OS.set_environment("OGS_TURBO", "1")
	var env_on = TurboConfigScript.new([])
	_expect(env_on.enabled == true, "OGS_TURBO=1 nao ligou o modo")
	OS.set_environment("OGS_TURBO", "")
	var env_off = TurboConfigScript.new([])
	_expect(env_off.enabled == false, "OGS_TURBO vazio manteve o modo ligado")

	_expect(plain.describe() != "", "describe() saiu vazio")
	print("[turbo_equiv] config OK (unitario)")


func _expect(condition: bool, message: String) -> void:
	if not condition:
		_fail("config: " + message)


# --- mundo -------------------------------------------------------------------

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


## Cidades no formato que o `GeographyService.apply_to_sim` espera.
func _cities_with_center(cities: Array) -> Array:
	var result: Array = []
	for city in cities:
		result.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
		})
	return result


## Carrega as definições e a geografia UMA vez: os quatro sims reutilizam tudo,
## e `apply_to_sim` é chamado por sim (um mesmo `geo` serve aos quatro).
func _load_world() -> Dictionary:
	var goods := _load_array(GOODS_PATH)
	var recipes := _load_array(RECIPES_PATH)
	var needs := _load_dict(NEEDS_PATH)
	var services := _load_dict(SERVICES_PATH)
	var currencies := _load_dict(CURRENCIES_PATH)
	var sector := _load_dict(SECTOR_EMPLOYMENT_PATH)
	var urban_land := _load_dict(URBAN_LAND_PATH)
	var housing_city := _load_dict(HOUSING_CITY_PATH)
	var housing := _load_dict(HOUSING_PATH)
	var municipal := _load_dict(MUNICIPAL_FINANCE_PATH)
	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	if cities.is_empty() or buildings.is_empty():
		_fail("cenario vazio (cidades=%d, construcoes=%d)" % [
			cities.size(), buildings.size()])
		return {}

	var geo: Object = GeographyServiceScript.new()
	geo.load_data()
	buildings = geo.resolve_initial_farms(buildings, cities)

	return {
		"goods": goods,
		"recipes": recipes,
		"needs": needs,
		"services": services,
		"currencies": currencies,
		"sector": sector,
		"urban_land": urban_land,
		"housing_city": housing_city,
		"housing": housing,
		"municipal": municipal,
		"cities": cities,
		"ports": ports,
		"buildings": buildings,
		"centers": _cities_with_center(cities),
		"geo": geo,
	}


## Um sim novo e idêntico aos outros: mesma ordem de carga do jogo ao vivo.
func _build_sim(world: Dictionary) -> Object:
	var sim = EconomySim.new()
	sim.set_goods(world["goods"])
	sim.set_recipes(world["recipes"])
	sim.set_needs(world["needs"])
	if sim.has_method("set_services"):
		sim.set_services(world["services"])
	if sim.has_method("set_currencies"):
		sim.set_currencies(world["currencies"])
	if sim.has_method("set_sector_employment") and not world["sector"].is_empty():
		sim.set_sector_employment(world["sector"])
	if not _failures.is_empty():
		return null

	sim.initialize_world(world["cities"], world["ports"], world["buildings"])

	if not sim.has_method("set_urban"):
		_fail("binario sem set_urban (rode o build)")
		return null
	sim.set_urban(world["urban_land"], world["housing_city"], world["housing"],
		world["municipal"])

	world["geo"].apply_to_sim(sim, world["centers"])
	sim.set_ai_enabled(true)
	GroupNamerScript.name_groups(sim)

	if sim.has_method("warm_status"):
		sim.warm_status()
	return sim


# --- os quatro mundos --------------------------------------------------------

func _run_worlds(world: Dictionary) -> void:
	var sim_a = _build_sim(world)
	var sim_b = _build_sim(world)
	var sim_c = _build_sim(world)
	var sim_d = _build_sim(world)
	if not _failures.is_empty():
		return

	var t0 := Time.get_ticks_usec()
	for _day in DAYS:
		sim_a.tick_day()
	var normal_ms := float(Time.get_ticks_usec() - t0) / 1000.0

	var turbo := _run_turbo(sim_b, BATCH, 0)
	var turbo_ms := float(turbo["ms"])

	var turbo_c := _run_turbo(sim_c, 1, COMMAND_DAY)
	var turbo_c_ms := float(turbo_c["ms"])

	var normal_d := _run_normal(sim_d, COMMAND_DAY)
	var normal_d_ms := float(normal_d["ms"])

	if not _failures.is_empty():
		return

	_compare("A(normal) vs B(turbo lote)", sim_a, sim_b, false)
	_compare("C(turbo 1/dia + choque) vs D(normal + choque)", sim_c, sim_d, true)

	# B publica por lote: 9 resumos para 60 dias (ceil(60/7)).
	var publishes := int(turbo["publishes"])
	if publishes != BATCH_PUBLISHES:
		_fail("B publicou %d resumos; esperado %d (ceil(%d/%d))" % [
			publishes, BATCH_PUBLISHES, DAYS, BATCH])

	# C: o choque aplicado no dia 30 está no log com `apply_day == 30`.
	var c_log: Array = sim_c.get_command_log()
	if not _has_price_shock(c_log, "wheat", COMMAND_DAY):
		_fail("C: price_shock('wheat') sem entrada com apply_day == %d no log de comandos" % COMMAND_DAY)

	for entry in [["C", sim_c], ["D", sim_d]]:
		if int(entry[1].pending_command_count()) != 0:
			_fail("%s terminou com %d comando(s) pendente(s)" % [
				entry[0], int(entry[1].pending_command_count())])

	_check_day(sim_a, "A")
	_check_day(sim_b, "B")
	_check_day(sim_c, "C")
	_check_day(sim_d, "D")

	var money_a := float(sim_a.get_status().get("money_total", 0.0))
	var money_b := float(sim_b.get_status().get("money_total", 0.0))
	if money_a != money_b:
		_fail("money_total A != B: %.17g != %.17g" % [money_a, money_b])

	print("[turbo_equiv] normal %d d em %.0f ms; turbo lote N=%d em %.0f ms" % [
		DAYS, normal_ms, BATCH, turbo_ms])
	print("[turbo_equiv] turbo 1/dia (C) em %.0f ms; normal (D) em %.0f ms; publicacoes B=%d" % [
		turbo_c_ms, normal_d_ms, publishes])


## 60 dias no caminho normal, com um choque aplicado depois do dia `shock_day`.
func _run_normal(sim, shock_day: int) -> Dictionary:
	var shocked := shock_day <= 0
	var t0 := Time.get_ticks_usec()
	for _day in DAYS:
		sim.tick_day()
		if not shocked and int(sim.get_status().get("day", 0)) >= shock_day:
			sim.price_shock("wheat", 1.1)
			shocked = true
	return {
		"ms": float(Time.get_ticks_usec() - t0) / 1000.0,
		"publishes": DAYS,
	}


## 60 dias pelo agendador, `batch` dias por tarefa. Cada tarefa devolve UM
## resumo publicado (o último lote roda só o que falta para fechar 60).
func _run_turbo(sim, batch: int, shock_day: int) -> Dictionary:
	var scheduler = TickSchedulerScript.new()
	var publishes := 0
	var day := 0
	var polls := 0
	var shocked := shock_day <= 0
	var t0 := Time.get_ticks_usec()
	while day < DAYS:
		scheduler.days_per_task = mini(batch, DAYS - day)
		if not scheduler.start(sim):
			_fail("start() recusou sem tarefa em andamento")
			break
		var finished := false
		while polls < MAX_POLLS:
			polls += 1
			var summary: Dictionary = scheduler.poll()
			if not summary.is_empty():
				publishes += 1
				day = int(summary.get("day", day))
				finished = true
				break
			OS.delay_usec(200)
		if not finished:
			_fail("tick do turbo nao terminou em %d polls" % MAX_POLLS)
			break
		if not shocked and day >= shock_day:
			sim.price_shock("wheat", 1.1)
			shocked = true
	return {
		"ms": float(Time.get_ticks_usec() - t0) / 1000.0,
		"publishes": publishes,
	}


# --- comparações -------------------------------------------------------------

func _compare(label: String, sim_a, sim_b, with_log: bool) -> void:
	var save_a := JSON.stringify(sim_a.to_dict(), "  ", true, true)
	var save_b := JSON.stringify(sim_b.to_dict(), "  ", true, true)
	var status_a := JSON.stringify(sim_a.get_status(), "  ", true, true)
	var status_b := JSON.stringify(sim_b.get_status(), "  ", true, true)

	var save_sha_a := save_a.sha256_text()
	var save_sha_b := save_b.sha256_text()
	var status_sha_a := status_a.sha256_text()
	var status_sha_b := status_b.sha256_text()

	if save_sha_a != save_sha_b:
		_fail("%s: save diferente (%s != %s)" % [
			label, save_sha_a.substr(0, 12), save_sha_b.substr(0, 12)])
	if status_sha_a != status_sha_b:
		_fail("%s: status diferente (%s != %s)" % [
			label, status_sha_a.substr(0, 12), status_sha_b.substr(0, 12)])

	var log_sha_a := "-"
	if with_log:
		var log_a := JSON.stringify(sim_a.get_command_log(), "  ", true, true)
		var log_b := JSON.stringify(sim_b.get_command_log(), "  ", true, true)
		log_sha_a = log_a.sha256_text()
		if log_sha_a != log_b.sha256_text():
			_fail("%s: log de comandos diferente (%s != %s)" % [
				label, log_sha_a.substr(0, 12),
				log_b.sha256_text().substr(0, 12)])

	print("[turbo_equiv] %s | save=%s status=%s log=%s" % [
		label, save_sha_a.substr(0, 12), status_sha_a.substr(0, 12),
		log_sha_a.substr(0, 12)])


func _check_day(sim, label: String) -> void:
	var day := int(sim.get_status().get("day", -1))
	if day != DAYS:
		_fail("%s terminou no dia %d; esperado %d" % [label, day, DAYS])


## Existe no log uma entrada do `price_shock` sobre `product` aplicada no dia?
func _has_price_shock(entries: Array, product: String, apply_day: int) -> bool:
	for entry in entries:
		if String(entry.get("type", "")) != "price_shock":
			continue
		var strings: Array = entry.get("s", [])
		if strings.is_empty() or String(strings[0]) != product:
			continue
		if int(entry.get("apply_day", -1)) == apply_day:
			return true
	return false
