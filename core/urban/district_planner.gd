extends RefCounted
class_name DistrictPlanner

## Planeja o tecido urbano como um aglomerado orgânico (referência: Victoria 3):
## sem grade nem ruas visíveis. Os lotes seguem uma espiral de ângulo áureo —
## densa no centro e rarefeita na borda — deformada por lóbulos de baixa
## frequência, o que dá um contorno irregular e "histórico" à cidade.
##
## Determinismo: a POSIÇÃO de cada lote depende só de (seed, índice). O estágio
## apenas acrescenta índices novos na borda e pode verticalizar lotes antigos;
## nada que já foi construído se move.

const GOLDEN_ANGLE := 2.399963
## Distância radial por raiz do índice; ~0,55 u entre vizinhos (footprint ~0,46).
const SPIRAL_STEP := 0.30
const SPIRAL_EXPONENT := 0.5
const POSITION_JITTER := 0.07

## Lotes por estágio: vila, cidade e metrópole.
const STAGE_LOTS := [26, 72, 150]
## Fração do raio considerada núcleo comercial.
const CORE_SHARE := 0.16

## Nível de cada módulo, usado para permitir só melhorias (nunca rebaixar).
const MODULE_LEVELS := {
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

## Caráter arquitetônico por cidade: matiz regional e tendência de
## verticalização. Não há modelos novos — apenas cor, adensamento e mix.
## Matizes sóbrios: os módulos têm paredes claras; o tint os assenta no terreno.
const CITY_STYLES := {
	"GBR-LDN": {"tint": Color(0.66, 0.56, 0.48), "mid_rise": 0.55, "stretch": 1.25},
	"FRA-PAR": {"tint": Color(0.74, 0.70, 0.62), "mid_rise": 0.85, "stretch": 1.10},
	"DEU-BER": {"tint": Color(0.60, 0.59, 0.57), "mid_rise": 0.50, "stretch": 1.15},
	"NED-AMS": {"tint": Color(0.64, 0.50, 0.40), "mid_rise": 0.30, "stretch": 1.35},
	"BEL-BRU": {"tint": Color(0.66, 0.54, 0.45), "mid_rise": 0.40, "stretch": 1.10},
}
const DEFAULT_STYLE := {"tint": Color(0.66, 0.62, 0.56), "mid_rise": 0.5, "stretch": 1.15}


## Lotes desejados para o estágio atual do layout.
func desired_lots(layout: CityLayout, industrialization: float, income: float,
		jobs: float) -> Array[Dictionary]:
	var stage: int = clampi(layout.stage, 0, STAGE_LOTS.size() - 1)
	var count: int = STAGE_LOTS[stage]
	var style: Dictionary = CITY_STYLES.get(layout.city_id, DEFAULT_STYLE)
	var shape := _shape(layout.seed_value, style)
	var lots: Array[Dictionary] = []

	for index in count:
		var lot := _plan_lot(layout.seed_value, stage, index, count, shape,
			industrialization, income, jobs, style)
		if not lot.is_empty():
			lots.append(lot)
	return lots


## Raio aproximado ocupado pelo estágio (para o solo urbano e o distrito
## industrial se afastarem do núcleo).
static func stage_radius(stage: int) -> float:
	var count: int = STAGE_LOTS[clampi(stage, 0, STAGE_LOTS.size() - 1)]
	return SPIRAL_STEP * pow(float(count), SPIRAL_EXPONENT) * 1.15


## Uma melhoria é permitida quando o módulo novo é de nível maior.
static func can_upgrade(current_id: String, target_id: String) -> bool:
	return int(MODULE_LEVELS.get(target_id, 0)) > int(MODULE_LEVELS.get(current_id, 0))


## Forma da cidade: eixo de alongamento e lóbulos, fixos por cidade.
func _shape(seed_value: int, style: Dictionary) -> Dictionary:
	var rng := RandomNumberGenerator.new()
	rng.seed = hash("shape:%d" % seed_value)
	return {
		"axis": rng.randf_range(0.0, PI),
		"stretch": float(style.get("stretch", 1.15)),
		"phase_a": rng.randf_range(0.0, TAU),
		"phase_b": rng.randf_range(0.0, TAU),
	}


func _plan_lot(seed_value: int, stage: int, index: int, count: int, shape: Dictionary,
		industrialization: float, income: float, jobs: float,
		style: Dictionary) -> Dictionary:
	# Posição: depende só de (seed, índice) — estável ao crescer.
	var place_rng := RandomNumberGenerator.new()
	place_rng.seed = hash("%d:p:%d" % [seed_value, index])
	var position := _spiral_position(index, shape, place_rng)

	# Vazios orgânicos (praças, jardins, campos) crescem para a borda.
	var radial := float(index) / float(STAGE_LOTS[STAGE_LOTS.size() - 1])
	if index > 0 and place_rng.randf() < 0.02 + radial * 0.12:
		return {}

	# Escolha do módulo: pode mudar com o estágio (verticalização).
	var rng := RandomNumberGenerator.new()
	rng.seed = hash("%d:%d:m:%d" % [seed_value, stage, index])
	var module_id := "marco_publico"
	if index > 0:
		var ring := float(index) / float(count)
		if ring <= CORE_SHARE:
			module_id = _pick_commercial(stage, industrialization, income, jobs, rng, style)
		else:
			module_id = _pick_residential(ring, stage, industrialization, income, style, rng)

	return _lot(index, position, place_rng, module_id, style)


## Espiral de ângulo áureo com alongamento e lóbulos de baixa frequência.
func _spiral_position(index: int, shape: Dictionary, rng: RandomNumberGenerator) -> Vector2:
	if index == 0:
		return Vector2.ZERO
	var angle := float(index) * GOLDEN_ANGLE
	var radius := SPIRAL_STEP * pow(float(index), SPIRAL_EXPONENT)
	var lobes := 1.0 \
		+ 0.16 * sin(2.0 * angle + float(shape["phase_a"])) \
		+ 0.09 * sin(3.0 * angle + float(shape["phase_b"]))
	var point := Vector2(cos(angle), sin(angle)) * radius * lobes
	point.x *= float(shape["stretch"])
	point = point.rotated(float(shape["axis"]))
	return point + Vector2(
		rng.randf_range(-POSITION_JITTER, POSITION_JITTER),
		rng.randf_range(-POSITION_JITTER, POSITION_JITTER))


func _pick_commercial(stage: int, industrialization: float, income: float, jobs: float,
		rng: RandomNumberGenerator, style: Dictionary) -> String:
	if stage >= 1:
		var tower_bias: float = clampf(
			industrialization * 0.22 + income * 0.18 + float(stage) * 0.06
			+ (float(style.get("mid_rise", 0.5)) - 0.5) * 0.15, 0.0, 0.35)
		if rng.randf() < tower_bias:
			return "com_torre"
	var office_share: float = clampf(jobs / 4_000_000.0, 0.0, 0.6) + float(stage) * 0.08
	return "com_escritorio_medio" if rng.randf() < office_share else "com_loja"


## Densidade cai do centro para a borda; o "mid_rise" regional puxa a cidade
## para prédios médios (Paris) ou casas baixas (Amsterdã).
func _pick_residential(ring: float, stage: int, industrialization: float,
		income: float, style: Dictionary, rng: RandomNumberGenerator) -> String:
	var mid_rise: float = float(style.get("mid_rise", 0.5))
	if stage == 0:
		return "res_vila" if rng.randf() < 0.35 else "res_casa_urbana"
	if ring <= 0.35:
		var vertical: float = clampf(
				industrialization * 0.25 + income * 0.2 + (mid_rise - 0.5) * 0.4, 0.0, 0.6)
		if rng.randf() < vertical:
			return "res_predio_10"
		return "res_predio_6" if rng.randf() < 0.6 + (mid_rise - 0.5) * 0.4 else "res_sobrado"
	if ring <= 0.65:
		var roll := rng.randf()
		if roll < 0.22 + (mid_rise - 0.5) * 0.3:
			return "res_predio_6"
		if roll < 0.40:
			return "res_conjunto"
		return "res_sobrado"
	var low := rng.randf()
	if low < 0.55:
		return "res_casa_urbana"
	if low < 0.88:
		return "res_vila"
	return "res_sobrado"


func _lot(index: int, position: Vector2, rng: RandomNumberGenerator, module_id: String,
		style: Dictionary) -> Dictionary:
	var base: Color = style.get("tint", Color.WHITE)
	var variation: float = rng.randf_range(0.86, 1.08)
	# Orientação livre, levemente puxada para a direção do centro: casas
	# "olham" para as vias implícitas sem formar grade.
	var facing := atan2(position.y, position.x)
	var yaw := facing + float(rng.randi_range(0, 3)) * PI * 0.5 + rng.randf_range(-0.35, 0.35)
	return {
		"i": index,
		"j": 0,
		"x": position.x,
		"z": position.y,
		"yaw": yaw,
		"scale": rng.randf_range(0.86, 1.12),
		"module": module_id,
		"tint": [base.r * variation, base.g * variation, base.b * variation],
	}
