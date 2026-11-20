extends SceneTree

## C3 (D-107) — BALANÇO POR GRUPO (empresa com balanço).
##
## Prova, no cenário de fixture (duas cidades, nação DEMO), SEM crédito:
##   1. `get_balance_status().world.groups[]` publica uma linha por grupo com
##      os campos do §5.2 do doc (caixa, estoque, capital fixo, dívida,
##      passivo, patrimônio líquido, alavancagem e a base do estoque).
##   2. A identidade do balanço fecha em cada grupo:
##      caixa + recebíveis + estoque + capital fixo + imóvel − passivo − dívida
##      = patrimônio líquido.
##   3. Aditividade declarada: os grupos são um SUBCONJUNTO das firmas da C1;
##      o caixa/estoque/capital fixo dos grupos nunca excede o total das firmas.
##   4. Sem crédito (default de jogo): dívida 0, alavancagem 0, banco sem
##      empréstimo/capital e `deposits: absent` — ausência declarada, não número
##      inventado.
##   5. `unallocated` publica o que não tem nação atribuível (leitura).
##
## Roda pelo runner: powershell -File tools/run_tests.ps1 -List c3_group_balance_test

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"

const DAYS := 40
const EPS := 1e-6

var _failures: Array[String] = []


func _init() -> void:
	var sim = _world(DAYS)
	_test_rows(sim)
	_test_identity(sim)
	_test_additivity(sim)
	_test_credit_off(sim)
	_test_unallocated(sim)
	if _failures.is_empty():
		print("[c3-balance] OK: balanço por grupo fecha, aditivo às firmas da C1 "
			+ "e crédito desligado declarado (dívida 0, depósito ausente).")
		quit(0)
	else:
		print("[c3-balance] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


func _test_rows(sim) -> void:
	var balance: Dictionary = sim.get_balance_status()
	var world: Dictionary = balance.get("world", {})
	var groups: Array = world.get("groups", [])
	var mirrored: Array = sim.get_groups()
	_check("uma linha por grupo", groups.size() == mirrored.size(),
		"balance=%d get_groups=%d" % [groups.size(), mirrored.size()])
	_check("há grupo no fixture", groups.size() > 0, "groups=%d" % groups.size())
	for row: Dictionary in groups:
		for key: String in ["id", "name", "origin_nation", "sector", "cash",
				"inventory", "fixed_capital", "payables", "debt", "net_worth",
				"daily_cost", "daily_profit", "building_count", "leverage",
				"inventory_basis"]:
			_check("%s tem %s" % [row.get("id"), key], row.has(key),
				"faltou " + key)
		var basis := String(row.get("inventory_basis", ""))
		if int(row.get("building_count", 0)) > 0:
			_check("%s declara a base do estoque" % row.get("id"),
				basis == "share of city market stock by capacity", basis)
		else:
			_check("%s sem prédio declara ausência" % row.get("id"),
				basis == "no_buildings", basis)


func _test_identity(sim) -> void:
	var groups: Array = sim.get_balance_status().get("world", {}).get(
		"groups", [])
	for row: Dictionary in groups:
		var assets := float(row.get("cash", 0.0)) \
			+ float(row.get("receivables", 0.0)) \
			+ float(row.get("inventory", 0.0)) \
			+ float(row.get("fixed_capital", 0.0)) \
			+ float(row.get("property", 0.0))
		var liabilities := float(row.get("payables", 0.0)) \
			+ float(row.get("debt", 0.0))
		var check := assets - liabilities
		var net := float(row.get("net_worth", 0.0))
		_check("%s: patrimônio = ativos − passivos" % row.get("id"),
			absf(check - net) <= maxf(EPS, absf(net) * 1e-9),
			"calculado=%.6f publicado=%.6f" % [check, net])


func _test_additivity(sim) -> void:
	var world: Dictionary = sim.get_balance_status().get("world", {})
	var firms: Dictionary = world.get("firms", {})
	var cash := 0.0
	var inventory := 0.0
	var fixed := 0.0
	for row: Dictionary in world.get("groups", []):
		cash += float(row.get("cash", 0.0))
		inventory += float(row.get("inventory", 0.0))
		fixed += float(row.get("fixed_capital", 0.0))
	var tolerance := maxf(1e-3, absf(float(firms.get("cash", 0.0))) * 1e-9)
	_check("caixa dos grupos ⊆ caixa das firmas",
		cash <= float(firms.get("cash", 0.0)) + tolerance,
		"grupos=%.3f firmas=%.3f" % [cash, float(firms.get("cash", 0.0))])
	_check("estoque dos grupos ⊆ estoque das firmas",
		inventory <= float(firms.get("inventory", 0.0))
			+ maxf(1e-3, absf(float(firms.get("inventory", 0.0))) * 1e-9),
		"grupos=%.3f firmas=%.3f" % [inventory,
			float(firms.get("inventory", 0.0))])
	_check("capital fixo dos grupos ⊆ capital fixo das firmas",
		fixed <= float(firms.get("fixed_capital", 0.0))
			+ maxf(1e-3, absf(float(firms.get("fixed_capital", 0.0))) * 1e-9),
		"grupos=%.3f firmas=%.3f" % [fixed,
			float(firms.get("fixed_capital", 0.0))])


func _test_credit_off(sim) -> void:
	var bank: Dictionary = sim.get_bank_status()
	var policy: Dictionary = bank.get("policy", {})
	var totals: Dictionary = bank.get("totals", {})
	_check("crédito nasce desligado", not bool(policy.get("enabled", true)),
		"enabled=" + str(policy.get("enabled")))
	_check("depósito declarado ausente (C4)",
		String(policy.get("deposits", "")) == "absent",
		String(policy.get("deposits", "")))
	_check("resolução declarada (C6)",
		String(policy.get("resolution", "")) == "C6",
		String(policy.get("resolution", "")))
	_check("fonte do default declarada",
		String(policy.get("source", "")).contains("bank_credit.json"),
		String(policy.get("source", "")))
	_check("sem banco: nenhuma nação publicada",
		(bank.get("nations", []) as Array).is_empty(),
		"nations=%d" % (bank.get("nations", []) as Array).size())
	_check("sem crédito: dívida dos grupos 0",
		absf(float(totals.get("group_debt", -1.0))) <= EPS,
		"group_debt=" + str(totals.get("group_debt")))
	_check("sem crédito: empréstimos/capital 0",
		absf(float(totals.get("loans", -1.0))) <= EPS
			and absf(float(totals.get("capital", -1.0))) <= EPS,
		"loans=%s capital=%s" % [totals.get("loans"), totals.get("capital")])
	for row: Dictionary in sim.get_balance_status().get("world", {}).get(
			"groups", []):
		_check("%s sem dívida e sem alavancagem" % row.get("id"),
			absf(float(row.get("debt", 0.0))) <= EPS
				and absf(float(row.get("leverage", 0.0))) <= EPS,
			"debt=%s leverage=%s" % [row.get("debt"), row.get("leverage")])
	var invariant: Dictionary = sim.get_balance_status().get("invariant", {})
	_check("identidade da C1 fecha sem crédito",
		bool(invariant.get("ok", false)), "financial_sum="
			+ str(invariant.get("financial_sum")))


func _test_unallocated(sim) -> void:
	var unallocated: Dictionary = sim.get_balance_status().get(
		"unallocated", {})
	for key: String in ["group_cash_without_origin", "construction_funds",
			"note"]:
		_check("unallocated publica " + key, unallocated.has(key),
			"faltou " + key)


func _check(name: String, ok: bool, detail: String) -> void:
	if not ok:
		_failures.append("%s (%s)" % [name, detail])


func _world(days: int):
	var sim = EconomySim.new()
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.set_services(_json(SERVICES_PATH))
	sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_json(AGE_STRUCTURE_PATH))
	sim.set_currencies({
		"regime": "peg",
		"currencies": [{
			"code": "DEM", "name": "Demo", "symbol": "D",
			"nations": ["DEMO"], "rate_per_anchor": 1.0, "rate": 1.0,
			"base_rate": 1.0,
		}],
	})
	sim.initialize_scenario()
	for day in days:
		sim.tick_day()
	return sim


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	return JSON.parse_string(file.get_as_text())
