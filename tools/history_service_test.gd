extends SceneTree

## Histórico de 12 meses (item 9): acúmulo por dia, anel de 30 dias, um ponto
## por mês (12 meses) com downsampling para o último dia do mês, séries por
## nação/cidade, idempotência do dia e round-trip de serialize/deserialize.
##
##   godot --headless --path . --script res://tools/history_service_test.gd

const SERVICE := preload("res://core/stats/history_service.gd")
const DAYS := 400
const EPS := 0.5

var _failures: Array[String] = []


func _init() -> void:
	_run()


func _run() -> void:
	print("=== history_service_test ===")
	var service = SERVICE.new()
	for day in DAYS:
		service.sample(day, _summary(day), _month_key(day))

	_check_daily(service)
	_check_monthly(service)
	_check_serialization(service)
	_check_idempotent(service)
	_check_empty()

	if _failures.is_empty():
		print("PASS: historico acumula 30 dias, faz downsampling para 12 meses e serializa")
		quit(0)
		return
	for failure in _failures:
		push_error("[history_service] " + failure)
	quit(1)


# --- Checagens --------------------------------------------------------------

func _check_daily(service) -> void:
	if service.nation_daily_count("AAA") != 30:
		_failures.append("nação AAA com %d dias (esperado 30)"
				% service.nation_daily_count("AAA"))
	if service.city_daily_count("AAA-1") != 30:
		_failures.append("cidade AAA-1 com %d dias (esperado 30)"
				% service.city_daily_count("AAA-1"))
	var days: PackedInt32Array = service.days_for_city("AAA-1")
	if days.size() != 30 or days[0] != DAYS - 30 or days[29] != DAYS - 1:
		_failures.append("janela diária errada: %s" % str(days))
	var population: PackedFloat32Array = service.nation_daily("AAA", "population")
	_expect(population.size() == 30, "série diária de população com %d pontos"
			% population.size())
	if population.size() == 30:
		_expect(close(population[29], float(DAYS) * 2000.0),
				"população do último dia = %.0f (esperado %.0f)"
				% [population[29], float(DAYS) * 2000.0])
		_expect(close(population[0], float(DAYS - 29) * 2000.0),
				"primeiro ponto do anel fora da janela: %.0f" % population[0])
	var food: PackedFloat32Array = service.city_daily("AAA-1", "food")
	if food.size() == 30:
		var expected: float = (50.0 + DAYS - 1) / (200.0 + DAYS - 1)
		_expect(close(food[29], expected), "comida da cidade = %.4f (esperado %.4f)"
				% [food[29], expected])
	var reserves: PackedFloat32Array = service.nation_daily("AAA", "reserves")
	if reserves.size() == 30:
		var expected_reserve := (1000.0 - (DAYS - 1)) * 2.0
		_expect(close(reserves[29], expected_reserve),
				"reservas da nação = %.1f (esperado %.1f)"
				% [reserves[29], expected_reserve])
	print("diário: %d pontos, %d..%d" % [days.size(), days[0], days[days.size() - 1]])


func _check_monthly(service) -> void:
	var labels: PackedStringArray = service.month_labels()
	if service.month_count() != 12 or labels.size() != 12:
		_failures.append("meses mantidos = %d (esperado 12)" % labels.size())
		return
	print("mensal: %s .. %s" % [labels[0], labels[11]])
	var population: PackedFloat32Array = service.nation_monthly("AAA", "population")
	if population.size() != 12:
		_failures.append("série mensal de população com %d pontos"
				% population.size())
		return
	for index in 12:
		var last_day := _last_day_of(labels[index])
		var expected: float = float(last_day + 1) * 2000.0
		_expect(close(population[index], expected),
				"mês %s população %.0f (esperado %.0f, último dia %d)"
				% [labels[index], population[index], expected, last_day])
	# O último mês continua sendo o mês do último dia amostrado.
	if labels[11] != _month_key(DAYS - 1):
		_failures.append("último rótulo mensal %s != %s"
				% [labels[11], _month_key(DAYS - 1)])
	# Cidade e nação têm a mesma grade de meses.
	var income: PackedFloat32Array = service.city_monthly("AAA-1", "income")
	if income.size() != 12 or not close(income[0], 0.5):
		_failures.append("renda mensal da cidade inválida: %s" % str(income))


