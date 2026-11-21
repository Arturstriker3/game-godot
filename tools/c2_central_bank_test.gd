extends SceneTree

## C2a (D-103) — BANCO CENTRAL POR MOEDA: estado + medição.
##
## Prova, sem mudar o mundo:
##   1. INVARIANTE: a moeda do dia é integralmente atribuível às moedas do
##      mundo (ou declarada como não atribuível) e a soma dos M0 por moeda
##      reproduz a base monetária da C1 (`financial_sum` continua 0, `balance.ok`
##      continua verdadeiro em todo dia).
##   2. AGREGADOS: M1 = M2 = M0 sem banco (C4) — ausência declarada; sem
##      emissão nova.
##   3. CPI: o índice por país é o MESMO número do IPC do G2 (uma dona só) e
##      bate com a conta feita à mão pela cesta do `needs.json` nas nações de
##      uma cidade (os pesos cancelam). A inflação de 12 meses é lida da série
##      mensal; sem 12 meses, a ausência é declarada (`has_12m` falso).
##   4. POLÍTICA: os campos (regime, independência, padrão monetário, moeda de
##      reserva) viajam no save e NÃO agem — dois mundos com políticas
##      diferentes tiram o MESMO save sem os blocos de política depois de 1 dia.
##   5. OURO: a reserva do BC vem do dado real da G2/D-101 e a soma por moeda
##      fecha com o relatório do StorageService; o contrato da D-101 fica
##      intacto (o ouro não é semeado como bem físico).
##   6. SAVE v27: round-trip bit-exato, continuação idêntica e migração
##      v26 → v27 (defaults declarados; nada inventado para trás).
##
## Roda pelo runner: powershell -File tools/run_tests.ps1 -List c2_central_bank_test

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const LandPropertyScript := preload("res://core/economy/land_property.gd")
const AgricultureServiceScript := preload("res://core/economy/agriculture_service.gd")
const StorageServiceScript := preload("res://core/economy/storage_service.gd")
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
const INFORMALITY_PATH := "res://data/definitions/economy/informality.json"
const HEALTH_ACCESS_PATH := "res://data/definitions/population/health_access.json"
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

const SAVE_VERSION := 31
const EPS := 1e-3
const CPI_SAMPLE_DAYS := 30
const MODEL_YEAR_DAYS := 360

var _failures: Array[String] = []
var _base_price := {}
var _basket := {}


