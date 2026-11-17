extends Node3D

## Mede os tempos de viagem REAIS do núcleo C++ (2026-11: caminhão 62 km/h em
## 10 h/dia × desvio 1,35; navio 33 km/h 24 h × 1,15; avião 800 km/h + 3 h de
## solo) e confere que a animação do comboio usa o MESMO tempo de calendário
## (`transit_hours / 24 × 60 s` em 1x) — o veículo nunca chega antes do núcleo.
##
## Pares pedidos: Cairo→Assiut, Cairo→Assuã, Roterdã→Xangai (navio),
## Londres→Nova York (avião). Pares que a economia não negociou nos N dias são
## calculados pela MESMA fórmula do núcleo e marcados como "modelo".
##
## Uso:
##   godot --headless --path . res://tools/travel_time_report.tscn

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const ConvoysScript: Script = preload("res://core/economy/trade_convoys.gd")

## Um dia completo da economia mundial custa dezenas de segundos headless;
## 5 dias já publicam ~1.100 pedidos (variedade de pares) sem alongar o teste.
const DAYS := 30
const SECONDS_PER_DAY := 60.0
## Espelho do `ModeSpec` do núcleo (speed_kmh, batch_t, turnaround_h, duty_h,
## route_factor), só para o cálculo derivado dos pares sem pedido real.
const SPEC := {
	"truck": {"speed": 62.0, "batch": 25.0, "turnaround": 6.0,
		"duty": 10.0, "route": 1.35},
	"ship": {"speed": 33.0, "batch": 2000.0, "turnaround": 24.0,
		"duty": 24.0, "route": 1.15},
	"plane": {"speed": 800.0, "batch": 20.0, "turnaround": 3.0,
		"duty": 24.0, "route": 1.0},
}
const LOAD_HOURS_PER_BATCH := 0.5
const EARTH_RADIUS_KM := 6371.0

var _legs: Dictionary = {}
var _formula_errors := 0
var _legs_checked := 0

## Assuã não é cidade do roster (a mais próxima é Kom Ombo, a ~45 km); a
## coordenada real de Assuã entra só no cálculo derivado.
const ASWAN := {"name": "Aswan", "x": 2956.9, "y": 915.4}


func _ready() -> void:
	_run.call_deferred()


func _load_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


func _run() -> void:
	var sim: Object = Economy.sim
	if sim == null:
		print("[travel] autoload Economy sem sim")
		get_tree().quit(1)
		return
	Economy.set_speed(0)
	if Economy._ticker.is_running():
		Economy._ticker.wait()

	for day in range(1, DAYS + 1):
		var started := Time.get_ticks_msec()
		var summary: Dictionary = sim.tick_day()
		var orders_seen := 0
		for order: Variant in summary.get("trade_orders", []):
			if typeof(order) != TYPE_DICTIONARY:
				continue
			orders_seen += 1
			for leg: Variant in order.get("legs", []):
				if typeof(leg) != TYPE_DICTIONARY:
					continue
				_audit_leg(leg)
				var key := "%s|%s|%s" % [leg.get("mode", ""),
						leg.get("from", ""), leg.get("to", "")]
				if not _legs.has(key):
					_legs[key] = leg
		print("[travel] dia %d: %d pedidos, %.1f s" % [
			day, orders_seen, float(Time.get_ticks_msec() - started) / 1000.0])
	print("[travel] %d dias, %d pares de trecho, %d trechos conferidos com a fórmula (%d divergências)" % [
		DAYS, _legs.size(), _legs_checked, _formula_errors])

	var cities := _load_array(CITIES_PATH)
	var by_name := {}
	for city: Dictionary in cities:
		by_name[String(city.get("name", ""))] = city
	var ports := _load_array(PORTS_PATH)
	for port: Dictionary in ports:
		by_name[String(port.get("name", ""))] = port

	_report("Cairo→Assiut (caminhão)", "truck", "Cairo", "Assiut", by_name)
	_report("Cairo→Assuã* (caminhão)", "truck", "Cairo", "Aswan", by_name)
	_report("Cairo→Kom Ombo (caminhão, cidade real mais próxima de Assuã)",
			"truck", "Cairo", "Kom Ombo", by_name)
	_report("Rotterdam→Shanghai (navio)", "ship", "Rotterdam", "Shanghai", by_name)
	_report("London→New York City (avião)", "plane", "London", "New York City",
			by_name)
	_report_first("Trecho real de caminhão saindo de Cairo", "truck", "Cairo")
	_report_first("Trecho real de navio saindo de Rotterdam", "ship", "Rotterdam")
	_report_first("Trecho real de avião saindo de London", "plane", "London")
	_report_examples()
	print("[travel] * Assuã não está no cities.json; o valor usa a coordenada " \
			+ "real de Assuã na fórmula do núcleo (1 lote = 25 t).")
	get_tree().quit(0 if _formula_errors == 0 else 1)


