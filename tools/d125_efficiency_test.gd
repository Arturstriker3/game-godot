extends SceneTree

## D-125 — eficiência de produção (estilo HOI4).
##
## Guarda o contrato da spec aprovada em 23/11/2026
## (`specs/D-125_eficiencia_producao.md`) e dos defaults declarados em
## `data/definitions/production/efficiency.json`:
##   (1) rampa: fábrica nova 30% -> 90% em 90 d -> 100% em 180 d;
##   (2) troca de produto: 7 d sem produção e retenção 60% na mesma família
##       (goods.json.sector), reinício na inicial na família diferente;
##   (3) modos da refinaria: padrão 45/29/9, foco diesel 38/36/9, foco
##       querosene 40/28/15, com 3 d de transição;
##   (4) trator/máquina: sem equipamento o teto é 85%; com equipamento e diesel
##       100%; sem diesel o equipamento não acelera (teto 85%) e a eficiência
##       conquistada não cai;
##   (5) seed do mundo inicial nasce maduro (eficiência 1,0 em t=0);
##   (6) save v40 aditivo: campos por construção + bloco definitions.efficiency,
##       e save v39 carrega com as plantas MADURAS (migração v39->v40);
##   (7) determinismo: mesma sequência de comandos = mesmo save.
##
##   godot --headless --path . --script res://tools/d125_efficiency_test.gd

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const EFFICIENCY_PATH := "res://data/definitions/production/efficiency.json"

const EPS := 1e-6
## Defaults de balanceamento declarados em efficiency.json (não são dado
## observado: são regra do jogo, ajustável pelo dono).
const FACTORY_START := 0.30
const FACTORY_MID := 0.90
const FACTORY_MID_DAYS := 90
const FACTORY_FULL_DAYS := 180
const FARM_START := 0.20
const FARM_DAYS := 365
const EQUIPMENT_CAP_WITHOUT := 0.85
const TRANSITION_FACTORY := 7
const TRANSITION_MODE := 3
const RETENTION_SAME_FAMILY := 0.60
const REFINERY_SHARES := {
	"standard": {"gasoline": 0.45, "diesel": 0.29, "jet_fuel": 0.09},
	"diesel": {"gasoline": 0.38, "diesel": 0.36, "jet_fuel": 0.09},
	"jet": {"gasoline": 0.40, "diesel": 0.28, "jet_fuel": 0.15},
}

var _failures: Array[String] = []


func _init() -> void:
	_test_seed_mature()
	_test_factory_ramp()
	_test_product_change_and_retention()
	_test_refinery_modes()
	_test_ethanol_is_not_refinery()
	_test_equipment()
	_test_save_load_v40()
	_test_determinism()
	_test_fallback_matches_json()
	_finish()


# --- (5) mundo inicial nasce maduro -----------------------------------------
func _test_seed_mature() -> void:
	var sim := _world()
	var row := _row(sim, "AGRIA", "FARM-A-1")
	_check("(5) fazenda do ano-base nasce madura (eficiência 1,0)",
			absf(float(row.get("production_efficiency", -1.0)) - 1.0) <= EPS,
			"eficiência=%.4f" % float(row.get("production_efficiency", -1.0)))
	_check("(5) fazenda do ano-base nasce com trator (equipment 1,0)",
			absf(float(row.get("equipment", -1.0)) - 1.0) <= EPS,
			"equipment=%.4f" % float(row.get("equipment", -1.0)))


# --- (1) rampa da fábrica ---------------------------------------------------
func _test_factory_ramp() -> void:
	var sim := _world()
	sim.set_ai_enabled(false)
	sim.set_instant_construction(true)
	var flour := _product(sim, "flour")
	var built: Dictionary = sim.build("AGRIA", "D125-FACTORY", "wheat", -1,
			flour, 100.0, 20.0, 2.0, 10.0, 0.0, "private", 10000.0, "flour")
	if not bool(built.get("created", false)):
		_fail("(1) fábrica não foi construída: %s" % built)
		return
	var row := _row(sim, "AGRIA", "D125-FACTORY")
	_check("(1) fábrica nova começa em 30%",
			absf(float(row.get("production_efficiency", -1.0)) - FACTORY_START)
				<= EPS,
			"eficiência=%.4f" % float(row.get("production_efficiency", -1.0)))
	for _day in FACTORY_MID_DAYS:
		sim.tick_day()
	row = _row(sim, "AGRIA", "D125-FACTORY")
	_check("(1) fábrica chega a 90% em 90 d",
			absf(float(row.get("production_efficiency", -1.0)) - FACTORY_MID)
				<= 1e-4,
			"eficiência=%.4f" % float(row.get("production_efficiency", -1.0)))
	for _day in FACTORY_FULL_DAYS - FACTORY_MID_DAYS:
		sim.tick_day()
	row = _row(sim, "AGRIA", "D125-FACTORY")
	_check("(1) fábrica chega a 100% em 180 d",
			absf(float(row.get("production_efficiency", -1.0)) - 1.0) <= 1e-4,
			"eficiência=%.4f" % float(row.get("production_efficiency", -1.0)))


