extends RefCounted
class_name DistrictPlanner

## Planeja os quarteirões de uma cidade: densidade, zonas e verticalização.
## Tudo é determinístico por (seed da cidade, estágio, célula), então a mesma
## cidade cresce sempre da mesma forma e preserva o que já foi construído.

const LOT_PITCH := 0.5
const LOT_JITTER := 0.02

## Ruas a cada quatro células (quarteirões de três lotes, ~75 m) com uma faixa
## de 0,5 unidade (~25 m): proporção plausível para uma cidade europeia.
const STREET_PERIOD := 4
const STREET_OFFSET := 2

## Raio (em lotes) e preenchimento por estágio: núcleo, bairros e metrópole.
const STAGE_RADIUS := [2, 4, 6]
const STAGE_FILL := [0.55, 0.74, 0.90]

## Nível de cada módulo, usado para permitir só melhorias (nunca rebaixar).
const MODULE_LEVELS := {
	"rua": 0,
	"marco_publico": 1,
	"res_casa_urbana": 1,
	"res_vila": 1,
	"res_sobrado": 2,
	"com_loja": 2,
	"ind_galpao": 2,
	"res_predio_6": 3,
	"com_escritorio_medio": 3,
	"ind_fabrica": 3,
	"res_predio_10": 4,
	"res_conjunto": 4,
	"com_torre": 5,
}


## Lotes desejados para o estágio atual do layout.
func desired_lots(layout: CityLayout, industrialization: float, income: float,
		jobs: float) -> Array[Dictionary]:
	var stage: int = clampi(layout.stage, 0, STAGE_RADIUS.size() - 1)
	var radius: int = STAGE_RADIUS[stage]
	var lots: Array[Dictionary] = []

	for i in range(-radius, radius + 1):
		for j in range(-radius, radius + 1):
			var lot := _plan_lot(layout.seed_value, stage, i, j,
				industrialization, income, jobs)
			if not lot.is_empty():
				lots.append(lot)

	return lots


## Uma melhoria é permitida quando o módulo novo é de nível maior.
static func can_upgrade(current_id: String, target_id: String) -> bool:
	return int(MODULE_LEVELS.get(target_id, 0)) > int(MODULE_LEVELS.get(current_id, 0))


func _plan_lot(seed_value: int, stage: int, i: int, j: int,
		industrialization: float, income: float, jobs: float) -> Dictionary:
	var rng := RandomNumberGenerator.new()
	rng.seed = hash("%d:%d:%d:%d" % [seed_value, stage, i, j])

	var ring: int = maxi(absi(i), absi(j))
	if ring == 0:
		return _lot(i, j, rng, "marco_publico")

	# Ruas: uma faixa livre a cada quatro células, formando quarteirões legíveis.
	if posmod(i, STREET_PERIOD) == STREET_OFFSET \
			or posmod(j, STREET_PERIOD) == STREET_OFFSET:
		return _street_lot(i, j)

	var fill: float = clampf(STAGE_FILL[stage] - 0.07 * float(ring - 1), 0.30, 0.95)
	if rng.randf() > fill:
		return {}

	# A indústria não nasce no tecido urbano: ela vive no distrito industrial,
	# planejado separadamente por IndustrialPlanner.
	var zone := _zone_for(i, j, ring)
	var module_id := _pick_module(zone, ring, stage, industrialization, income, jobs, rng)
	return _lot(i, j, rng, module_id)


## Núcleo comercial e bairros residenciais; indústria fica fora deste plano.
func _zone_for(i: int, j: int, ring: int) -> String:
	if ring <= 2:
		return "commercial"
	return "residential"


func _pick_module(zone: String, ring: int, stage: int, industrialization: float,
		income: float, jobs: float, rng: RandomNumberGenerator) -> String:
	match zone:
		"commercial":
			# Torres só no núcleo e em cidades já verticalizadas.
			if stage >= 1 and ring <= 2:
				var tower_bias: float = clampf(
					industrialization * 0.28 + income * 0.22 + float(stage) * 0.08, 0.0, 0.5)
				if rng.randf() < tower_bias:
					return "com_torre"
			var office_share: float = clampf(jobs / 4_000_000.0, 0.0, 0.7) \
					+ float(stage) * 0.08
			return "com_escritorio_medio" if rng.randf() < office_share else "com_loja"
		_:
			return _pick_residential(ring, stage, industrialization, income, rng)


## Densidade cai do centro para a periferia; nada de torre isolada no subúrbio.
func _pick_residential(ring: int, stage: int, industrialization: float,
		income: float, rng: RandomNumberGenerator) -> String:
	if stage == 0:
		return "res_vila" if rng.randf() < 0.35 else "res_casa_urbana"
	if ring <= 2:
		var vertical: float = clampf(industrialization * 0.30 + income * 0.25, 0.0, 0.55)
		return "res_predio_10" if rng.randf() < vertical else "res_predio_6"
	if ring <= 4:
		var roll := rng.randf()
		if roll < 0.35:
			return "res_predio_6"
		if roll < 0.60:
			return "res_conjunto"
		return "res_sobrado"
	var low := rng.randf()
	if low < 0.55:
		return "res_casa_urbana"
	if low < 0.85:
		return "res_vila"
	return "res_sobrado"


func _street_lot(i: int, j: int) -> Dictionary:
	return {
		"i": i,
		"j": j,
		"x": i * LOT_PITCH,
		"z": j * LOT_PITCH,
		"yaw": 0.0,
		"scale": 1.0,
		"module": "rua",
		"tint": [1.0, 1.0, 1.0],
	}


func _lot(i: int, j: int, rng: RandomNumberGenerator, module_id: String) -> Dictionary:
	var tint: float = rng.randf_range(0.94, 1.04)
	return {
		"i": i,
		"j": j,
		"x": i * LOT_PITCH + rng.randf_range(-LOT_JITTER, LOT_JITTER),
		"z": j * LOT_PITCH + rng.randf_range(-LOT_JITTER, LOT_JITTER),
		"yaw": float(rng.randi_range(0, 3)) * PI * 0.5 + rng.randf_range(-0.035, 0.035),
		"scale": rng.randf_range(0.94, 1.04),
		"module": module_id,
		"tint": [tint, tint, tint],
	}
