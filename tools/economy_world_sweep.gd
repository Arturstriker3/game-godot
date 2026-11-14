extends SceneTree

## F5 — critério de aceite do preço na ÂNCORA (opção c, GAME_STATE §15).
##
## O preço interno é XGS e a moeda local é só exibição (`price_local`), então
## variações de TAXA não podem mudar trajetória: o sweep roda o MESMO teste de
## mundo em cinco variações e agrega o PIOR caso por nação. Taxas fixas
## (`currencies.json`, regime ausente/nulo) e sementes de jitter devem fechar
## idênticas ao XGS; qualquer queda numa delas seria vazamento da exibição.
##
##   1. taxas reais (currencies.json);
##   2. taxas reais + semente 1 (jitter determinístico de ±1e-9);
##   3. taxas reais + semente 2;
##   4. --fx=half (0,5 exato em binário: câmbio sem arredondamento);
##   5. --currencies=0 (mundo XGS pré-F5).
##
## Passa quando, em TODAS as variações, nenhuma nação piora a comida além de
## FOOD_TOLERANCE vs. o baseline (f4) e as falências ficam em baseline + 2.
## Escreve o relatório agregado (`--out=`) e um JSON por variação em
## `user://world_sweep/`.
##
##   godot --headless --path . --script res://tools/economy_world_sweep.gd
##     [--days=365] [--baseline=res://docs/economy/perf/f4_world_ai_365.json]
##     [--out=res://docs/economy/perf/f5_world_sweep.json]

const TEST_SCRIPT := "res://tools/economy_world_test.gd"
const DEFAULT_BASELINE := "res://docs/economy/perf/f4_world_ai_365.json"
const FOOD_TOLERANCE := 0.025
const BANKRUPTCY_TOLERANCE := 2
const VARIANTS := [
	{"name": "real", "args": ["--fx=real"]},
	{"name": "real_seed1", "args": ["--fx=real", "--seed=1"]},
	{"name": "real_seed2", "args": ["--fx=real", "--seed=2"]},
	{"name": "half", "args": ["--fx=half"]},
	{"name": "xgs", "args": ["--currencies=0"]},
]