# --- (2) troca de produto, transição e retenção -----------------------------
func _test_product_change_and_retention() -> void:
	var sim := _world()
	sim.set_ai_enabled(false)
	sim.set_instant_construction(true)
	var flour := _product(sim, "flour")
	var built: Dictionary = sim.build("AGRIA", "D125-SWITCH", "wheat", -1,
			flour, 100.0, 20.0, 2.0, 10.0, 0.0, "private", 10000.0, "flour")
	if not bool(built.get("created", false)):
		_fail("(2) fábrica não foi construída: %s" % built)
		return
	for _day in FACTORY_MID_DAYS:
		sim.tick_day()
	var before := float(_row(sim, "AGRIA", "D125-SWITCH").get(
			"production_efficiency", 0.0))
	# Mesma família (food): guarda 60% da eficiência.
	var same: Dictionary = sim.change_product("AGRIA", "D125-SWITCH", "bread")
	_check("(2) troca de produto abre transição de 7 d (fábrica)",
			int(same.get("transition_days", -1)) == TRANSITION_FACTORY,
			"transição=%d" % int(same.get("transition_days", -1)))
	var expected_same := maxf(FACTORY_START, before * RETENTION_SAME_FAMILY)
	_check("(2) retenção de 60% na mesma família",
			absf(float(same.get("efficiency", -1.0)) - expected_same) <= 1e-4,
			"eficiência=%.4f esperada=%.4f" % [
				float(same.get("efficiency", -1.0)), expected_same])
	var row := _row(sim, "AGRIA", "D125-SWITCH")
	_check("(2) a planta fica em transição no status",
			int(row.get("transition_days", -1)) == TRANSITION_FACTORY,
			"transição=%d" % int(row.get("transition_days", -1)))
	# Durante a transição a produção é ZERO e a eficiência não cresce.
	var bread := _product(sim, "bread")
	var produced_before := _produced(sim.get_status(), bread)
	for _day in TRANSITION_FACTORY:
		sim.tick_day()
	var produced_after := _produced(sim.get_status(), bread)
	row = _row(sim, "AGRIA", "D125-SWITCH")
	_check("(2) transição sem produção de pão",
			absf(produced_after - produced_before) <= EPS,
			"produzido=%.6f" % (produced_after - produced_before))
	_check("(2) eficiência não cresce durante a transição",
			absf(float(row.get("production_efficiency", -1.0)) - expected_same)
				<= 1e-4,
			"eficiência=%.4f" % float(row.get("production_efficiency", -1.0)))
	_check("(2) transição termina em 0 d",
			int(row.get("transition_days", -1)) == 0,
			"transição=%d" % int(row.get("transition_days", -1)))
	# Família diferente (metal): reinicia na eficiência inicial da classe.
	var other: Dictionary = sim.change_product("AGRIA", "D125-SWITCH", "steel")
	_check("(2) família diferente reinicia na eficiência inicial (30%)",
			absf(float(other.get("efficiency", -1.0)) - FACTORY_START) <= 1e-4,
			"eficiência=%.4f" % float(other.get("efficiency", -1.0)))
	_check("(2) troca de família também abre 7 d de transição",
			int(other.get("transition_days", -1)) == TRANSITION_FACTORY,
			"transição=%d" % int(other.get("transition_days", -1)))
	# Mina não troca de produto: a jazida manda (API recusa).
	sim.set_instant_construction(true)
	var iron := _product(sim, "iron")
	var mine: Dictionary = sim.build("AGRIA", "D125-MINE", "iron", -1, iron,
			100.0, 20.0, 2.0, 10.0, 0.0, "private", 10000.0, "")
	if bool(mine.get("created", false)):
		var refused: Dictionary = sim.change_product("AGRIA", "D125-MINE",
				"steel")
		_check("(2) mina não troca de produto (API recusa)",
				not bool(refused.get("changed", true))
					and not String(refused.get("error", "")).is_empty(),
				"%s" % refused)


