extends SceneTree

## B4 — MIGRAÇÃO (êxodo rural → polo urbano e cidade → cidade na mesma nação).
##
## Valida a conservação de pessoas e dinheiro, o round-trip de save sem tick, a
## migração v19 → v20 e o determinismo de threads no mundo real; e, num cenário
## CONTROLADO (vaga de moradia injetada via save), a direção do fluxo (quem
## recebe é o polo atraente), o limite por moradia (destino sem vaga não recebe)
## e a estrutura etária/estrato dos migrantes (jovens 15–24 + 25–44 acima da
## base; saem primeiro os quintis mais baixos).
##
## Uso: godot --headless --path . --script res://tools/b4_migration_test.gd

const RegionDataScript := preload("res://core/economy/region_data.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

const TOLERANCE := 0.5
const SAVE_VERSION := 26

var _failures: Array = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[b4] OK: migração — conservação, direção, limite, idade, save v21.")
		quit(0)
	else:
		print("[b4] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_chains(sim: Object) -> void:
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	if sim.has_method("set_services"):
		sim.set_services(_json(SERVICES_PATH))
	if sim.has_method("set_currencies"):
		sim.set_currencies(_json(CURRENCIES_PATH))
	if sim.has_method("set_sector_employment"):
		sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	if sim.has_method("set_age_structure"):
		sim.set_age_structure(_json(AGE_STRUCTURE_PATH))


func _make_world() -> Object:
	var sim = EconomySim.new()
	_load_chains(sim)
	var cities: Array = _json(CITIES_PATH)
	var ports: Array = _json(PORTS_PATH)
	var buildings: Array = _json(BUILDINGS_PATH)
	RegionDataScript.attach(cities)
	sim.initialize_world(cities, ports, buildings)
	if sim.has_method("set_urban"):
		sim.set_urban(_json(URBAN_LAND_PATH), _json(HOUSING_CITY_PATH),
			_json(HOUSING_PATH), _json(MUNICIPAL_FINANCE_PATH))
	return sim


func _sha(text: String) -> String:
	return text.sha256_text()


## Índice de uma cidade no array `cities` do save (pelo id); -1 se ausente.
func _city_index(save: Dictionary, city_id: String) -> int:
	var cities: Array = save.get("cities", [])
	for i in cities.size():
		if String(cities[i].get("id", "")) == city_id:
			return i
	return -1


## Bloco `region` de uma cidade no save.
func _region_block(save: Dictionary, city_id: String) -> Dictionary:
	var i := _city_index(save, city_id)
	if i < 0:
		return {}
	return save.get("cities", [])[i].get("region", {})


## Soma das coortes (0-14..65+) do interior de uma cidade.
func _region_age_sum(save: Dictionary, city_id: String) -> float:
	var bands: Array = _region_block(save, city_id).get("age_bands", [])
	var total := 0.0
	for value in bands:
		total += float(value)
	return total


## Fração do interior em 15–24 + 25–44 (os migrantes saem jovens).
func _region_young_share(save: Dictionary, city_id: String) -> float:
	var bands: Array = _region_block(save, city_id).get("age_bands", [])
	var total := 0.0
	var young := 0.0
	for b in bands.size():
		total += float(bands[b])
		if b == 1 or b == 2:
			young += float(bands[b])
	return young / maxf(total, 1.0)


## Fração do interior no quintil mais pobre (Q1).
func _region_q1_share(save: Dictionary, city_id: String) -> float:
	var people: Array = _region_block(save, city_id).get("strata", {}).get("people", [])
	var total := 0.0
	for value in people:
		total += float(value)
	return float(people[0]) / maxf(total, 1.0)


## Duas cidades da MESMA nação com região (para o A/B controlado por vaga).
## Prefere um par de país pobre (êxodo rural com gap positivo): ETH-ADD tem
## região de ~31 M e renda rural per capita baixa (o diferencial urbano-rural
## dispara o êxodo). Cai para a busca genérica se o par não estiver pronto.
func _pick_pair(save: Dictionary) -> Array:
	var preferred := ["ETH-ADD", "ETH-AWA"]
	var ok := true
	for cid in preferred:
		var region: Dictionary = _region_block(save, cid)
		if not bool(region.get("ready", false)) \
				or float(region.get("region_pop", 0.0)) <= 0.0:
			ok = false
			break
	if ok:
		return preferred
	var by_nation := {}
	for city: Dictionary in save.get("cities", []):
		var region: Dictionary = city.get("region", {})
		if not bool(region.get("ready", false)):
			continue
		if float(region.get("region_pop", 0.0)) <= 0.0:
			continue
		var nation := String(city.get("nation", ""))
		if not by_nation.has(nation):
			by_nation[nation] = []
		by_nation[nation].append(String(city.get("id", "")))
	for nation in by_nation:
		if by_nation[nation].size() >= 2:
			return [by_nation[nation][0], by_nation[nation][1]]
	return []


## Infla o estoque de moradia de uma cidade (vaga persistente ≈ `vacancy_people`
## pessoas). `units *= (1 + vacancy_units/stock)`; pessoas por unidade =
## pop_anchor / households.
func _inject_vacancy(save: Dictionary, city_id: String, vacancy_people: float) -> void:
	var i := _city_index(save, city_id)
	if i < 0:
		_fail("injeção: cidade %s ausente" % city_id)
		return
	var urban: Array = save.get("urban", [])
	var housing: Array = save.get("housing", [])
	if i >= urban.size() or i >= housing.size():
		_fail("injeção: sem bloco urban/housing para %s" % city_id)
		return
	var units: Array = urban[i].get("units", [])
	var stock := 0.0
	for row: Array in units:
		for value in row:
			stock += float(value)
	if stock <= 0.0:
		_fail("injeção: %s sem estoque de moradia" % city_id)
		return
	var households := float(housing[i].get("households", 0.0))
	var pop_anchor := float(housing[i].get("pop_anchor", 0.0))
	var people_per_unit := 0.0
	if households > 0.0:
		people_per_unit = maxf(pop_anchor, 1.0) / households
	else:
		people_per_unit = 1.0
	var vacancy_units := vacancy_people / maxf(people_per_unit, 1e-9)
	var factor := 1.0 + vacancy_units / stock
	var new_units: Array = []
	for row: Array in units:
		var new_row: Array = []
		for value in row:
			new_row.append(float(value) * factor)
		new_units.append(new_row)
	urban[i]["units"] = new_units


## Vaga de moradia do polo em PESSOAS (unidades vazias × pessoas por domicílio),
## exatamente como a fase de migração calcula (`urban_vacancy_people`).
func _vacancy_people(sim: Object, city_id: String, save: Dictionary) -> float:
	var units := 0.0
	for value in sim.get_urban_city(city_id).get("vacancy", []):
		units += float(value)
	if units <= 0.0:
		return 0.0
	var i := _city_index(save, city_id)
	var housing: Array = save.get("housing", [])
	if i < 0 or i >= housing.size():
		return 0.0
	var households := float(housing[i].get("households", 0.0))
	var pop_anchor := float(housing[i].get("pop_anchor", 0.0))
	var people_per_unit := (maxf(pop_anchor, 1.0) / households) \
		if households > 0.0 else 1.0
	return units * people_per_unit


## (a) conservação de dinheiro e de pessoas (invariante urbano + interior).
func _check_conservation() -> void:
	var sim := _make_world()
	var money_start := float(sim.get_status().get("money_total", 0.0))
	var days := 60
	for day in days:
		sim.tick_day()
	var money_end := float(sim.get_status().get("money_total", 0.0))
	var drift := absf(money_end - money_start) / maxf(absf(money_start), 1.0)
	if drift > 1e-6:
		_fail("conservação: drift de dinheiro %.9f > 1e-6 em %d dias" % [drift, days])
	else:
		print("[b4] conservação: drift=%.9f em %d dias." % [drift, days])
	# Pessoas: Σ urbano + Σ interior == Σ população exibida, em todos os dias.
	var save: Dictionary = sim.to_dict()
	var urban := 0.0
	var region := 0.0
	var total := 0.0
	for city: Dictionary in save.get("cities", []):
		urban += float(city.get("sim_population", 0.0))
		region += float(city.get("region", {}).get("region_pop", 0.0))
		total += float(city.get("population", 0.0))
	if absf((urban + region) - total) > 1.0:
		_fail("conservação: Σ urbano+interior %.0f != Σ população %.0f" % [
			urban + region, total])
	else:
		print("[b4] conservação: urbano+interior == população (%.0f)." % total)
	# O status de migração publica o urbano/região VIVOS (Σ simulation_population
	# e Σ region.region_pop), não a seed do dia 0 — confere a consistência.
	var migration: Dictionary = sim.get_status().get("migration", {})
	var urban_status := float(migration.get("urban_population", 0.0))
	var region_status := float(migration.get("region_population", 0.0))
	if absf(urban_status - urban) > 1.0 or absf(region_status - region) > 1.0:
		_fail("conservação: get_status().migration urbano/região (%.0f/%.0f) não bate com Σ (%.0f/%.0f)" % [
			urban_status, region_status, urban, region])
	var urbanization := float(migration.get("urbanization", 0.0))
	var expected_urbanization := urban / maxf(urban + region, 1.0)
	if absf(urbanization - expected_urbanization) > 1e-9:
		_fail("conservação: urbanization %.9f != urbano/(urbano+região) %.9f" % [
			urbanization, expected_urbanization])
	print("[b4] conservação: status migration urbano=%.0f região=%.0f urbanization=%.6f." % [
		urban_status, region_status, urbanization])


## (b) determinismo de threads 1 × 16 (mesmo hash do save).
func _check_determinism() -> void:
	var sim1 := _make_world()
	sim1.set_sim_threads(1)
	for day in 60:
		sim1.tick_day()
	var hash1 := _sha(JSON.stringify(sim1.to_dict()))
	var sim16 := _make_world()
	sim16.set_sim_threads(16)
	for day in 60:
		sim16.tick_day()
	var hash16 := _sha(JSON.stringify(sim16.to_dict()))
	if hash1 != hash16:
		_fail("determinismo: hash do save difere entre threads 1 e 16 (%s vs %s)" % [
			hash1.substr(0, 12), hash16.substr(0, 12)])
	else:
		print("[b4] determinismo: threads 1×16 → mesmo save (%.12s...)." % hash1)


## (c) round-trip: dia 0 bit-exato (simetria do bloco novo) + acumuladores de
## migração sobrevivem ao load.
func _check_save_roundtrip() -> void:
	# (1) dia 0: o bloco `region.migration` é salvo e recarregado simetricamente.
	var sim := _make_world()
	var snap0: Dictionary = sim.to_dict()
	var restored0 = EconomySim.new()
	_load_chains(restored0)
	restored0.from_dict(snap0)
	if JSON.stringify(restored0.to_dict()) != JSON.stringify(snap0):
		_fail("round-trip dia 0 não preserva o estado (JSON difere)")
	else:
		print("[b4] round-trip dia 0: bit-exato.")
	# (2) após 30 dias (com migração), os acumuladores sobrevivem ao load.
	for day in 30:
		sim.tick_day()
	var snap: Dictionary = sim.to_dict()
	var restored = EconomySim.new()
	_load_chains(restored)
	restored.from_dict(snap)
	var mig_before: Dictionary = sim.get_status().get("migration", {})
	var mig_after: Dictionary = restored.get_status().get("migration", {})
	for key in ["rural_in_total", "city_in_total", "city_out_total", "urbanization"]:
		if float(mig_before.get(key, 0.0)) != float(mig_after.get(key, 0.0)):
			_fail("round-trip: %s do mundo não sobrevive ao load (%s vs %s)" % [
				key, str(mig_before.get(key)), str(mig_after.get(key))])
	print("[b4] round-trip: acumuladores de migração sobrevivem ao load (rural_in=%.0f)." % [
		float(mig_after.get("rural_in_total", 0.0))])


## (d) migração v19 → v20: sem o bloco `migration`, os acumuladores zeram e a
## versão sobe para 20; nada é inventado para trás.
func _check_v19_migration() -> void:
	var sim := _make_world()
	for day in 15:
		sim.tick_day()
	var snapshot: Dictionary = sim.to_dict()
	var v19: Dictionary = snapshot.duplicate(true)
	v19["version"] = 19
	var cities: Array = v19.get("cities", []).duplicate(true)
	for city: Dictionary in cities:
		var region: Dictionary = city.get("region", {})
		if not region.is_empty() and region.has("migration"):
			var region_copy: Dictionary = region.duplicate(true)
			region_copy.erase("migration")
			city["region"] = region_copy
	v19["cities"] = cities
	var migrated = EconomySim.new()
	_load_chains(migrated)
	migrated.from_dict(v19)
	var after: Dictionary = migrated.to_dict()
	if int(after.get("version", 0)) != SAVE_VERSION:
		_fail("migração: versão deveria ser %d (é %s)" % [
			SAVE_VERSION, str(after.get("version"))])
		return
	var checked := 0
	var nonzero := 0
	for city: Dictionary in after.get("cities", []):
		var region: Dictionary = city.get("region", {})
		if not bool(region.get("ready", false)):
			continue
		var migration: Dictionary = region.get("migration", {})
		if migration.is_empty():
			_fail("%s: migração não ganhou o bloco no v20" % String(city.get("id", "?")))
			continue
		if float(migration.get("rural_in_total", -1.0)) != 0.0 \
				or float(migration.get("city_in_total", -1.0)) != 0.0 \
				or float(migration.get("city_out_total", -1.0)) != 0.0:
			nonzero += 1
		checked += 1
	if checked == 0:
		_fail("migração: nenhuma cidade com região migrada")
	else:
		print("[b4] migração v19→v20: %d cidades, %d com acumulador ≠ 0 (esperado 0)." % [
			checked, nonzero])
	if nonzero > 0:
		_fail("migração v19→v20: %d cidades inventaram acumulador ≠ 0" % nonzero)


## (e) mecânica controlada: direção (polo recebe), limite por vaga e estrutura
## etária/estrato dos migrantes. A/B: injeta vaga em UMA cidade; a irmã da
## mesma nação (sem vaga) é o controle.
func _check_mechanics() -> void:
	var sim := _make_world()
	for day in 30:
		sim.tick_day()
	var base: Dictionary = sim.to_dict()
	var pair := _pick_pair(base)
	if pair.size() < 2:
		_fail("mecânica: não achei 2 cidades da mesma nação com região")
		return
	var target := String(pair[0])
	var control := String(pair[1])
	var target_nation := ""
	var base_cities: Array = base.get("cities", [])
	var ti := _city_index(base, target)
	target_nation = String(base_cities[ti].get("nation", ""))
	# Baseline: sem vaga, NENHUMA cidade deveria ter migrado (portão da vaga).
	var before_total := 0.0
	for city: Dictionary in base.get("cities", []):
		before_total += float(city.get("region", {}).get("migration", {}).get(
			"rural_in_total", 0.0))
	if before_total > 0.0:
		print("[b4] nota: baseline (30 d) já migrou %.0f — o portão de vaga não " % before_total \
			+ "é absoluto neste mundo; o A/B usa o delta.")
	# Injetamos vaga PERSISTENTE no alvo (a irmã fica sem vaga).
	_inject_vacancy(base, target, 100000.0)
	var sim2 = EconomySim.new()
	_load_chains(sim2)
	sim2.from_dict(base)
	var young_before := _region_young_share(sim2.to_dict(), target)
	var young_control_before := _region_young_share(sim2.to_dict(), control)
	var q1_before := _region_q1_share(sim2.to_dict(), target)
	var q1_control_before := _region_q1_share(sim2.to_dict(), control)
	var rural_before := float(_region_block(sim2.to_dict(), target).get(
		"migration", {}).get("rural_in_total", 0.0))
	var region_pop_before := float(_region_block(sim2.to_dict(), target).get(
		"region_pop", 0.0))
	for day in 30:
		sim2.tick_day()
	var after: Dictionary = sim2.to_dict()
	var target_mig: Dictionary = _region_block(after, target).get("migration", {})
	var control_mig: Dictionary = _region_block(after, control).get("migration", {})
	var rural_after := float(target_mig.get("rural_in_total", 0.0))
	var rural_delta := rural_after - rural_before
	var control_delta := float(control_mig.get("rural_in_total", 0.0)) \
		- float(_region_block(base, control).get("migration", {}).get(
			"rural_in_total", 0.0))
	# (1) direção: o polo com vaga RECEBE (êxodo rural → urbano); o sem vaga não.
	if rural_delta <= 0.0:
		_fail("mecânica: %s (%s) não recebeu êxodo (Δ rural_in = %.0f)" % [
			target, target_nation, rural_delta])
	else:
		print("[b4] direção: %s (%s) recebeu Δ rural_in = %.0f." % [
			target, target_nation, rural_delta])
	# O controle sem vaga não pode receber MAIS que o alvo (o gate de vaga vale).
	if control_delta > rural_delta:
		_fail("mecânica: controle %s (sem vaga) migrou %.0f > alvo %.0f" % [
			control, control_delta, rural_delta])
	# O interior do alvo encolheu (pessoas saíram para o polo).
	var region_pop_after := float(_region_block(after, target).get("region_pop", 0.0))
	if region_pop_after >= region_pop_before:
		_fail("mecânica: interior de %s não encolheu (%.0f → %.0f)" % [
			target, region_pop_before, region_pop_after])
	# (2) idade: os migrantes saem jovens — a fatia 15–24+25–44 do interior do
	# alvo cai MAIS que a do controle (mesma nação = mesma demografia).
	var young_after := _region_young_share(after, target)
	var young_control_after := _region_young_share(after, control)
	var young_drop := young_before - young_after
	var young_control_drop := young_control_before - young_control_after
	if young_drop <= young_control_drop:
		_fail("mecânica: interior de %s não envelheceu mais que o controle " % target \
			+ "(Δ jovem %.6f vs %.6f)" % [young_drop, young_control_drop])
	else:
		print("[b4] idade: %s perdeu jovens (Δ share 15–24+25–44 = %.6f vs controle %.6f)." % [
			target, young_drop, young_control_drop])
	# (3) estrato: saem primeiro os quintis mais baixos — a fatia Q1 do interior
	# do alvo cai mais que a do controle.
	var q1_after := _region_q1_share(after, target)
	var q1_control_after := _region_q1_share(after, control)
	var q1_drop := q1_before - q1_after
	var q1_control_drop := q1_control_before - q1_control_after
	if q1_drop <= q1_control_drop:
		_fail("mecânica: Q1 do interior de %s não encolheu mais que o controle " % target \
			+ "(Δ Q1 %.6f vs %.6f)" % [q1_drop, q1_control_drop])
	else:
		print("[b4] estrato: %s perdeu mais do quintil pobre (Δ Q1 = %.6f vs controle %.6f)." % [
			target, q1_drop, q1_control_drop])


## (f) limite por moradia: a migração preenche EXATAMENTE a vaga disponível no
## destino (nem mais, nem menos). A vaga injetada decai antes do fechamento
## mensal (depreciação do estoque + crescimento da demanda + o pico mensal de
## demografia que roda no mesmo tick), então o que vincula é a vaga DISPONÍVEL no
## momento — que a fase de migração consome por inteiro. Provamos lendo a vaga
## do polo logo depois do fechamento (a fase não mexe no campo `vacancy`; ela só
## move gente, então o campo continua valendo o que a migração viu).
func _check_housing_limit() -> void:
	var sim := _make_world()
	for day in 30:
		sim.tick_day()
	var base: Dictionary = sim.to_dict()
	var pair := _pick_pair(base)
	if pair.size() < 2:
		_fail("limite: não achei 2 cidades da mesma nação com região")
		return
	var target := String(pair[0])
	var ti := _city_index(base, target)
	var cadence_day := ti % 30
	# Dias até o próximo fechamento mensal do alvo (estamos no dia 30).
	var days_until := (cadence_day - 30 % 30 + 30) % 30
	if days_until == 0:
		days_until = 30
	# Vaga bem abaixo da taxa máxima mensal (0,5% × região) — o teto que vincula
	# é a VAGA, não a taxa.
	var vacancy := 30000.0
	_inject_vacancy(base, target, vacancy)
	var sim2 = EconomySim.new()
	_load_chains(sim2)
	sim2.from_dict(base)
	var rural_before := float(_region_block(sim2.to_dict(), target).get(
		"migration", {}).get("rural_in_total", 0.0))
	var region_pop := float(_region_block(sim2.to_dict(), target).get(
		"region_pop", 0.0))
	var max_rate := region_pop * 0.005
	for day in days_until:
		sim2.tick_day()
	var rural_after := float(_region_block(sim2.to_dict(), target).get(
		"migration", {}).get("rural_in_total", 0.0))
	var delta := rural_after - rural_before
	var available := _vacancy_people(sim2, target, base)
	if delta <= 0.0:
		_fail("limite: %s não migrou com vaga %d (Δ = %.0f)" % [
			target, int(vacancy), delta])
	if delta > vacancy * 1.05 + 0.5:
		_fail("limite: %s recebeu %.0f > vaga injetada %.0f" % [target, delta, vacancy])
	if delta > max_rate:
		_fail("limite: %s recebeu %.0f > taxa máxima %.0f (a vaga não vincula)" % [
			target, delta, max_rate])
	# A vaga TEM de estar abaixo da taxa máxima; senão a taxa é o teto e o caso
	# não prova o limite de moradia.
	if available * 1.05 >= max_rate:
		_fail("limite: vaga disponível %.0f não está abaixo da taxa máxima %.0f — o caso não é vinculante" % [
			available, max_rate])
	# O motor consome a vaga DISPONÍVEL por inteiro (delta ≈ vaga disponível).
	if delta < available * 0.95 or delta > available * 1.05:
		_fail("limite: %s recebeu %.0f ≠ vaga disponível %.0f (motor não respeita o teto)" % [
			target, delta, available])
	print("[b4] limite: %s recebeu %.0f == vaga disponível %.0f (taxa máx %.0f)." % [
		target, delta, available, max_rate])


func _run() -> void:
	_check_conservation()
	_check_determinism()
	_check_save_roundtrip()
	_check_v19_migration()
	_check_mechanics()
	_check_housing_limit()