## Confere `transit_hours` do núcleo contra a FÓRMULA nova (jornada + desvio de
## rota) usando a distância e os lotes que o próprio trecho reporta.
func _audit_leg(leg: Dictionary) -> void:
	var mode := String(leg.get("mode", ""))
	if not SPEC.has(mode):
		return
	var spec: Dictionary = SPEC[mode]
	var distance := float(leg.get("distance_km", 0.0))
	var trips := maxi(1, int(leg.get("trips", 1)))
	var effective := float(spec["speed"]) * float(spec["duty"]) / 24.0
	var expected := distance * float(spec["route"]) / effective \
			+ float(spec["turnaround"]) + float(trips) * LOAD_HOURS_PER_BATCH
	var actual := float(leg.get("transit_hours", 0.0))
	_legs_checked += 1
	if absf(expected - actual) > 0.05:
		_formula_errors += 1
		if _formula_errors <= 5:
			print("[travel] DIVERGE %s %s→%s: núcleo %.2f h, fórmula %.2f h" % [
				mode, leg.get("from", "?"), leg.get("to", "?"), actual, expected])


func _report(label: String, mode: String, from: String, to: String,
		by_name: Dictionary) -> void:
	var leg: Dictionary = _legs.get("%s|%s|%s" % [mode, from, to], {})
	if not leg.is_empty():
		_print_leg(label, leg, "núcleo")
		return
	var a: Dictionary = by_name.get(from, {})
	var b: Dictionary = ASWAN if to == "Aswan" else by_name.get(to, {})
	if a.is_empty() or b.is_empty():
		print("[travel] %s: par ausente do cenário (não medido)" % label)
		return
	var spec: Dictionary = SPEC[mode]
	var distance := _distance_km(a, b)
	var effective := float(spec["speed"]) * float(spec["duty"]) / 24.0
	var hours := distance * float(spec["route"]) / effective \
			+ float(spec["turnaround"]) + LOAD_HOURS_PER_BATCH
	_print_leg(label, {
		"mode": mode,
		"from": from,
		"to": to,
		"distance_km": distance,
		"trips": 1,
		"transit_hours": hours,
	}, "modelo (não negociado em %d dias)" % DAYS)


## Primeiro trecho REAL do núcleo com esta origem (para dar um valor de mercado
## além dos pares pedidos, que nem sempre são negociados).
func _report_first(label: String, mode: String, from: String) -> void:
	for key: Variant in _legs:
		var leg: Dictionary = _legs[key]
		if String(leg.get("mode", "")) == mode \
				and String(leg.get("from", "")) == from:
			_print_leg("%s→%s (%s, exemplo real)" % [from, leg.get("to", "?"),
					mode], leg, "núcleo")
			return
	print("[travel] %s: nenhum trecho %s nos %d dias" % [label, mode, DAYS])


## Um exemplo real por modal (o mais longo de cada), independente de origem.
func _report_examples() -> void:
	var best: Dictionary = {}
	var counts: Dictionary = {}
	for key: Variant in _legs:
		var leg: Dictionary = _legs[key]
		var mode := String(leg.get("mode", ""))
		counts[mode] = int(counts.get(mode, 0)) + 1
		if not best.has(mode) or float(leg.get("distance_km", 0.0)) \
				> float(best[mode].get("distance_km", 0.0)):
			best[mode] = leg
	for mode: String in ["truck", "ship", "plane"]:
		if not best.has(mode):
			print("[travel] exemplos: nenhum trecho %s nos %d dias" % [mode, DAYS])
			continue
		_print_leg("Mais longo %s (%d pares no total)" % [mode,
				int(counts.get(mode, 0))], best[mode], "núcleo")


func _print_leg(label: String, leg: Dictionary, fonte: String) -> void:
	var mode := String(leg.get("mode", ""))
	var hours := float(leg.get("transit_hours", 0.0))
	var seconds := float(ConvoysScript._segment_seconds(mode, hours,
			float(leg.get("distance_km", 0.0)), 1.0))
	var expected := hours / 24.0 * SECONDS_PER_DAY
	print("[travel] %s: %.0f km, %d lote(s), %.1f h de jogo, %.1f s reais em 1x — animação %.1f s (%s)" % [
		label, float(leg.get("distance_km", 0.0)), int(leg.get("trips", 1)),
		hours, expected, seconds, fonte])
	if absf(seconds - maxf(expected, 0.35)) > 0.02:
		print("[travel] FALHA: animação %.2f s ≠ núcleo %.2f s em %s" % [
			seconds, expected, label])
		_formula_errors += 1


func _distance_km(a: Dictionary, b: Dictionary) -> float:
	var lat1 := deg_to_rad(90.0 - float(a.get("y", 0.0)) / 2500.0 * 180.0)
	var lat2 := deg_to_rad(90.0 - float(b.get("y", 0.0)) / 2500.0 * 180.0)
	var dlat := lat2 - lat1
	var dlon := deg_to_rad((float(b.get("x", 0.0)) - float(a.get("x", 0.0)))
			/ 5000.0 * 360.0)
	var s1 := sin(dlat / 2.0)
	var s2 := sin(dlon / 2.0)
	var h := s1 * s1 + cos(lat1) * cos(lat2) * s2 * s2
	return 2.0 * EARTH_RADIUS_KM * asin(sqrt(h))