var _days := 365
var _baseline_path := DEFAULT_BASELINE
var _out := ""
var _failures: Array[String] = []


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = maxi(1, int(arg.split("=")[1]))
		elif arg.begins_with("--baseline="):
			_baseline_path = arg.split("=", true, 1)[1]
		elif arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
	_sweep()
	if _failures.is_empty():
		print("[sweep] OK: %d variacoes em %d dias sem nacao acima da tolerancia." % [
			VARIANTS.size(), _days])
		quit(0)
	else:
		print("[sweep] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _sweep() -> void:
	var baseline: Variant = _load_json(_baseline_path)
	if typeof(baseline) != TYPE_DICTIONARY:
		print("[sweep] baseline ilegivel: " + _baseline_path)
		quit(2)
		return
	var baseline_nations: Dictionary = (baseline as Dictionary).get("nations", {})
	var baseline_bankruptcies := int((baseline as Dictionary).get("meta", {}).get(
		"bankruptcies", 0))
	var limit := baseline_bankruptcies + BANKRUPTCY_TOLERANCE
	var project := ProjectSettings.globalize_path("res://").trim_suffix("/").trim_suffix("\\")
	var godot := OS.get_executable_path()
	var sweep_dir := ProjectSettings.globalize_path("user://world_sweep")
	DirAccess.make_dir_recursive_absolute(sweep_dir)

	# pior queda por nação em todas as variações: {tag: {delta, food, variant}}
	var worst: Dictionary = {}
	var reports: Dictionary = {}
	var bankruptcies_by_variant: Dictionary = {}
	var failed_variants: Dictionary = {}
	for variant: Dictionary in VARIANTS:
		var name := String(variant["name"])
		var out_path := "user://world_sweep/%s_%d.json" % [name, _days]
		var args: Array = ["--headless", "--path", project, "--script", TEST_SCRIPT, "--",
			"--ai=1", "--days=%d" % _days, "--baseline=" + _baseline_path, "--out=" + out_path]
		args.append_array(variant["args"])
		var output: Array = []
		var code := OS.execute(godot, args, output, true)
		var report: Variant = _load_json(out_path)
		if typeof(report) != TYPE_DICTIONARY:
			_failures.append("%s: sem relatorio (%s)" % [name, out_path])
			failed_variants[name] = code
			continue
		reports[name] = report
		var data: Dictionary = report
		bankruptcies_by_variant[name] = int(data.get("meta", {}).get("bankruptcies", 0))
		if code != 0:
			failed_variants[name] = code
		_accumulate(name, data.get("nations", {}), baseline_nations, worst)

	# Relatório: nações que caíram em QUALQUER variação, ordenadas pela pior.
	var rows: Array = []
	for tag in worst:
		var entry: Dictionary = worst[tag]
		var before := float((baseline_nations.get(tag, {}) as Dictionary).get("food", 1.0))
		rows.append({
			"tag": tag,
			"baseline": before,
			"worst_food": float(entry["food"]),
			"delta": float(entry["delta"]),
			"variant": String(entry["variant"]),
		})
	rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["delta"]) < float(b["delta"]))
	print("[sweep] pior caso por nacao (variacoes: %s):" % _variant_names())
	for row: Dictionary in rows:
		print("  %-4s baseline=%.3f pior=%.3f delta=%+.3f (%s)" % [
			String(row["tag"]), float(row["baseline"]), float(row["worst_food"]),
			float(row["delta"]), String(row["variant"])])
		if float(row["delta"]) < -FOOD_TOLERANCE:
			_failures.append("%s: pior comida %.3f (baseline %.3f, %.3f) em %s" % [
				String(row["tag"]), float(row["worst_food"]), float(row["baseline"]),
				float(row["delta"]), String(row["variant"])])
	for name in bankruptcies_by_variant:
		var count := int(bankruptcies_by_variant[name])
		print("[sweep] %-10s falencias=%d (limite %d)" % [name, count, limit])
		if count > limit:
			_failures.append("%s: %d falencias > %d (baseline + %d)" % [
				name, count, limit, BANKRUPTCY_TOLERANCE])
	for name in failed_variants:
		if not reports.has(name):
			continue
		_failures.append("%s: teste de mundo retornou %d" % [name, int(failed_variants[name])])

	if _out != "":
		_write_report(rows, bankruptcies_by_variant, limit)


func _accumulate(variant: String, nations: Dictionary, baseline_nations: Dictionary,
		worst: Dictionary) -> void:
	for tag in nations:
		if not baseline_nations.has(tag):
			continue
		var before := float((baseline_nations[tag] as Dictionary).get("food", 1.0))
		var after := float((nations[tag] as Dictionary).get("food", 1.0))
		var delta := after - before
		if not worst.has(tag) or delta < float((worst[tag] as Dictionary)["delta"]):
			worst[tag] = {"delta": delta, "food": after, "variant": variant}


func _variant_names() -> String:
	var names: Array = []
	for variant: Dictionary in VARIANTS:
		names.append(String(variant["name"]))
	return ", ".join(names)


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _write_report(rows: Array, bankruptcies: Dictionary, limit: int) -> void:
	var dir := _out.get_base_dir()
	if dir != "" and not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var file := FileAccess.open(_out, FileAccess.WRITE)
	if file == null:
		_failures.append("nao abriu para escrita: " + _out)
		return
	file.store_string(JSON.stringify({
		"meta": {
			"days": _days,
			"baseline": _baseline_path,
			"food_tolerance": FOOD_TOLERANCE,
			"bankruptcy_limit": limit,
			"bankruptcies": bankruptcies,
			"variants": VARIANTS,
		},
		"nations": rows,
	}, "  "))
	file.close()
	print("[sweep] escrito %s" % _out)
