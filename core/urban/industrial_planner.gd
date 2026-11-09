extends RefCounted
class_name IndustrialPlanner

## Planeja o distrito industrial, separado do tecido urbano.
##
## Cada setor ocupa uma faixa fixa da grade a leste da cidade: os complexos
## principais usam as três primeiras posições e as estruturas auxiliares as três
## seguintes. Como a posição depende só do setor e do índice, a cidade pode
## crescer sem deslocar o que já foi construído (determinismo e save/load).

## Distância da âncora industrial ao centro e passo da espiral do distrito.
const ANCHOR_DISTANCE := 4.6
const CLUSTER_STEP := 0.5
const MIN_SHARE := 0.05
const MIN_INTENSITY := 0.12
const SLOTS_PER_ROLE := 3

const SECTOR_ORDER: Array[String] = ["steel", "manufacturing", "refining", "power", "storage"]

## Índice do primeiro slot de cada setor: 3 principais + 3 auxiliares.
const SECTOR_SLOT_BASE := {
	"steel": 0,
	"manufacturing": 6,
	"refining": 12,
	"power": 18,
	"storage": 24,
}

## O estágio escolhe a variante mais moderna do setor (modernização).
const MAIN_MODULES := {
	"steel": ["ind_siderurgia"],
	"manufacturing": ["ind_fabrica", "ind_manufatura"],
	"refining": ["ind_refinaria"],
	"power": ["ind_energia"],
	"storage": ["ind_galpao", "ind_armazem"],
}

const AUX_MODULES := {
	"steel": ["ind_aux_chamine", "ind_aux_patio", "ind_aux_tanques"],
	"manufacturing": ["ind_aux_galpao", "ind_aux_patio", "ind_aux_chamine"],
	"refining": ["ind_aux_tanques", "ind_aux_chamine", "ind_aux_patio"],
	"power": ["ind_aux_chamine", "ind_aux_tanques", "ind_aux_patio"],
	"storage": ["ind_aux_galpao", "ind_aux_tanques", "ind_aux_patio"],
}

const MODULE_LEVELS := {
	"ind_aux_chamine": 1,
	"ind_aux_tanques": 1,
	"ind_aux_galpao": 1,
	"ind_aux_patio": 1,
	"ind_galpao": 1,
	"ind_armazem": 2,
	"ind_fabrica": 2,
	"ind_manufatura": 3,
	"ind_siderurgia": 3,
	"ind_refinaria": 3,
	"ind_energia": 3,
}


## Lotes industriais desejados para o perfil econômico atual.
func desired_lots(seed_value: int, profile: Dictionary, stage: int) -> Array[Dictionary]:
	var intensity := clampf(float(profile.get("intensity", 0.0)), 0.0, 1.0)
	if intensity < MIN_INTENSITY:
		return []

	var total_main := clampi(
		roundi(intensity * (4.0 + float(stage) * 4.0)), 1, SECTOR_ORDER.size() * SLOTS_PER_ROLE)
	var main_counts := _distribute(profile, total_main)
	var aux_raw := roundi(float(total_main) * (0.35 + float(stage) * 0.35))
	var total_aux := clampi(maxi(aux_raw, 1 if total_main > 0 else 0),
		0, SECTOR_ORDER.size() * SLOTS_PER_ROLE)
	var aux_counts := _distribute(profile, total_aux)

	var specs: Array[Dictionary] = []
	for sector in SECTOR_ORDER:
		var main_count: int = int(main_counts.get(sector, 0))
		var aux_count: int = int(aux_counts.get(sector, 0))
		if main_count == 0 and aux_count == 0:
			continue
		for _index in main_count:
			specs.append({
				"sector": sector,
				"module": _main_module(sector, stage),
				"aux": false,
			})
		var aux_list: Array = AUX_MODULES[sector]
		for index in aux_count:
			specs.append({
				"sector": sector,
				"module": String(aux_list[index % aux_list.size()]),
				"aux": true,
			})

	return _place(seed_value, specs)


## Uma melhoria é permitida quando o módulo novo é de nível maior.
static func can_upgrade(current_id: String, target_id: String) -> bool:
	return int(MODULE_LEVELS.get(target_id, 0)) > int(MODULE_LEVELS.get(current_id, 0))