func _init() -> void:
	_base_price = _load_base_prices()
	_basket = _load_basket()
	_test_fixture()
	_test_world()
	if _failures.is_empty():
		print("[c2a] OK: banco central por moeda, M0/M1/M2, CPI x G2, "
			+ "política salva (sem mecânica), ouro do cofre, save v27.")
		quit(0)
	else:
		print("[c2a] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _check(cond: bool, message: String) -> void:
	if not cond:
		_fail(message)


func _close(a: float, b: float, tolerance: float) -> bool:
	return absf(a - b) <= tolerance


func _rel_close(a: float, b: float, rel: float = 1e-9) -> bool:
	return absf(a - b) <= maxf(EPS, rel * maxf(absf(a), absf(b)))


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_base_prices() -> Dictionary:
	var out := {}
	for entry: Dictionary in _json(GOODS_PATH):
		var id := String(entry.get("id", ""))
		if id.is_empty():
			continue
		# Mesma regra do set_goods: preço > 0 do catálogo, senão 40.0.
		var price := float(entry.get("base_price", 0.0))
		out[id] = price if price > 0.0 else 40.0
	return out


## Cesta alimentar do núcleo (set_needs): substitutos de food no nível
## básico × fator calórico de `equivalence.calories` limitado a [0,05, 4,0].
func _load_basket() -> Dictionary:
	var needs: Dictionary = _json(NEEDS_PATH)
	var substitutes: Array = ((needs.get("levels", {}) as Dictionary)
		.get("basic", {}) as Dictionary).get("categories", {}) \
		.get("food", {}).get("substitutes", [])
	var calories: Dictionary = (needs.get("equivalence", {}) as Dictionary) \
		.get("calories", {})
	var out := {}
	for id: Variant in substitutes:
		var product := String(id)
		var factor := float(calories.get(product, 1.0))
		out[product] = clampf(factor, 0.05, 4.0)
	return out


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


func _check_day(sim: Object, label: String) -> void:
	var status: Dictionary = sim.get_status()
	var balance: Dictionary = status.get("balance", {})
	_check(not balance.is_empty(), "%s: status sem `balance` (C1)" % label)
	_check(bool(balance.get("ok", false)), "%s: balance.ok falso" % label)
	_check(absf(float(balance.get("financial_sum", 1e9))) <= EPS,
		"%s: identidade financeira da C1 != 0" % label)
	var bank: Dictionary = sim.get_central_bank_status()
	var invariant: Dictionary = bank.get("invariant", {})
	_check(not invariant.is_empty(), "%s: banco sem bloco `invariant`" % label)
	_check(bool(invariant.get("ok", false)),
		"%s: moeda não atribuível às moedas (resíduo %.6f)"
			% [label, float(invariant.get("residual", 1e9))])
	_check(_close(float(invariant.get("allocated", 0.0))
			+ float(invariant.get("unallocated", 0.0)),
		float(invariant.get("money_total", 0.0)),
		maxf(EPS, 1e-9 * absf(float(invariant.get("money_total", 0.0))))),
		"%s: atribuído + não atribuído != money_total" % label)


## CPI de uma nação calculado À MÃO a partir do save (cesta do needs.json).
## Peso da cidade = necessidade da cidade (`pop.need`; a soma de
## `stratum_need` é o mesmo valor) e, sem ela, população de simulação ×
## `kNeedPerCapita` — a mesma régua do núcleo.
const NEED_PER_CAPITA := 0.0003

func _cpi_hand(save: Dictionary, nation: String) -> Dictionary:
	var products: Array = save.get("products", [])
	var weighted := 0.0
	var weight := 0.0
	var cities := 0
	var unweighted := 0
	for city: Dictionary in save.get("cities", []):
		if String(city.get("nation", "")) != nation:
			continue
		cities += 1
		var city_weight := float((city.get("pop", {}) as Dictionary)
			.get("need", 0.0))
		if city_weight <= 1e-12:
			city_weight = float(city.get("simulation_population", 0.0)) \
				* NEED_PER_CAPITA
		if city_weight <= 0.0:
			unweighted += 1
			continue
		var markets: Array = city.get("markets", [])
		var prices := {}
		for index in mini(markets.size(), products.size()):
			prices[String(products[index])] = float(
				(markets[index] as Dictionary).get("price", 0.0))
		var city_index := 0.0
		var city_factor := 0.0
		for product: String in _basket:
			if not prices.has(product):
				continue
			var price := float(prices[product])
			if price <= 0.0:
				continue
			if not _base_price.has(product) or float(_base_price[product]) <= 0.0:
				continue
			var factor := float(_basket[product])
			city_index += factor * (price / float(_base_price[product]))
			city_factor += factor
		if city_factor <= 0.0:
			continue
		weighted += city_weight * (city_index / city_factor)
		weight += city_weight
	var out := {"index": 0.0, "ok": false, "cities": float(cities),
		"weight": weight, "unweighted": float(unweighted)}
	if weight > 0.0:
		out["index"] = weighted / weight
		out["ok"] = true
	return out


func _test_fixture() -> void:
	# --- AGRIA/PORTUS: cenário de validação, 40 dias ----------------------
	var sim = EconomySim.new()
	_load_chains(sim)
	sim.initialize_scenario()
	var money0 := float(sim.get_status().get("money_total", 0.0))
	_check(money0 > 0.0, "(fixture) money_total deveria ser > 0")
	for day in 40:
		sim.tick_day()
		_check_day(sim, "fixture dia %d" % (day + 1))
		var money := float(sim.get_status().get("money_total", 0.0))
		_check(_close(money, money0, 1e-3),
			"(fixture) dinheiro não conservado no dia %d" % (day + 1))

	var bank: Dictionary = sim.get_central_bank_status()
	var policy: Dictionary = bank.get("monetary_policy", {})
	_check(String(policy.get("standard", "")) == "fiat",
		"(fixture) padrão monetário default deveria ser fiat (veio %s)"
			% String(policy.get("standard", "")))
	_check(String(policy.get("reserve_currency", "")) == "USD",
		"(fixture) moeda de reserva default deveria ser USD")
	# C2b1 (D-104): a política agora AGE (juro + emissão); esta suíte mantém a
	# medição da C2a e delega a mecânica a `tools/c2b1_monetary_test.gd`.
	_check(bool(policy.get("active", false)),
		"(fixture) C2b1: a política deveria estar ativa")
	# Nenhuma emissão nova: M0 = moeda em circulação; sem banco, M1 = M2 = M0.
	var totals: Dictionary = bank.get("totals", {})
	_check(_rel_close(float(totals.get("m0", -1.0)),
		float(bank.get("invariant", {}).get("allocated", -2.0)), 1e-12),
		"(fixture) M0 total != moeda atribuída")
	_check(_close(float(totals.get("m1", -1.0)), float(totals.get("m0", 0.0)),
		1e-9)
		and _close(float(totals.get("m2", -1.0)),
			float(totals.get("m0", 0.0)), 1e-9),
		"(fixture) sem banco (C4) M1 = M2 = M0")
	_check(_close(float(totals.get("bank_reserves", -1.0)), 0.0, 1e-12),
		"(fixture) reservas bancárias deveriam ser 0 (sem banco)")
	_check(_close(float(totals.get("securities", -1.0)), 0.0, 1e-12),
		"(fixture) títulos deveriam ser 0 (sem dívida pública)")
	# Mundo de cenário sem a G2: sem reserva de ouro declarada.
	var gold: Dictionary = bank.get("gold", {})
	_check(bool(gold.get("declared", true)) == false,
		"(fixture) sem G2 não há ouro declarado")
	_check(_close(float(totals.get("gold_tonnes", -1.0)), 0.0, 1e-12),
		"(fixture) ouro sem declaração deveria ser 0 t")

	# CPI por nação: conta à mão (cesta do needs.json × necessidade das cidades).
	var save: Dictionary = sim.to_dict()
	var cpi: Dictionary = bank.get("cpi", {})
	_check(not cpi.is_empty(), "(fixture) CPI ausente")
	for nation: String in cpi:
		var hand := _cpi_hand(save, nation)
		_check(bool(hand.get("ok", false)),
			"(fixture) CPI à mão sem preço de cesta em %s" % nation)
		if not bool(hand.get("ok", false)):
			continue
		_check(_rel_close(float((cpi[nation] as Dictionary).get("index", -1.0)),
			float(hand["index"]), 1e-9),
			"(fixture) CPI de %s != conta à mão (%.9f vs %.9f)"
				% [nation, float((cpi[nation] as Dictionary)
					.get("index", -1.0)), float(hand["index"])])
	# Sem 12 meses a inflação anual é AUSENTE, não um zero inventado.
	var first_nation := String(cpi.keys()[0])
	var row: Dictionary = cpi[first_nation]
	_check(int(row.get("observations", 0)) >= 2,
		"(fixture) série do CPI deveria ter >= 2 amostras em 40 d (tem %d)"
			% int(row.get("observations", 0)))
	_check(bool(row.get("has_12m", true)) == false,
		"(fixture) 40 d não pode ter inflação de 12 meses")
	_check(bool(row.get("has_annualized", false)),
		"(fixture) 2 amostras em 30 d já anualizam")

	# --- Save v27: round-trip bit-exato e continuação idêntica -----------
	_check(int(save.get("version", 0)) == SAVE_VERSION,
		"(fixture) save version %s != %d"
			% [str(save.get("version", 0)), SAVE_VERSION])
	var currencies: Array = save.get("currencies", [])
	_check(not currencies.is_empty(), "(fixture) save sem moedas")
	for entry: Dictionary in currencies:
		_check(entry.has("exchange_regime"),
			"(v27) moeda sem `exchange_regime`")
		_check(entry.has("bank_independence"),
			"(v27) moeda sem `bank_independence`")
	var copy = EconomySim.new()
	_load_chains(copy)
	copy.from_dict(save)
	_check(JSON.stringify(copy.to_dict(), "", true, true)
		== JSON.stringify(save, "", true, true),
		"(fixture) round-trip do save v27 não é bit-exato")
	for day in 3:
		sim.tick_day()
		copy.tick_day()
	_check(JSON.stringify(sim.to_dict(), "", true, true)
		== JSON.stringify(copy.to_dict(), "", true, true),
		"(fixture) continuação após load divergiu do caminho direto")
	_check_day(sim, "fixture pós round-trip")

	# --- Migração v26 → v27: defaults declarados, nada inventado ---------
	var old: Dictionary = save.duplicate(true)
	old["version"] = 26
	old.erase("monetary_policy")
	old.erase("central_bank")
	for entry: Dictionary in old.get("currencies", []):
		entry.erase("exchange_regime")
		entry.erase("bank_independence")
	var migrated = EconomySim.new()
	_load_chains(migrated)
	migrated.from_dict(old)
	var migrated_save: Dictionary = migrated.to_dict()
	_check(int(migrated_save.get("version", 0)) == SAVE_VERSION,
		"(migração) v26 deveria sair como v27 (saiu %s)"
			% str(migrated_save.get("version", 0)))
	var migrated_policy: Dictionary = migrated_save.get("monetary_policy", {})
	_check(String(migrated_policy.get("standard", "")) == "fiat",
		"(migração) padrão monetário deveria nascer fiat")
	_check(String(migrated_policy.get("reserve_currency", "")) == "USD",
		"(migração) moeda de reserva deveria nascer USD")
	var migrated_bank: Dictionary = migrated_save.get("central_bank", {})
	_check(bool(migrated_bank.get("gold_declared", true)) == false,
		"(migração) v26 não pode inventar ouro declarado")
	_check((migrated_bank.get("gold_tonnes_by_currency", {}) as Dictionary)
		.is_empty(), "(migração) v26 não pode inventar toneladas de ouro")
	_check((migrated_bank.get("cpi", {}) as Dictionary).is_empty(),
		"(migração) v26 não pode inventar série de CPI")
	_check(int(migrated_bank.get("cpi_last_day", 0)) == -1,
		"(migração) v26 não pode inventar o dia da última amostra")
	for entry: Dictionary in migrated_save.get("currencies", []):
		_check(String(entry.get("bank_independence", "")) == "no_data",
			"(migração) independência deveria nascer no_data")
		var regime := String(entry.get("exchange_regime", ""))
		var expected := "floating" if bool(entry.get("floating", false)) \
			else "peg"
		_check(regime == expected,
			"(migração) regime %s não nasce do `floating` (esperado %s)"
				% [regime, expected])
	migrated.tick_day()
	_check_day(migrated, "migração v26 -> v27")


func _make_world() -> Object:
	var sim = EconomySim.new()
	_load_chains(sim)
	sim.set_informality(_json(INFORMALITY_PATH))
	sim.set_health_access(_json(HEALTH_ACCESS_PATH))
	sim.set_sim_threads(1)
	var cities: Array = _json(CITIES_PATH)
	var ports: Array = _json(PORTS_PATH)
	var buildings: Array = _json(BUILDINGS_PATH)
	RegionDataScript.attach(cities)
	var geo := GeographyServiceScript.new()
	geo.load_data()
	buildings = geo.resolve_initial_farms(buildings, cities)
	if geo.has_method("resolve_initial_mines"):
		buildings = geo.resolve_initial_mines(buildings, cities)
	sim.initialize_world(cities, ports, buildings)
	GeographyServiceScript.apply_initial_infra(sim)
	if sim.has_method("set_urban"):
		sim.set_urban(_json(URBAN_LAND_PATH), _json(HOUSING_CITY_PATH),
			_json(HOUSING_PATH), _json(MUNICIPAL_FINANCE_PATH))
	var centers: Array = []
	for city: Dictionary in cities:
		centers.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)),
				float(city.get("y", 0.0))),
		})
	geo.apply_to_sim(sim, centers)
	_check(LandPropertyScript.apply(sim) > 0,
		"(mundo) LandProperty.apply deveria semear as regiões")
	if sim.has_method("set_crop_plan"):
		var agri := AgricultureServiceScript.new()
		agri.apply_to_sim(sim, cities, geo, {
			"calendar_nations": agri.calendar_nations(cities),
			"initial_stock": true,
			"farms": AgricultureServiceScript.farms_of(buildings),
		})
	sim.set_meta("storage_report", {})
	if sim.has_method("set_storage_capacity"):
		var storage := StorageServiceScript.new()
		var report: Dictionary = storage.apply_to_sim(sim, cities, buildings,
			{"only_missing": true, "initial_stock": true})
		sim.set_meta("storage_report", report)
	return sim