# --- (3) modos da refinaria -------------------------------------------------
func _test_refinery_modes() -> void:
	# Cada modo em um mundo novo: o bootstrap só oferta enquanto o estoque está
	# abaixo do alvo, então zeramos as saídas antes do dia medido.
	_check_refinery_fraction("standard")
	_check_refinery_fraction("diesel")
	_check_refinery_fraction("jet")
	# Transição do modo: 3 d sem produção.
	var sim := _world()
	sim.set_ai_enabled(false)
	sim.set_instant_construction(true)
	if not _build_refinery(sim):
		return
	var switched: Dictionary = sim.set_refinery_mode("AGRIA", "D125-REF",
			"diesel")
	_check("(3) troca de modo abre 3 d de transição",
			int(switched.get("transition_days", -1)) == TRANSITION_MODE,
			"transição=%d" % int(switched.get("transition_days", -1)))
	var gasoline := _product(sim, "gasoline")
	var before := _produced(sim.get_status(), gasoline)
	for _day in TRANSITION_MODE:
		sim.tick_day()
	var after := _produced(sim.get_status(), gasoline)
	_check("(3) troca de modo sem produção durante a transição",
			absf(after - before) <= EPS, "produzido=%.6f" % (after - before))


## (3) `gasoline_b` é o etanol (`chemical_plant`, insumo `ethanol`), não o
## refino de petróleo: o modo da refinaria não pode valer para ela. O prefixo
## do id (`gasoline_`) não é assinatura do processo.
func _test_ethanol_is_not_refinery() -> void:
	var sim := _world()
	sim.set_ai_enabled(false)
	sim.set_instant_construction(true)
	var gasoline := _product(sim, "gasoline")
	sim.set_market_stock("AGRIA", "ethanol", 5000.0)
	var built: Dictionary = sim.build("AGRIA", "D125-ETHANOL", "ethanol", -1,
			gasoline, 200.0, 100.0, 2.0, 12.0, 0.0, "private", 10000.0,
			"gasoline_b")
	if not bool(built.get("created", false)):
		_fail("(3) planta de etanol não foi construída: %s" % built)
		return
	var refused: Dictionary = sim.set_refinery_mode("AGRIA", "D125-ETHANOL",
			"diesel")
	_check("(3) gasoline_b (etanol) recusa o modo de refinaria",
			not bool(refused.get("changed", true))
				and not String(refused.get("error", "")).is_empty(),
			"%s" % refused)
	var row := _row(sim, "AGRIA", "D125-ETHANOL")
	_check("(3) gasoline_b fica no modo 0 e sem transição",
			int(row.get("refinery_mode", -1)) == 0
				and int(row.get("transition_days", -1)) == 0,
			"modo=%d transição=%d" % [int(row.get("refinery_mode", -1)),
				int(row.get("transition_days", -1))])
	# O lote continua o 0,80 da receita (sem as frações de refino).
	for good: String in ["gasoline", "diesel", "jet_fuel"]:
		sim.set_market_stock("AGRIA", good, 0.0)
	var before := {
		"gasoline": _produced(sim.get_status(), gasoline),
		"diesel": _produced(sim.get_status(), _product(sim, "diesel")),
		"jet_fuel": _produced(sim.get_status(), _product(sim, "jet_fuel")),
	}
	sim.tick_day()
	var made := {
		"gasoline": _produced(sim.get_status(), gasoline)
			- float(before["gasoline"]),
		"diesel": _produced(sim.get_status(), _product(sim, "diesel"))
			- float(before["diesel"]),
		"jet_fuel": _produced(sim.get_status(), _product(sim, "jet_fuel"))
			- float(before["jet_fuel"]),
	}
	_check("(3) gasoline_b produz gasolina e nenhum co-produto de refino",
			float(made["gasoline"]) > EPS
				and absf(float(made["diesel"])) <= EPS
				and absf(float(made["jet_fuel"])) <= EPS,
			"%s" % made)


