extends SceneTree

## A3 - replay da fila de comandos (docs/economy/refactor/A3_commands.md).
##
## Contrato: mesmo snapshot + MESMA lista de comandos = MESMO mundo. O teste
##  1. cria o cenário de validação e guarda o snapshot do dia 0;
##  2. roda alguns dias emitindo comandos (choque de preço, estoque, tarifa,
##     investimento) entre os dias - cada um entra na fila como Command;
##  3. serializa o log em JSON (o mesmo formato que o jogo pode gravar) e lê de
##     volta;
##  4. carrega o snapshot do dia 0 num segundo núcleo e reproduz o log com
##     `replay_commands()`;
##  5. compara os dois snapshots campo a campo: TEM de ser 0 diferenças.
## Também confere que o log cresce com os comandos, que a fila fica vazia e que
## a ordem de emissão (seq) é estritamente crescente.
##
## Uso: godot --headless --path . --script res://tools/command_replay_test.gd

const DAYS := 6

var _failures: Array[String] = []
var _checks := 0


func _init() -> void:
	var base = EconomySim.new()
	base.initialize_scenario()
	# Um dia antes do snapshot: é o tick que semeia a indústria de referência
	# (seed_industry), então o mundo do dia 1 já tem construção para investir.
	base.tick_day()
	var start: Dictionary = base.to_snapshot()
	var start_day := _day_of(base)

	var sim = EconomySim.new()
	sim.from_snapshot(start)
	var building_id := _first_building_id(sim)
	var city_id := _first_city_id(sim)
	_check(building_id != "" and city_id != "", "cenário tem cidade/construção")

	var issued := 0
	for offset in range(DAYS):
		var day := start_day + offset
		match offset % 3:
			0:
				sim.price_shock("wheat", 1.1)
				issued += 1
			1:
				sim.set_market_stock(city_id, "wheat", 1234.0 + float(offset))
				sim.set_market_price(city_id, "wheat", 42.0 + float(offset))
				issued += 2
			2:
				var invested: Dictionary = sim.invest(city_id, building_id, 50.0)
				# Fora do dia o efeito é imediato (contrato de retorno do
				# método): nada de "queued" para quem chama entre ticks.
				_check(not invested.has("queued"),
					"invest entre dias aplica na hora (dia %d)" % day)
				issued += 1
		sim.set_tariff(city_id, city_id, "*", 0.0)
		issued += 1
		sim.tick_day()

	var target_day := _day_of(sim)
	_check(sim.pending_command_count() == 0, "fila vazia depois dos ticks")
	var log: Array = sim.get_command_log()
	_check(log.size() == issued,
		"log tem um registro por comando (%d/%d)" % [log.size(), issued])
	var reference: Dictionary = sim.to_snapshot()

	var last_seq := 0
	var ordered := true
	for entry in log:
		var seq := int(entry.get("seq", 0))
		ordered = ordered and seq > last_seq
		last_seq = seq
	_check(ordered, "ordem de emissão (seq) estritamente crescente")
	_check(_log_has_types(log), "log cobre os tipos emitidos")

	# JSON: o log é serializável e volta com o mesmo tamanho/estrutura.
	var text := JSON.stringify(log)
	var parsed = JSON.parse_string(text)
	_check(parsed is Array and parsed.size() == log.size(),
		"log sobrevive ao JSON (stringify + parse)")

	# Replay a partir do MESMO snapshot inicial.
	var replay = EconomySim.new()
	replay.from_snapshot(start)
	_check(replay.replay_commands(parsed, target_day), "replay_commands aceitou o log")
	_check(_day_of(replay) == target_day,
		"replay termina no dia %d (obtido %d)" % [target_day, _day_of(replay)])
	var replayed: Dictionary = replay.to_snapshot()
	var diffs: Array[String] = []
	_diff("snapshot", reference, replayed, diffs)
	if not diffs.is_empty():
		for diff in diffs.slice(0, 5):
			print("[replay] divergência: " + diff)
	_check(diffs.is_empty(),
		"replay reproduz o mundo campo a campo (%d diferenças)" % diffs.size())

	# Determinismo do replay: duas execuções do mesmo log no mesmo snapshot.
	var again = EconomySim.new()
	again.from_snapshot(start)
	again.replay_commands(parsed, target_day)
	var diffs_again: Array[String] = []
	_diff("snapshot", replayed, again.to_snapshot(), diffs_again)
	_check(diffs_again.is_empty(), "replay é determinístico entre execuções")

	if _failures.is_empty():
		print("[replay] OK: %d checagens, %d comandos, %d dias, 0 diferenças."
			% [_checks, log.size(), target_day - start_day])
		quit(0)
	else:
		for failure in _failures:
			print("[replay] FALHA: " + failure)
		quit(1)


func _day_of(sim) -> int:
	return int(sim.get_status().get("day", 0))


func _first_city_id(sim) -> String:
	var cities: Array = sim.get_status().get("cities", [])
	if cities.is_empty():
		return ""
	return String(cities[0].get("id", ""))


func _first_building_id(sim) -> String:
	var cities: Array = sim.get_status().get("cities", [])
	for city in cities:
		var id := String(city.get("id", ""))
		if id == "":
			continue
		# A lista de construções só existe no detalhe sob demanda.
		var detail: Dictionary = sim.get_city_status(id)
		for building in detail.get("buildings", []):
			var building_id := String(building.get("id", ""))
			if building_id != "":
				return building_id
	return ""


func _log_has_types(log: Array) -> bool:
	var wanted := {
		"invest": false,
		"price_shock": false,
		"set_market_stock": false,
		"set_market_price": false,
		"set_tariff": false,
	}
	for entry in log:
		var type := String(entry.get("type", ""))
		if wanted.has(type):
			wanted[type] = true
	for type in wanted.keys():
		if not bool(wanted[type]):
			return false
	return true


func _diff(path: String, expected, actual, out: Array[String]) -> void:
	if typeof(expected) != typeof(actual):
		out.append("%s: tipo %d != %d" % [path, typeof(expected), typeof(actual)])
		return
	if expected is Dictionary:
		var keys: Array = expected.keys()
		for key in actual.keys():
			if not expected.has(key):
				keys.append(key)
		for key in keys:
			_diff("%s.%s" % [path, String(key)], expected.get(key), actual.get(key), out)
		return
	if expected is Array:
		if expected.size() != actual.size():
			out.append("%s: tamanho %d != %d" % [path, expected.size(), actual.size()])
			return
		for index in range(expected.size()):
			_diff("%s[%d]" % [path, index], expected[index], actual[index], out)
		return
	if expected is float or expected is int:
		if float(expected) != float(actual):
			out.append("%s: %s != %s" % [path, str(expected), str(actual)])
		return
	if expected != actual:
		out.append("%s: %s != %s" % [path, str(expected), str(actual)])


func _check(condition: bool, label: String) -> void:
	_checks += 1
	if not condition:
		_failures.append(label)