func _test_world() -> void:
	# --- Mundo real: 32 dias (2 amostras mensais do CPI) ------------------
	var sim = _make_world()
	if sim == null:
		return
	var money0 := float(sim.get_status().get("money_total", 0.0))
	var violations := 0
	for day in 32:
		var summary: Dictionary = sim.tick_day()
		violations += (summary.get("invariant_violations", []) as Array).size()
		if day % 15 == 14 or day == 31:
			_check_day(sim, "mundo dia %d" % (day + 1))
	_check(violations == 0, "(mundo) %d violações de invariante" % violations)
	_check(_close(float(sim.get_status().get("money_total", 0.0)), money0,
		maxf(EPS, 1e-9 * money0)),
		"(mundo) dinheiro não conservado em 32 d")

	var bank: Dictionary = sim.get_central_bank_status()
	var invariant: Dictionary = bank.get("invariant", {})
	var money_total := float(invariant.get("money_total", 0.0))
	_check(money_total > 1e6, "(mundo) money_total suspeito (%.4f)" % money_total)
	_check(float(invariant.get("allocated", 0.0)) > 0.9 * money_total,
		"(mundo) menos de 90%% da moeda atribuída a moedas (%.4f de %.4f)"
			% [float(invariant.get("allocated", 0.0)), money_total])
	_check(absf(float(invariant.get("residual", 1e9))) <= EPS,
		"(mundo) resíduo do M0 != 0")

	var currencies: Dictionary = bank.get("currencies", {})
	_check(currencies.size() > 100,
		"(mundo) esperava > 100 moedas, veio %d" % currencies.size())
	_check(currencies.has("USD"), "(mundo) falta a moeda USD")
	var m0_sum := 0.0
	var gold_sum := 0.0
	var gold_value_sum := 0.0
	for code: String in currencies:
		var row: Dictionary = currencies[code]
		var m0 := float(row.get("m0", -1.0))
		m0_sum += m0
		_check(m0 >= 0.0, "(mundo) %s: M0 negativo" % code)
		_check(_close(float(row.get("m1", -1.0)), m0, 1e-6)
			and _close(float(row.get("m2", -1.0)),
				float(row.get("m1", -2.0)), 1e-6),
			"(mundo) %s: sem banco (C4) M1 = M2 = M0" % code)
		_check(String(row.get("bank_independence", "")) == "no_data",
			"(mundo) %s: independência deveria ser no_data (ausência)"
				% code)
		var assets: Dictionary = row.get("assets", {})
		_check(_close(float(assets.get("securities", -1.0)), 0.0, 1e-12),
			"(mundo) %s: títulos 0 na C2a" % code)
		_check(_close(float(assets.get("reserve_currency_holdings", -1.0)),
			0.0, 1e-12),
			"(mundo) %s: carteira em moeda de reserva 0 na C2a" % code)
		gold_sum += float(assets.get("gold_tonnes", 0.0))
		gold_value_sum += float(assets.get("gold_value", 0.0))
	_check(_rel_close(m0_sum, float(invariant.get("allocated", -1.0)), 1e-9),
		"(mundo) soma dos M0 por moeda != moeda atribuída")
	var totals: Dictionary = bank.get("totals", {})
	_check(_close(float(totals.get("gold_tonnes", -1.0)), gold_sum, 1e-6),
		"(mundo) total de ouro != soma por moeda")
	_check(gold_value_sum > 0.0, "(mundo) ouro do BC sem valor")
	var gold_block: Dictionary = bank.get("gold", {})
	_check(bool(gold_block.get("declared", false)),
		"(mundo) a G2 aplicada deveria declarar o ouro do BC")
	_check(String(gold_block.get("price_basis", "")) == "mean_market_price",
		"(mundo) preço do ouro deveria vir do mercado (veio %s)"
			% String(gold_block.get("price_basis", "")))

	# G2: o ouro declarado no núcleo é o lido pelo StorageService.
	var report: Dictionary = sim.get_meta("storage_report", {})
	var gold_read := float(report.get("gold_reserve_t", -1.0))
	_check(gold_read > 8000.0,
		"(mundo) StorageService deveria ler o ouro real (%.1f t)" % gold_read)
	_check(_rel_close(gold_sum, gold_read, 1e-9),
		"(mundo) ouro no BC (%.3f t) != ouro lido da G2 (%.3f t)"
			% [gold_sum, gold_read])
	_check(_close(float(report.get("gold_central_bank_t", -1.0)), gold_read,
		1e-6),
		"(mundo) relatório da G2 deveria declarar o ouro no BC")
	_check(bool(report.get("gold_applied", false)) == true,
		"(mundo) C2b2/D-106: o ouro do cofre é físico (gold_applied)")

	# --- CPI: mesma dona do G2, conta à mão e série mensal --------------
	var cpi: Dictionary = bank.get("cpi", {})
	_check(cpi.size() > 100,
		"(mundo) esperava > 100 países no CPI, veio %d" % cpi.size())
	var storage_status: Dictionary = sim.get_storage_status()
	var storage_nations: Dictionary = storage_status.get("nations", {})
	var mismatches := 0
	for nation: String in cpi:
		var row: Dictionary = cpi[nation]
		var index := float(row.get("index", 0.0))
		_check(index > 0.2 and index < 5.0,
			"(mundo) %s: CPI fora do plausível (%.4f)" % [nation, index])
		if not storage_nations.has(nation):
			continue
		var g2 := float((storage_nations[nation] as Dictionary)
			.get("ipc", -1.0))
		if not _rel_close(index, g2, 1e-12):
			mismatches += 1
			if mismatches <= 3:
				_fail("(mundo) %s: CPI do BC %.9f != IPC do G2 %.9f"
					% [nation, index, g2])
	_check(mismatches == 0,
		"(mundo) %d países com CPI divergente do IPC do G2" % mismatches)

	var save: Dictionary = sim.to_dict()
	var hand_checked := 0
	for nation: String in cpi:
		var hand := _cpi_hand(save, nation)
		if not bool(hand.get("ok", false)) \
				or float(hand.get("unweighted", 1.0)) > 0.0:
			continue
		hand_checked += 1
		if not _rel_close(float((cpi[nation] as Dictionary)
				.get("index", -1.0)), float(hand["index"]), 1e-9):
			_fail("(mundo) %s: CPI != conta à mão (%.9f vs %.9f)"
				% [nation, float((cpi[nation] as Dictionary)
					.get("index", -1.0)), float(hand["index"])])
	_check(hand_checked > 100,
		"(mundo) poucas nações com conta à mão do CPI (%d)" % hand_checked)

	var totals_cpi: Dictionary = bank.get("cpi_totals", {})
	var cached: Dictionary = save.get("central_bank", {}).get("cpi", {})
	_check(int(totals_cpi.get("last_sample_day", -1)) > 0,
		"(mundo) série do CPI não foi amostrada")
	var samples_seen := 0
	for nation: String in cached:
		samples_seen = maxi(samples_seen, int(
			((cached[nation] as Dictionary).get("index", []) as Array).size()))
	_check(samples_seen >= 2,
		"(mundo) 32 d deveriam dar 2 amostras mensais (viu %d)" % samples_seen)
	# 2 meses ainda não fecham 12 meses: a inflação anual é declarada ausente.
	for nation: String in cpi:
		var row: Dictionary = cpi[nation]
		if int(row.get("observations", 0)) < 13:
			_check(bool(row.get("has_12m", true)) == false,
				"(mundo) %s: inflação de 12 m sem 13 amostras" % nation)
			_check(not row.has("inflation_12m"),
				"(mundo) %s: publicou inflação de 12 m sem série" % nation)

	# --- Série e inflação: leitura sobre série feita à mão --------------
	_check_inflation_reader(save)

	# --- Política salva e SEM mecânica ---------------------------------
	_check_policy_is_only_state(save)

	# --- Save v27: round-trip e continuação -----------------------------
	# Caminho do JOGO: o mundo é montado de novo (as definições, como os planos
	# de safra, não viajam no save) e o save entra por cima.
	var copy = _make_world()
	if copy == null:
		return
	copy.from_dict(save)
	var copy_save: Dictionary = copy.to_dict()
	# `pop.dependents` é DERIVADO no load (B1/v16: population − labor_force
	# recalculado depois de `apply_demography`) — característica anterior à C2a,
	# declarada aqui em vez de escondida. Todo o resto do save v27 tem de fechar
	# bit a bit (inclusive os blocos novos do banco central).
	var derived := _count_derived_diffs(save, copy_save)
	var direct_norm := _normalize_derived(save)
	var copy_norm := _normalize_derived(copy_save)
	_check(JSON.stringify(copy_norm, "", true, true)
		== JSON.stringify(direct_norm, "", true, true),
		"(mundo) round-trip do save v27 não é bit-exato: "
			+ _diff(direct_norm, copy_norm, "save"))
	print("[c2a] round-trip do mundo: derivados recalculados no load (B1, "
		+ "anterior à C2a): dependentes em %d cidades, estudantes em %d"
		% [int(derived[0]), int(derived[1])])
	for day in 3:
		sim.tick_day()
		copy.tick_day()
	var direct_save: Dictionary = sim.to_dict()
	var loaded_save: Dictionary = copy.to_dict()
	_check(JSON.stringify(direct_save, "", true, true)
		== JSON.stringify(loaded_save, "", true, true),
		"(mundo) continuação após load divergiu do caminho direto: "
			+ _diff(direct_save, loaded_save, "save"))
	_check_day(sim, "mundo pós round-trip")


