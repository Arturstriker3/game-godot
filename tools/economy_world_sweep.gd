extends SceneTree

const TmpPaths := preload("res://tools/tmp_paths.gd")

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
## `$OPENGS_TMP/world_sweep/` (default `G:/opengs_tmp/world_sweep/`).
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
	var baseline_nations: Dictionary = _normalize_nations(
		(baseline as Dictionary).get("nations", {}))
	# F9 (`f9_invest_world_sweep.json`) grava `meta.bankruptcies` como DICIONÁRIO
	# por variação e `meta.bankruptcy_limit`; f4 grava como int. Normaliza para
	# manter o teto de falências de construções correto (28 no caso da F9).
	var meta: Dictionary = (baseline as Dictionary).get("meta", {})
	var baseline_bankruptcies := 0
	if typeof(meta.get("bankruptcy_limit", 0)) == TYPE_INT:
		baseline_bankruptcies = int(meta["bankruptcy_limit"]) - BANKRUPTCY_TOLERANCE
	elif typeof(meta.get("bankruptcies", 0)) == TYPE_INT:
		baseline_bankruptcies = int(meta["bankruptcies"])
	elif typeof(meta.get("bankruptcies", 0)) == TYPE_DICTIONARY:
		var per_variant: Dictionary = meta["bankruptcies"]
		for key in per_variant:
			baseline_bankruptcies = maxi(baseline_bankruptcies, int(per_variant[key]))
	var limit := baseline_bankruptcies + BANKRUPTCY_TOLERANCE
	var project := ProjectSettings.globalize_path("res://").trim_suffix("/").trim_suffix("\\")
	var godot := OS.get_executable_path()
	var sweep_dir := TmpPaths.dir("world_sweep")

	# pior queda por nação em todas as variações: {tag: {delta, food, variant}}
	var worst: Dictionary = {}
	var reports: Dictionary = {}
	var bankruptcies_by_variant: Dictionary = {}
	## F8: falências e contagem de negócios de SERVIÇO são reportadas à parte —
	## o limite de falências acima é o das CONSTRUÇÕES (baseline f4). Negócio de
	## serviço abre e fecha por desenho (F8), então não entra no mesmo teto.
	var service_by_variant: Dictionary = {}
	var failed_variants: Dictionary = {}
	for variant: Dictionary in VARIANTS:
		var name := String(variant["name"])
		var out_path := sweep_dir.path_join("%s_%d.json" % [name, _days])
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
		service_by_variant[name] = {
			"bankruptcies": int(data.get("meta", {}).get("service_bankruptcies", 0)),
			"businesses": int(data.get("meta", {}).get("service_businesses", 0)),
		}
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
	for name in service_by_variant:
		var row: Dictionary = service_by_variant[name]
		print("[sweep] %-10s negocios_servico=%d falencias_servico=%d (informativo)" % [
			name, int(row["businesses"]), int(row["bankruptcies"])])
	for name in failed_variants:
		if not reports.has(name):
			continue
		_failures.append("%s: teste de mundo retornou %d" % [name, int(failed_variants[name])])

	if _out != "":
		_write_report(rows, bankruptcies_by_variant, service_by_variant, limit)


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


## Baseline de comida: aceita o formato `f4` (dicionário `{tag: {food: x}}`) e o
## formato `f9_invest_world_sweep` (array de `{tag, baseline, delta, worst_food}`).
func _normalize_nations(raw: Variant) -> Dictionary:
	var out: Dictionary = {}
	if typeof(raw) == TYPE_DICTIONARY:
		for tag in raw:
			var row: Dictionary = raw[tag] as Dictionary
			if row.has("food"):
				out[String(tag)] = {"food": float(row["food"])}
		return out
	if typeof(raw) == TYPE_ARRAY:
		for entry in raw:
			var row: Dictionary = entry as Dictionary
			var tag := String(row.get("tag", ""))
			if tag == "":
				continue
			out[tag] = {"food": float(row.get("worst_food", 1.0))}
		return out
	return out


func _write_report(rows: Array, bankruptcies: Dictionary, service: Dictionary,
		limit: int) -> void:
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
			"service_businesses": service,
			"variants": VARIANTS,
		},
		"nations": rows,
	}, "  "))
	file.close()
	print("[sweep] escrito %s" % _out)