func _check_refinery_fraction(mode: String) -> void:
	var sim := _world()
	sim.set_ai_enabled(false)
	sim.set_instant_construction(true)
	if not _build_refinery(sim):
		return
	if mode != "standard":
		var switched: Dictionary = sim.set_refinery_mode("AGRIA", "D125-REF",
				mode)
		if int(switched.get("transition_days", -1)) != TRANSITION_MODE:
			_fail("(3) modo %s não abriu transição: %s" % [mode, switched])
			return
		for _day in TRANSITION_MODE:
			sim.tick_day()
	# Zera as saídas para o dia medido (o alvo de bootstrap é o estoque baixo).
	for good: String in ["gasoline", "diesel", "jet_fuel"]:
		sim.set_market_stock("AGRIA", good, 0.0)
	var before := {
		"gasoline": _produced(sim.get_status(), _product(sim, "gasoline")),
		"diesel": _produced(sim.get_status(), _product(sim, "diesel")),
		"jet_fuel": _produced(sim.get_status(), _product(sim, "jet_fuel")),
	}
	sim.tick_day()
	var made := {
		"gasoline": _produced(sim.get_status(), _product(sim, "gasoline"))
			- float(before["gasoline"]),
		"diesel": _produced(sim.get_status(), _product(sim, "diesel"))
			- float(before["diesel"]),
		"jet_fuel": _produced(sim.get_status(), _product(sim, "jet_fuel"))
			- float(before["jet_fuel"]),
	}
	var shares: Dictionary = REFINERY_SHARES[mode]
	var gas := float(made["gasoline"])
	if gas <= EPS:
		_fail("(3) modo %s não produziu gasolina: %s" % [mode, made])
		return
	_check("(3) modo %s: diesel = %.2f/%.2f da gasolina" % [
			mode, float(shares["diesel"]), float(shares["gasoline"])],
			absf(float(made["diesel"]) - gas
				* float(shares["diesel"]) / float(shares["gasoline"]))
				<= 1e-4 * maxf(1.0, gas), "%s" % made)
	_check("(3) modo %s: querosene = %.2f/%.2f da gasolina" % [
			mode, float(shares["jet_fuel"]), float(shares["gasoline"])],
			absf(float(made["jet_fuel"]) - gas
				* float(shares["jet_fuel"]) / float(shares["gasoline"]))
				<= 1e-4 * maxf(1.0, gas), "%s" % made)


# --- (4) trator / máquina de mineração --------------------------------------
func _test_equipment() -> void:
	# Sem equipamento: teto de maturação 85%.
	var plain := _farm_world()
	for _day in FARM_DAYS:
		plain.tick_day()
	var row := _row(plain, "AGRIA", "D125-FARM")
	_check("(4) sem equipamento a fazenda para em 85%",
			absf(float(row.get("production_efficiency", -1.0))
				- EQUIPMENT_CAP_WITHOUT) <= 1e-4,
			"eficiência=%.4f" % float(row.get("production_efficiency", -1.0)))
	# Com equipamento e diesel: chega a 100% (crescimento acelerado).
	var powered := _farm_world()
	powered.set_market_stock("AGRIA", "diesel", 1000.0)
	powered.set_building_equipment("AGRIA", "D125-FARM", 1.0)
	for _day in FARM_DAYS:
		powered.tick_day()
	row = _row(powered, "AGRIA", "D125-FARM")
	_check("(4) com trator e diesel a fazenda chega a 100%",
			absf(float(row.get("production_efficiency", -1.0)) - 1.0) <= 1e-4,
			"eficiência=%.4f" % float(row.get("production_efficiency", -1.0)))
	# Trator sem diesel fica parado: não acelera (teto 85%) e não consome.
	var dry := _farm_world()
	dry.set_building_equipment("AGRIA", "D125-FARM", 1.0)
	for _day in FARM_DAYS:
		dry.tick_day()
	row = _row(dry, "AGRIA", "D125-FARM")
	_check("(4) trator sem diesel não acelera (teto 85%)",
			absf(float(row.get("production_efficiency", -1.0))
				- EQUIPMENT_CAP_WITHOUT) <= 1e-4,
			"eficiência=%.4f" % float(row.get("production_efficiency", -1.0)))
	_check("(4) o trator sem diesel não consome o bem",
			_market_stock(dry, "AGRIA", "diesel") <= EPS,
			"estoque=%.6f" % _market_stock(dry, "AGRIA", "diesel"))
	# A eficiência conquistada não cai quando o trator para (madura + sem
	# diesel continua 1,0).
	var mature := _world()
	mature.set_market_stock("AGRIA", "diesel", 0.0)
	for _day in 3:
		mature.tick_day()
	var seed_row := _row(mature, "AGRIA", "FARM-A-1")
	_check("(4) eficiência madura não cai com o trator parado",
			absf(float(seed_row.get("production_efficiency", -1.0)) - 1.0)
				<= 1e-4,
			"eficiência=%.4f" % float(
				seed_row.get("production_efficiency", -1.0)))


