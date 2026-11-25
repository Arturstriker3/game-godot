extends RefCounted
class_name GroupNamer

## Nomeia os grupos econômicos pela cultura de origem.
##
## Porte fiel de `AiGroupInvestor::name_groups` (`gdextension/src/sim/ai/`)
## para uso depois que a IA passou a viver no C++: o núcleo não sabe cultura nem
## idioma, então só o NOME continua em GDScript. Nada aqui decide economia — a
## função lê o ranking de grupos do núcleo e devolve os nomes por
## `set_group_names`.
##
## O `place_by_nation` (opcional) dá o nome da cidade-sede usado nos padrões com
## `{place}`. Garante unicidade dentro do lote; o núcleo só armazena.

const CultureDb := preload("res://core/culture/culture_db.gd")
const BusinessNameGenerator := preload("res://core/culture/business_name_generator.gd")

## Setor do bem (goods.json) → setor de nome de empresa (business_names).
const SECTOR_TO_BUSINESS := {
	"agro": "wholesale", "food": "food_service", "mining": "construction",
	"energy": "construction", "metal": "construction",
	"machinery": "construction", "chemical": "construction",
	"wood": "construction", "textile": "retail", "luxury": "retail",
	"packaging": "retail", "electronics": "it",
	"transport": "freight_logistics", "military": "professional_services",
	"construction": "construction", "primary": "wholesale",
	"industry": "construction", "state": "finance",
}


## Devolve quantos nomes o núcleo aplicou (0 = sem grupo, sem cultura ou sem
## dados de nome). `sim` é a fachada `EconomySim`.
static func name_groups(sim: Object, place_by_nation: Dictionary = {}) -> int:
	if sim == null:
		return 0
	var groups: Array = []
	if sim.has_method("get_group_ranking"):
		groups = sim.get_group_ranking()
	elif sim.has_method("get_groups"):
		groups = sim.get_groups()
	if groups.is_empty():
		return 0
	if not BusinessNameGenerator.load_data() or not CultureDb.load_data():
		return 0
	var generator := BusinessNameGenerator.new()
	var names: Dictionary = {}
	var used: Dictionary = {}
	for group: Dictionary in groups:
		var id := String(group.get("id", ""))
		if id.is_empty():
			continue
		var origin := String(group.get("origin_nation", ""))
		var sector := String(group.get("sector", ""))
		var culture := CultureDb.national_culture(origin)
		var business_sector := String(SECTOR_TO_BUSINESS.get(sector, "wholesale"))
		var place := String(place_by_nation.get(origin, ""))
		var seed_value := absi(hash(id)) % 100000
		var name := ""
		if culture != "" and BusinessNameGenerator.has_sector(
				BusinessNameGenerator.language_for(culture), business_sector):
			name = generator.generate(culture, business_sector, place, seed_value)
		if name.is_empty():
			# Fallback declarado: rótulo do setor + nação. Nunca inventa cultura.
			name = "%s %s" % [business_sector.capitalize(), origin]
		var key := name.to_lower()
		if used.has(key):
			var suffix := 2
			while used.has("%s %d" % [key, suffix]):
				suffix += 1
			name = "%s %d" % [name, suffix]
			key = name.to_lower()
		used[key] = true
		names[id] = name
	if names.is_empty():
		return 0
	return int(sim.set_group_names(names))