## A inflação é LEITURA da série: monta séries conhecidas no save e confere a
## conta publicada (nada é recalculado fora da série).
func _check_inflation_reader(save: Dictionary) -> void:
	var probe: Dictionary = save.duplicate(true)
	var nation := "USA"
	var samples: Array = []
	var days: Array = []
	for month in 13:
		samples.append(1.0 + 0.1 * float(month) / 12.0)
		days.append(month * CPI_SAMPLE_DAYS)
	(probe.get("central_bank", {}) as Dictionary)["cpi"] = {
		nation: {"index": samples, "days": days},
	}
	(probe.get("central_bank", {}) as Dictionary)["cpi_last_day"] = 360
	var sim = EconomySim.new()
	_load_chains(sim)
	sim.from_dict(probe)
	var bank: Dictionary = sim.get_central_bank_status()
	var row: Dictionary = (bank.get("cpi", {}) as Dictionary).get(nation, {})
	_check(not row.is_empty(), "(inflação) %s ausente do CPI" % nation)
	_check(int(row.get("observations", 0)) == 13,
		"(inflação) esperava 13 amostras, veio %d"
			% int(row.get("observations", 0)))
	_check(int(row.get("span_days", 0)) == 12 * CPI_SAMPLE_DAYS,
		"(inflação) span de dias errado (%d)" % int(row.get("span_days", 0)))
	_check(bool(row.get("has_12m", false)),
		"(inflação) 13 amostras deveriam fechar 12 meses")
	_check(_rel_close(float(row.get("inflation_12m", -9.0)), 0.1, 1e-12),
		"(inflação) 12 m deveria ser exatamente 10%% (veio %.9f)"
			% float(row.get("inflation_12m", -9.0)))
	# Duas amostras: sem 12 meses, mas com anualização declarada.
	var short_probe: Dictionary = save.duplicate(true)
	(short_probe.get("central_bank", {}) as Dictionary)["cpi"] = {
		nation: {"index": [1.0, 1.1], "days": [0, CPI_SAMPLE_DAYS]},
	}
	(short_probe.get("central_bank", {}) as Dictionary)["cpi_last_day"] = 30
	var short_sim = EconomySim.new()
	_load_chains(short_sim)
	short_sim.from_dict(short_probe)
	var short_row: Dictionary = (short_sim.get_central_bank_status()
		.get("cpi", {}) as Dictionary).get(nation, {})
	_check(bool(short_row.get("has_12m", true)) == false,
		"(inflação) 2 amostras não podem fechar 12 meses")
	_check(bool(short_row.get("has_annualized", false)),
		"(inflação) 2 amostras deveriam anualizar")
	var expected := pow(1.1, float(MODEL_YEAR_DAYS) / float(CPI_SAMPLE_DAYS)) - 1.0
	_check(absf(float(short_row.get("inflation_annualized", -9.0)) - expected)
		<= 1e-9,
		"(inflação) anualizada %.9f != %.9f"
			% [float(short_row.get("inflation_annualized", -9.0)), expected])


