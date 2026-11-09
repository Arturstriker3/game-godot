extends Node3D

## Cena principal: monta o mundo de cidades (fundo GPS + cidades + Voronoi) e
## conecta o sinal de comércio do núcleo C++ aos comboios 3D.

const CityWorldScript: Script = preload("res://map/city_world.gd")
const ConvoysScript: Script = preload("res://core/economy/trade_convoys.gd")

var city_world
var convoys


func _ready() -> void:
	_build_city_world()
	_build_convoys()
	Economy.trade_orders_ready.connect(convoys.spawn_orders)


func _build_city_world() -> void:
	city_world = CityWorldScript.new()
	# Mesma posição do antigo mapa, para a câmera (Player) já enquadrar o mundo.
	city_world.position = Vector3(5.45, -32.76, 89.74)
	add_child(city_world)

	var cities: Array = Economy.sim.get_status()["cities"]
	city_world.build(cities)


func _build_convoys() -> void:
	convoys = ConvoysScript.new()
	convoys.position = Vector3.ZERO
	city_world.add_child(convoys)