# --- (6) save v40 e migração v39 -> v40 -------------------------------------
func _test_save_load_v40() -> void:
	var sim := _world()
	sim.set_ai_enabled(false)
	sim.set_instant_construction(true)
	if not _build_refinery(sim):
		return
	sim.set_refinery_mode("AGRIA", "D125-REF", "diesel")
	for _day in 5:
		sim.tick_day()
	var save: Dictionary = sim.to_dict()
	_check("(6) save sai na versão 40 ou posterior",
			int(save.get("version", -1)) >= 40,
			"version=%d" % int(save.get("version", -1)))
	# O bloco estático viaja no snapshot (mesmo `definitions` do CLI); o save de
	# estado carrega só a parte dinâmica, como os demais sistemas.
	var snapshot: Dictionary = sim.to_snapshot()
	var definitions: Dictionary = snapshot.get("definitions", {})
	_check("(6) snapshot guarda definitions.efficiency",
			definitions.has("efficiency"), "chaves=%s" % [
				definitions.keys()])
	var entry := _save_building(save, "D125-REF")
	_check("(6) save guarda os campos da eficiência por construção",
			entry.has("production_efficiency") and entry.has("efficiency_days")
				and entry.has("transition_days") and entry.has("equipment")
				and entry.has("refinery_mode"),
			"chaves=%s" % [entry.keys()])
	var loaded := _load(save)
	var loaded_row := _row(loaded, "AGRIA", "D125-REF")
	_check("(6) load v40 preserva modo e eficiência",
			int(loaded_row.get("refinery_mode", -1)) == 1
				and absf(float(loaded_row.get("production_efficiency", -1.0))
					- float(entry.get("production_efficiency", -2.0))) <= EPS,
			"modo=%d eficiência=%.4f" % [
				int(loaded_row.get("refinery_mode", -1)),
				float(loaded_row.get("production_efficiency", -1.0))])
	# Migração v39 -> v40: sem as chaves, o estoque do ano-base carrega MADURO.
	var legacy: Dictionary = save.duplicate(true)
	legacy["version"] = 39
	for row: Variant in legacy.get("buildings", []):
		var building: Dictionary = row
		for key: String in ["production_efficiency", "efficiency_days",
				"transition_days", "equipment", "refinery_mode"]:
			building.erase(key)
	(legacy.get("definitions", {}) as Dictionary).erase("efficiency")
	var migrated := _load(legacy)
	var migrated_row := _row(migrated, "AGRIA", "D125-REF")
	_check("(6) save v39 carrega com as plantas maduras",
			absf(float(migrated_row.get("production_efficiency", -1.0)) - 1.0)
				<= EPS
				and absf(float(migrated_row.get("equipment", -1.0)) - 1.0)
					<= EPS,
			"eficiência=%.4f equipment=%.4f" % [
				float(migrated_row.get("production_efficiency", -1.0)),
				float(migrated_row.get("equipment", -1.0))])


# --- (7) determinismo -------------------------------------------------------
func _test_determinism() -> void:
	var first := _deterministic_run()
	var second := _deterministic_run()
	_check("(7) mesma sequência de comandos = mesmo save",
			first == second, "primeiro=%s segundo=%s" % [first, second])