func _check_serialization(service) -> void:
	var data: Dictionary = service.serialize()
	var json := JSON.stringify(data)
	var parsed: Variant = JSON.parse_string(json)
	if typeof(parsed) != TYPE_DICTIONARY:
		_failures.append("serialize não é JSON válido")
		return
	var restored = SERVICE.new()
	if not restored.deserialize(parsed):
		_failures.append("deserialize recusou o próprio save")
		return
	if restored.month_count() != service.month_count():
		_failures.append("meses perdidos no round-trip: %d != %d"
				% [restored.month_count(), service.month_count()])
	_compare(service.nation_daily("AAA", "gdp"), restored.nation_daily("AAA", "gdp"),
			"gdp diário")
	_compare(service.nation_monthly("BBB", "food"),
			restored.nation_monthly("BBB", "food"), "comida mensal")
	_compare(service.city_daily("AAA-2", "population"),
			restored.city_daily("AAA-2", "population"), "população diária")
	if restored.days_for_city("AAA-2") != service.days_for_city("AAA-2"):
		_failures.append("dias perdidos no round-trip")
	print("serialização: %d bytes de JSON" % json.length())


func _check_idempotent(service) -> void:
	var before: int = service.nation_daily_count("AAA")
	service.sample(DAYS - 1, _summary(DAYS - 1), _month_key(DAYS - 1))
	if service.nation_daily_count("AAA") != before:
		_failures.append("mesmo dia amostrado duas vezes duplicou o ponto")


func _check_empty() -> void:
	var empty = SERVICE.new()
	if not empty.nation_daily("AAA", "population").is_empty():
		_failures.append("serviço vazio devolveu série diária")
	if not empty.nation_monthly("AAA", "population").is_empty():
		_failures.append("serviço vazio devolveu série mensal")
	if empty.month_count() != 0:
		_failures.append("serviço vazio com meses")
	# Um mês parcial (5 dias) fica com 1 mês e 5 pontos diários.
	for day in 5:
		empty.sample(day, _summary(day), _month_key(day))
	if empty.month_count() != 1 or empty.nation_daily_count("AAA") != 5:
		_failures.append("mês parcial: %d meses, %d dias"
				% [empty.month_count(), empty.nation_daily_count("AAA")])


# --- Dados sintéticos -------------------------------------------------------

func _summary(day: int) -> Dictionary:
	return {
		"day": day,
		"cities": [
			_city("AAA-1", "AAA", day),
			_city("AAA-2", "AAA", day),
			_city("BBB-1", "BBB", day),
		],
	}


func _city(id: String, nation: String, day: int) -> Dictionary:
	return {
		"id": id,
		"nation": nation,
		"population": float(day + 1) * 1000.0,
		"gdp": float(day) * 10.0,
		"production": 100.0,
		"need": 200.0 + float(day),
		"stockpile": 50.0 + float(day),
		"income": 0.5,
		"deposits": {"coal": {"remaining_kt": 1000.0 - float(day)}},
	}


## Dia 0 = janeiro de 2026; meses de 30 dias para o teste ser determinístico.
func _month_key(day: int) -> String:
	var total := day / 30
	return "%04d-%02d" % [2026 + total / 12, total % 12 + 1]


## Último dia amostrado do mês `key` (limitado ao último dia da partida).
func _last_day_of(key: String) -> int:
	var parts := key.split("-")
	var total := (int(parts[0]) - 2026) * 12 + int(parts[1]) - 1
	return mini((total + 1) * 30 - 1, DAYS - 1)


# --- Utilidades -------------------------------------------------------------

func _compare(left: PackedFloat32Array, right: PackedFloat32Array,
		what: String) -> void:
	if left.size() != right.size():
		_failures.append("%s: tamanho %d != %d" % [what, left.size(), right.size()])
		return
	for index in left.size():
		if not close(left[index], right[index]):
			_failures.append("%s[%d]: %.6f != %.6f"
					% [what, index, left[index], right[index]])
			return


func _expect(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)


static func close(a: float, b: float) -> bool:
	return absf(a - b) <= EPS
