extends SceneTree

## B5 (D-089) — A/B obrigatório da INFRAESTRUTURA INICIAL em 365 d com IA.
##
## Roda `tools/b5_world_ai_365.gd` três vezes, um processo por variante (como o
## sweep do F5), e agrega a tabela:
##
##   1. `infra_zero`            — todas as regiões em 0 (a B5/D-089 anterior);
##   2. `infra_real`            — níveis iniciais do dado World Bank (o jogo);
##   3. `infra_real_no_penalty` — idem, com a penalidade de acesso zerada
##      (`set_balance("infra_access_penalty")`), isolando o frete dos níveis.
##
## Para cada variante: nações fora do portão de comida (média de 30 d < 0,975) e
## quais, falências, drift de dinheiro, obras de infra concluídas pela IA
## (Δ dos níveis) e concessões. A diferença entre 2 e 3 é a causa atribuída ao
## frete; entre 1 e 2 é o efeito dos níveis iniciais.
##
## Uso:
##   & $godot --headless --path . --script res://tools/b5_infra_ab_365.gd \
##     [--days=365] [--out=res://docs/economy/perf/b5_infra_ab_365.json]

const HARNESS := "res://tools/b5_world_ai_365.gd"
const OUT_PATH := "res://docs/economy/perf/b5_infra_ab_365.json"
const VARIANTS := [
	{"name": "infra_zero", "args": ["--infra=0"]},
	{"name": "infra_real", "args": ["--infra=real"]},
	{"name": "infra_real_no_penalty", "args": ["--infra=real", "--access-penalty=0"]},
]

var _days := 365
var _out := OUT_PATH
var _failures: Array[String] = []


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = maxi(1, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
	_ab()
	if _failures.is_empty():
		quit(0)
	else:
		for failure in _failures:
			print("[b5-ab] FALHA: " + failure)
		quit(1)


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _ab() -> void:
	var project := ProjectSettings.globalize_path("res://").trim_suffix("/").trim_suffix("\\")
	var godot := OS.get_executable_path()
	var work_dir := ProjectSettings.globalize_path("user://b5_infra_ab")
	DirAccess.make_dir_recursive_absolute(work_dir)

	var rows: Array = []
	for variant: Dictionary in VARIANTS:
		var name := String(variant["name"])
		var out_path := "user://b5_infra_ab/%s_%d.json" % [name, _days]
		var args: Array = ["--headless", "--path", project, "--script", HARNESS,
			"--", "--days=%d" % _days, "--tag=" + name, "--out=" + out_path]
		for extra in variant["args"]:
			args.append(String(extra))
		var output: Array = []
		var code := OS.execute(godot, args, output, true)
		for line: String in output:
			if line.begins_with("[b5-365]") or line.begins_with("ERROR") \
					or line.begins_with("SCRIPT ERROR"):
				print(line)
		var report: Variant = _load_json(out_path)
		if typeof(report) != TYPE_DICTIONARY:
			_failures.append("%s: sem relatorio (%s, exit %d)" % [
				name, out_path, code])
			continue
		if code != 0:
			_failures.append("%s: harness retornou %d" % [name, code])
		rows.append(report)

	if rows.size() == VARIANTS.size():
		_report(rows)
	elif _out != "":
		_write({"meta": {"days": _days, "error": "variantes incompletas"},
			"variants": rows})


## Tabela final: a mesma linha por variante + deltas contra `infra_zero`.
func _report(rows: Array) -> void:
	var base: Dictionary = rows[0]
	var table: Array = []
	for row: Dictionary in rows:
		table.append({
			"name": String(row["tag"]),
			"mode": String(row["mode"]),
			"access_penalty": float(row["access_penalty"]),
			"below_count": int(row["food"]["below_count"]),
			"nations": int(row["food"]["nations"]),
			"below": row["food"]["below"],
			"food_min": float(row["food"]["min"]),
			"food_min_nation": String(row["food"]["min_nation"]),
			"food_median": float(row["food"]["median"]),
			"food_mean": float(row["food"]["mean"]),
			"delta_below_vs_zero": int(row["food"]["below_count"])
				- int(base["food"]["below_count"]),
			"delta_mean_vs_zero": float(row["food"]["mean"])
				- float(base["food"]["mean"]),
			"bankruptcies": int(row["bankruptcies"]),
			"money_drift": float(row["money_drift"]),
			"infra_initial": int(row["infra"]["initial"]["total"]),
			"infra_final": int(row["infra"]["final"]["total"]),
			"works_total": int(row["infra"]["works"]["total"]),
			"works_road": int(row["infra"]["works"]["road"]),
			"works_power": int(row["infra"]["works"]["power"]),
			"works_water": int(row["infra"]["works"]["water"]),
			"concessions": int(row["concessions"]),
			"invariant_violations": int(row["invariant_violations"]),
			"stalled_days": int(row["stalled_days"]),
		})

	print("[b5-ab] tabela (365 d, IA):")
	print("  %-24s %-8s %6s %8s %8s %8s %10s %8s %8s %8s" % [
		"variante", "infra", "portão", "mediana", "média", "falênc.", "drift",
		"obras", "estrada", "concess."])
	for row: Dictionary in table:
		print("  %-24s %-8d %3d/%-3d %8.3f %8.3f %8d %10.3f %8d %8d %8d" % [
			String(row["name"]), int(row["infra_initial"]),
			int(row["below_count"]), int(row["nations"]),
			float(row["food_median"]), float(row["food_mean"]),
			int(row["bankruptcies"]), float(row["money_drift"]),
			int(row["works_total"]), int(row["works_road"]),
			int(row["concessions"])])
	for row: Dictionary in table:
		var below: Array = row["below"]
		var tags: Array = []
		for entry: Dictionary in below:
			tags.append("%s %.3f" % [String(entry["tag"]), float(entry["food"])])
		print("[b5-ab] %s fora do portão (%d): %s" % [
			String(row["name"]), below.size(), ", ".join(tags)])

	_write({
		"meta": {
			"days": _days,
			"food_gate": 0.975,
			"variants": VARIANTS,
			"note": "portão = média de 30 d de consumido/necessidade por nação; obras = Δ dos níveis de infra (uma obra = um nível)",
		},
		"variants": table,
	})


func _write(data: Dictionary) -> void:
	var dir := _out.get_base_dir()
	if dir != "" and not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var file := FileAccess.open(_out, FileAccess.WRITE)
	if file == null:
		_failures.append("nao abriu para escrita: " + _out)
		return
	file.store_string(JSON.stringify(data, "  ") + "\n")
	file.close()
	print("[b5-ab] escrito " + _out)