# --- (8) espelho compilado == JSON -----------------------------------------
## Se o efficiency.json não carregar, o núcleo usa o espelho compilado. O
## espelho tem de ser EXATAMENTE o arquivo — senão o mesmo save roda diferente
## conforme o arquivo esteja legível.
func _test_fallback_matches_json() -> void:
	var json: Dictionary = _json(EFFICIENCY_PATH)
	var sim = EconomySim.new()
	sim.set_sim_threads(1)
	sim.set_goods(_json(GOODS_PATH))
	if not sim.has_method("get_efficiency_params"):
		_fail("(8) binário sem get_efficiency_params")
		return
	var got: Dictionary = sim.get_efficiency_params()
	for cls in ["factory", "refinery", "mine", "farm", "ranch"]:
		var want: Dictionary = json.get(cls, {})
		var row: Dictionary = got.get(cls, {})
		for field in ["start", "mid", "mid_days", "full_days"]:
			_check("(8) fallback %s.%s == JSON" % [cls, field],
					absf(float(row.get(field, -999.0))
						- float(want.get(field, -999.0))) <= EPS,
					"fallback=%s json=%s" % [row.get(field), want.get(field)])
	var equipment: Dictionary = json.get("equipment", {})
	_check("(8) fallback equipment.cap_without == JSON",
			absf(float(got.get("equipment_cap_without", -1.0))
				- float(equipment.get("cap_without", -2.0))) <= EPS,
			"fallback=%s json=%s" % [got.get("equipment_cap_without"),
				equipment.get("cap_without")])
	_check("(8) fallback equipment.rate_boost == JSON",
			absf(float(got.get("equipment_rate_boost", -1.0))
				- float(equipment.get("rate_boost", -2.0))) <= EPS,
			"fallback=%s json=%s" % [got.get("equipment_rate_boost"),
				equipment.get("rate_boost")])
	_check("(8) fallback equipment.fuel_per_capacity_day == JSON",
			absf(float(got.get("equipment_fuel_per_capacity_day", -1.0))
				- float(equipment.get("fuel_per_capacity_day", -2.0))) <= EPS,
			"fallback=%s json=%s" % [got.get("equipment_fuel_per_capacity_day"),
				equipment.get("fuel_per_capacity_day")])
	var transitions: Dictionary = json.get("transitions", {})
	for pair in [["factory_days", "transition_factory_days"],
			["refinery_days", "transition_refinery_days"],
			["military_days", "transition_military_days"],
			["semiconductor_days", "transition_semiconductor_days"],
			["mode_days", "transition_mode_days"]]:
		_check("(8) fallback transitions.%s == JSON" % pair[0],
				int(got.get(pair[1], -1)) == int(transitions.get(pair[0], -2)),
				"fallback=%s json=%s" % [got.get(pair[1]),
					transitions.get(pair[0])])
	var retention: Dictionary = json.get("retention", {})
	_check("(8) fallback retention.same_family == JSON",
			absf(float(got.get("retention_same_family", -1.0))
				- float(retention.get("same_family", -2.0))) <= EPS,
			"fallback=%s json=%s" % [got.get("retention_same_family"),
				retention.get("same_family")])
	_check("(8) fallback retention.other == JSON",
			absf(float(got.get("retention_other", -1.0))
				- float(retention.get("other", -2.0))) <= EPS,
			"fallback=%s json=%s" % [got.get("retention_other"),
				retention.get("other")])
	var ai: Dictionary = json.get("ai", {})
	_check("(8) fallback ai.mode_payback_days == JSON",
			int(got.get("ai_mode_payback_days", -1))
				== int(ai.get("mode_payback_days", -2)),
			"fallback=%s json=%s" % [got.get("ai_mode_payback_days"),
				ai.get("mode_payback_days")])
	_check("(8) fallback ai.mode_cooldown_days == JSON",
			int(got.get("ai_mode_cooldown_days", -1))
				== int(ai.get("mode_cooldown_days", -2)),
			"fallback=%s json=%s" % [got.get("ai_mode_cooldown_days"),
				ai.get("mode_cooldown_days")])
	_check("(8) fallback ai.mode_min_gain == JSON",
			absf(float(got.get("ai_mode_min_gain", -1.0))
				- float(ai.get("mode_min_gain", -2.0))) <= EPS,
			"fallback=%s json=%s" % [got.get("ai_mode_min_gain"),
				ai.get("mode_min_gain")])
	# Modos da refinaria: mesma matriz 3×3 do JSON, na ordem `order`
	# (standard, diesel, jet) × (gasoline, diesel, jet_fuel).
	var modes: Dictionary = json.get("refinery_modes", {})
	var order: Array = modes.get("order", [])
	var got_modes: Array = got.get("mode_shares", [])
	_check("(8) fallback mode_shares tem 3 modos",
			got_modes.size() == 3 and order.size() == 3,
			"fallback=%d json=%d" % [got_modes.size(), order.size()])
	if got_modes.size() == 3 and order.size() == 3:
		var goods: Array = ["gasoline", "diesel", "jet_fuel"]
		for mode in 3:
			var row: Dictionary = modes.get(String(order[mode]), {})
			var got_row: Array = got_modes[mode]
			for slot in 3:
				var good := String(goods[slot])
				_check("(8) fallback mode_shares.%s.%s == JSON" % [
						String(order[mode]), good],
						absf(float(got_row[slot])
							- float(row.get(good, -2.0))) <= EPS,
						"fallback=%s json=%s" % [got_row[slot],
							row.get(good)])