## A política é ESTADO: viaja no save e não move um centavo. Dois mundos com
## políticas diferentes (carregados do mesmo save) tiram o MESMO estado sem os
## blocos de política depois de um dia.
func _check_policy_is_only_state(save: Dictionary) -> void:
	var base_sim = EconomySim.new()
	_load_chains(base_sim)
	base_sim.from_dict(save)
	base_sim.tick_day()

	var edited: Dictionary = save.duplicate(true)
	edited["monetary_policy"] = {"standard": "gold", "reserve_currency": "EUR"}
	var currencies: Array = edited.get("currencies", [])
	_check(not currencies.is_empty(), "(política) save sem moedas")
	if not currencies.is_empty():
		(currencies[0] as Dictionary)["exchange_regime"] = "band"
		(currencies[0] as Dictionary)["bank_independence"] = "controlled"
	(edited.get("central_bank", {}) as Dictionary)["gold_tonnes_by_currency"] = {
		"USD": 1234.5,
	}
	var edited_sim = EconomySim.new()
	_load_chains(edited_sim)
	edited_sim.from_dict(edited)
	edited_sim.tick_day()

	# 1. O estado carregado é o que foi salvo (e o default não foi sobrescrito).
	var status: Dictionary = edited_sim.get_central_bank_status()
	var policy: Dictionary = status.get("monetary_policy", {})
	_check(String(policy.get("standard", "")) == "gold",
		"(política) padrão monetário salvo não voltou (veio %s)"
			% String(policy.get("standard", "")))
	_check(String(policy.get("reserve_currency", "")) == "EUR",
		"(política) moeda de reserva salva não voltou")
	if not currencies.is_empty():
		var code := String((currencies[0] as Dictionary).get("code", ""))
		var row: Dictionary = (status.get("currencies", {}) as Dictionary) \
			.get(code, {})
		_check(String(row.get("regime", "")) == "band",
			"(política) regime da moeda não voltou (veio %s)"
				% String(row.get("regime", "")))
		_check(String(row.get("bank_independence", "")) == "controlled",
			"(política) independência da moeda não voltou")
	var usd: Dictionary = (status.get("currencies", {}) as Dictionary) \
		.get("USD", {})
	_check(_close(float((usd.get("assets", {}) as Dictionary)
		.get("gold_tonnes", -1.0)), 1234.5, 1e-9),
		"(política) ouro salvo não voltou para o BC do USD")

	# 2. Round-trip preserva a política.
	var round_trip: Dictionary = edited_sim.to_dict()
	_check(String((round_trip.get("monetary_policy", {}) as Dictionary)
		.get("standard", "")) == "gold",
		"(política) round-trip perdeu o padrão monetário")

	# 3. C2b1 (D-104): a política deixou de ser inerte — a independência
	# `controlled` muda juro/emissão por desenho. A identidade "dois mundos com
	# políticas diferentes têm o mesmo estado" da C2a é substituída pela prova
	# de que o estado de política viaja (acima) e pela mecânica em
	# `tools/c2b1_monetary_test.gd` (Taylor, passo, ΔM0 = emissão).