func _main_module(sector: String, stage: int) -> String:
	var list: Array = MAIN_MODULES[sector]
	return String(list[mini(stage, list.size() - 1)])


## Distribui um total de posições pelas frações do perfil, com teto por setor.
func _distribute(profile: Dictionary, total: int) -> Dictionary:
	if total <= 0:
		return {}

	var counts := {}
	var remainders: Array = []
	var assigned := 0

	for sector in SECTOR_ORDER:
		var share := maxf(float(profile.get(sector, 0.0)), 0.0)
		var raw := share * float(total)
		var base := mini(int(floor(raw)), SLOTS_PER_ROLE)
		counts[sector] = base
		assigned += base
		remainders.append({"sector": sector, "frac": raw - floor(raw), "share": share})

	var remaining := mini(total - assigned,
		SECTOR_ORDER.size() * SLOTS_PER_ROLE - assigned)
	remainders.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		if is_equal_approx(float(a["frac"]), float(b["frac"])):
			return String(a["sector"]) < String(b["sector"])
		return float(a["frac"]) > float(b["frac"]))

	var guard := 0
	while remaining > 0 and guard < 64:
		guard += 1
		var progressed := false
		for item in remainders:
			if remaining <= 0:
				break
			if float(item["share"]) < MIN_SHARE:
				continue
			var sector: String = item["sector"]
			if int(counts[sector]) >= SLOTS_PER_ROLE:
				continue
			counts[sector] = int(counts[sector]) + 1
			remaining -= 1
			progressed = true
		if not progressed:
			break

	return counts


## Posiciona os lotes na grade fixa do distrito, com jitter determinístico.
func _place(seed_value: int, specs: Array[Dictionary]) -> Array[Dictionary]:
	var lots: Array[Dictionary] = []
	var counters := {}

	for spec in specs:
		var sector: String = spec["sector"]
		var role := "aux" if bool(spec["aux"]) else "main"
		var counter_key := sector + ":" + role
		var index: int = int(counters.get(counter_key, 0))
		counters[counter_key] = index + 1
		if index >= SLOTS_PER_ROLE:
			continue

		var slot: int = int(SECTOR_SLOT_BASE[sector]) \
				+ (SLOTS_PER_ROLE if role == "aux" else 0) + index
		var key := "%s:%s:%d" % [sector, role, index]

		var rng := RandomNumberGenerator.new()
		rng.seed = hash("%d:%s" % [seed_value, key])

		# Aglomerado orgânico na borda da cidade: espiral em torno de uma âncora
		# cuja direção é fixa por cidade. A posição depende só do slot.
		var position := _anchor(seed_value) + _cluster_offset(slot) + Vector2(
				rng.randf_range(-0.12, 0.12), rng.randf_range(-0.12, 0.12))

		# Fuligem/concreto: indústria mais escura que o tecido urbano.
		var tint := rng.randf_range(0.6, 0.72)
		lots.append({
			"key": key,
			"sector": sector,
			"module": String(spec["module"]),
			"x": position.x,
			"z": position.y,
			"yaw": float(rng.randi_range(0, 3)) * PI * 0.5 + rng.randf_range(-0.25, 0.25),
			"scale": rng.randf_range(0.95, 1.06),
			"tint": [tint, tint, tint],
			"level": int(MODULE_LEVELS.get(String(spec["module"]), 1)),
		})

	return lots


## Direção do distrito industrial, fixa por cidade (fora do núcleo urbano).
func _anchor(seed_value: int) -> Vector2:
	var rng := RandomNumberGenerator.new()
	rng.seed = hash("industry:%d" % seed_value)
	return Vector2.RIGHT.rotated(rng.randf_range(0.0, TAU)) * ANCHOR_DISTANCE


## Posição do slot numa espiral de ângulo áureo ao redor da âncora.
func _cluster_offset(slot: int) -> Vector2:
	if slot == 0:
		return Vector2.ZERO
	var angle := float(slot) * 2.399963
	return Vector2(cos(angle), sin(angle)) * CLUSTER_STEP * sqrt(float(slot))