func _deterministic_run() -> String:
	var sim := _world()
	sim.set_ai_enabled(false)
	sim.set_instant_construction(true)
	if not _build_refinery(sim):
		return "erro"
	sim.set_refinery_mode("AGRIA", "D125-REF", "jet")
	for _day in 10:
		sim.tick_day()
	return JSON.stringify(sim.to_dict(), "", true, true)


# --- helpers ---------------------------------------------------------------
func _world() -> Object:
	var sim = EconomySim.new()
	sim.set_sim_threads(1)
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	var params: Variant = _json(EFFICIENCY_PATH)
	if params is Dictionary and sim.has_method("set_efficiency_params"):
		sim.set_efficiency_params(params)
	sim.initialize_scenario()
	return sim


func _farm_world() -> Object:
	var sim := _world()
	sim.set_ai_enabled(false)
	sim.set_instant_construction(true)
	var wheat := _product(sim, "wheat")
	var built: Dictionary = sim.build("AGRIA", "D125-FARM", "wheat", -1,
			wheat, 100.0, 20.0, 2.0, 10.0, 0.0, "private", 10000.0, "")
	if not bool(built.get("created", false)):
		_fail("(4) fazenda não foi construída: %s" % built)
	return sim


func _build_refinery(sim: Object) -> bool:
	var gasoline := _product(sim, "gasoline")
	if gasoline < 0:
		_fail("(3) goods.json sem gasoline")
		return false
	sim.set_market_stock("AGRIA", "oil", 5000.0)
	var built: Dictionary = sim.build("AGRIA", "D125-REF", "oil", -1,
			gasoline, 500.0, 200.0, 2.0, 12.0, 0.0, "private", 10000.0,
			"gasoline_a")
	if not bool(built.get("created", false)):
		_fail("(3) refinaria não foi construída: %s" % built)
		return false
	return true


func _load(save: Dictionary) -> Object:
	var sim = EconomySim.new()
	sim.set_sim_threads(1)
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.from_dict(save)
	return sim


func _row(sim: Object, city_id: String, building_id: String) -> Dictionary:
	for row: Variant in sim.get_city_status(city_id).get("buildings", []):
		var building: Dictionary = row
		if String(building.get("id", "")) == building_id:
			return building
	return {}


func _save_building(save: Dictionary, building_id: String) -> Dictionary:
	for row: Variant in save.get("buildings", []):
		var building: Dictionary = row
		if String(building.get("id", "")) == building_id:
			return building
	return {}


func _product(sim: Object, product: String) -> int:
	return Array(sim.get_status().get("products", [])).find(product)


func _produced(status: Dictionary, index: int) -> float:
	var produced: Array = status.get("produced_total", [])
	if index < 0 or index >= produced.size():
		return 0.0
	return float(produced[index])


func _market_stock(sim: Object, city_id: String, product: String) -> float:
	var index := _product(sim, product)
	var markets: Array = sim.get_city_status(city_id).get("markets", [])
	if index < 0 or index >= markets.size():
		return 0.0
	return float((markets[index] as Dictionary).get("stock", 0.0))


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _check(name: String, ok: bool, detail: String = "") -> void:
	if ok:
		print("[d125] ok: " + name)
	else:
		_fail("%s (%s)" % [name, detail])


func _fail(message: String) -> void:
	_failures.append(message)


func _finish() -> void:
	if _failures.is_empty():
		print("[d125] OK: rampa, troca de produto/retensão, modos da refinaria, "
			+ "trator/máquina, seed maduro, save v40 e determinismo.")
		quit(0)
	else:
		print("[d125] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)
