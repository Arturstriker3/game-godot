extends Node

## Autoload `Economy`: acopla o núcleo C++ (EconomySim) ao jogo.
## Avança um dia por tick e mostra o resumo no console e numa label na tela.
##
## A simulação é regional: cada nação agrega estados, e cada estado tem
## população, produção e demanda próprias.

const TICK_SECONDS := 1.5

## Emitido a cada tick com os pedidos de comércio do dia.
signal trade_orders_ready(orders: Array)

var sim: RefCounted
var _label: Label


func _ready() -> void:
	sim = EconomySim.new()
	sim.initialize()
	_build_overlay()

	var timer := Timer.new()
	timer.wait_time = TICK_SECONDS
	timer.timeout.connect(_run_day)
	add_child(timer)
	timer.start()

	# Adia o primeiro dia: a cena principal conecta o sinal de comércio no _ready.
	_run_day.call_deferred()


func _build_overlay() -> void:
	var layer := CanvasLayer.new()
	layer.layer = 100
	add_child(layer)

	_label = Label.new()
	_label.position = Vector2(16, 16)
	_label.add_theme_font_size_override("font_size", 14)
	_label.add_theme_color_override("font_color", Color(1, 1, 1))
	_label.add_theme_color_override("font_outline_color", Color(0, 0, 0))
	_label.add_theme_constant_override("outline_size", 4)
	layer.add_child(_label)


func _run_day() -> void:
	var summary: Dictionary = sim.tick()
	var text := _format(summary)
	_label.text = text
	print(text)
	trade_orders_ready.emit(summary["trade_orders"])


func _format(summary: Dictionary) -> String:
	var lines: Array[String] = ["Dia %d" % summary["day"]]

	lines.append_array(_format_nations(summary["nations"]))
	lines.append_array(_format_states(summary["states"]))
	lines.append_array(_format_orders(summary["trade_orders"]))

	return "\n".join(lines)


func _format_nations(nations: Array) -> Array[String]:
	var lines: Array[String] = ["-- Nacoes --"]

	for nation in nations:
		lines.append("%-8s pop %.1fM  demanda %.0f  deficit %.0f  estoque %.0f  transito %.0f" % [
			nation["name"],
			nation["population"] / 1_000_000.0,
			nation["need"],
			nation["deficit"],
			nation["stockpile"],
			nation["in_transit"],
		])

	return lines


func _format_states(states: Array) -> Array[String]:
	var lines: Array[String] = ["-- Estados --"]

	for state in states:
		lines.append("  %-3s %-22s pop %5.1fM  prod %5.0f  demanda %5.0f  estoque %6.0f  env %5.0f  trans %5.0f" % [
			state["nation"],
			state["name"],
			state["population"] / 1_000_000.0,
			state["production"],
			state["need"],
			state["stockpile"],
			state["dispatched"],
			state["in_transit"],
		])

	return lines


func _format_orders(orders: Array) -> Array[String]:
	var lines: Array[String] = ["-- Comercio --"]

	for order in orders:
		lines.append("[%s] %s -> %s: %.0f t | %.1f h | frete %.0f" % [
			order["scope"], order["from_state"], order["to_state"], order["amount"],
			order["transit_hours"], order["freight_cost"],
		])
		for leg in order["legs"]:
			lines.append("    %-5s %6.0f km  %5.0f t  %2d viagens  %5.1f h" % [
				leg["mode"], leg["distance_km"], leg["load_tons"],
				leg["trips"], leg["transit_hours"],
			])

	return lines