func _strip_policy(save: Dictionary) -> Dictionary:
	var out: Dictionary = save.duplicate(true)
	out.erase("monetary_policy")
	out.erase("central_bank")
	for entry: Dictionary in out.get("currencies", []):
		entry.erase("exchange_regime")
		entry.erase("bank_independence")
	return out



## Normaliza os campos DERIVADOS no load (B1 recalcula coortes → dependentes e
## estudantes). Característica anterior à C2a: não é medido aqui, só declarado.
func _normalize_derived(save: Dictionary) -> Dictionary:
	var out: Dictionary = save.duplicate(true)
	for city: Dictionary in out.get("cities", []):
		var pop: Dictionary = city.get("pop", {})
		if pop.is_empty():
			continue
		pop["dependents"] = 0.0
		var labor: Dictionary = pop.get("labor", {})
		if not labor.is_empty():
			labor["students"] = 0.0
	return out


## Quantas cidades tiveram os campos derivados recalculados no load.
func _count_derived_diffs(before: Dictionary, after: Dictionary) -> Array:
	var left: Array = before.get("cities", [])
	var right: Array = after.get("cities", [])
	var dependents := 0
	var students := 0
	for index in mini(left.size(), right.size()):
		var a_pop: Dictionary = (left[index] as Dictionary).get("pop", {})
		var b_pop: Dictionary = (right[index] as Dictionary).get("pop", {})
		var a_dep := float(a_pop.get("dependents", 0.0))
		var b_dep := float(b_pop.get("dependents", 0.0))
		if absf(a_dep - b_dep) > 1e-9:
			dependents += 1
		var a_stud := float((a_pop.get("labor", {}) as Dictionary)
			.get("students", 0.0))
		var b_stud := float((b_pop.get("labor", {}) as Dictionary)
			.get("students", 0.0))
		if absf(a_stud - b_stud) > 1e-9:
			students += 1
	return [dependents, students]


## Diagnóstico do round-trip: primeiro caminho em que dois saves divergem.
func _diff(a: Variant, b: Variant, path: String) -> String:
	if typeof(a) != typeof(b):
		return "%s: tipo %d != %d" % [path, typeof(a), typeof(b)]
	if a is Dictionary:
		var left: Dictionary = a
		var right: Dictionary = b
		if left.size() != right.size():
			return "%s: %d chaves != %d" % [path, left.size(), right.size()]
		for key: Variant in left.keys():
			if not right.has(key):
				return "%s.%s: ausente no segundo save" % [path, str(key)]
			var found := _diff(left[key], right[key], "%s.%s" % [path, str(key)])
			if not found.is_empty():
				return found
		return ""
	if a is Array:
		var left_list: Array = a
		var right_list: Array = b
		if left_list.size() != right_list.size():
			return "%s: tamanho %d != %d" % [path, left_list.size(),
				right_list.size()]
		for index in left_list.size():
			var found := _diff(left_list[index], right_list[index],
				"%s[%d]" % [path, index])
			if not found.is_empty():
				return found
		return ""
	if a != b:
		return "%s: %s != %s" % [path, str(a), str(b)]
	return ""
