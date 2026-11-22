extends SceneTree

## Replay headless de um snapshot do mundo (A1 — núcleo C++ fora do Godot).
##
## Carrega `{"definitions": ..., "save": ...}` de `tools/export_world_snapshot.gd`
## com `from_snapshot_file`, roda N dias sem IA e regrava o snapshot final
## com `to_snapshot_file`. Como o snapshot traz as próprias definições,
## NADA de `data/definitions/` é lido aqui — o arquivo basta.
##
##   godot --headless --path . --script res://tools/snapshot_replay.gd \
##     -- --in=snapshot.json --days=60 --out=resultado.json [--threads=0] [--ai=0|1]
##
## - `--in` e `--out` são obrigatórios; `--days` padrão 60; `--threads` padrão 0
##   (mesmo contrato de `set_sim_threads`: 0 deixa o núcleo decidir).
## - `--ai=1` liga a IA econômica do núcleo no replay (`set_ai_enabled(true)`,
##   A5), depois de `from_snapshot_file`; padrão 0 = replay sem IA.
## - `invariant_violations` é conferido a CADA dia: a primeira violação aborta
##   na hora, sem gravar resultado.
## - Leitura e escrita do mundo passam pelo serializador da BIBLIOTECA
##   (`ogs::save::parse_json_file`/`dump_json`), nunca por `JSON.parse_string`/
##   `JSON.stringify`: o JSON do Godot não é round-trip IEEE-exato para
##   |x| < 1. O `JSON.parse_string` do arquivo só é usado para CONTAR cidades
##   (conferência de que o mundo foi aplicado); nenhum desses valores entra no
##   mundo.
## - A saída tem exatamente duas chaves (`definitions`, `save`) — é o que
##   permite comparar dois replays byte a byte (e contra o CLI).
## - Saída: 0 ok, 1 falha. Toda falha vira linha `[replay] FALHA: ...`.

var _in := ""
var _out := ""
var _days := 60
var _threads := 0
## `--ai=1` liga a IA econômica do núcleo (A5). Padrão: replay sem IA.
var _ai := false

var _failures: Array[String] = []


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--in="):
			_in = arg.split("=", true, 1)[1]
		elif arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
		elif arg.begins_with("--days="):
			_days = maxi(0, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--threads="):
			_threads = maxi(0, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--ai="):
			_ai = arg.split("=", true, 1)[1] != "0"

	if _in == "":
		print("[replay] FALHA: --in=<snapshot.json> e obrigatorio")
		quit(1)
		return
	if _out == "":
		print("[replay] FALHA: --out=<resultado.json> e obrigatorio")
		quit(1)
		return

	_run()
	if _failures.is_empty():
		quit(0)
	else:
		for failure in _failures:
			print("[replay] FALHA: " + failure)
		quit(1)


func _fail(msg: String) -> void:
	_failures.append(msg)


func _count(block: Dictionary, key: String) -> int:
	return Array(block.get(key, [])).size()


func _run() -> void:
	# Contagem de cidades do arquivo, só para conferir que o mundo foi aplicado.
	# O `JSON.parse_string` daqui NÃO alimenta o mundo (ele perde bits em
	# |x| < 1): o mundo vem de `from_snapshot_file`, que lê pelo
	# serializador exato da biblioteca.
	var file := FileAccess.open(_in, FileAccess.READ)
	if file == null:
		_fail("nao abriu " + _in)
		return
	var text := file.get_as_text()
	file.close()

	var parsed: Variant = JSON.parse_string(text)
	if typeof(parsed) != TYPE_DICTIONARY:
		_fail(_in + " nao e um objeto JSON")
		return
	var snapshot: Dictionary = parsed
	var definitions: Variant = snapshot.get("definitions", null)
	var save_block: Variant = snapshot.get("save", null)
	if typeof(definitions) != TYPE_DICTIONARY or typeof(save_block) != TYPE_DICTIONARY:
		_fail(_in + " sem os blocos 'definitions' e 'save' (nao e snapshot do nucleo)")
		return
	var file_cities := _count(save_block, "cities")

	var sim = EconomySim.new()
	if not sim.has_method("from_snapshot_file"):
		_fail("binario sem from_snapshot_file (rode o build do gdextension)")
		return
	if sim.has_method("set_sim_threads"):
		sim.set_sim_threads(_threads)

	# O snapshot traz as definições: nenhum `set_*` aqui.
	if not sim.from_snapshot_file(_in):
		_fail("from_snapshot_file recusou " + _in)
		return

	# `from_snapshot_file` só emite erro no log quando o bloco é inválido;
	# a conferência de cidades garante que o mundo foi mesmo aplicado.
	var loaded: Dictionary = sim.to_dict()
	if _count(loaded, "cities") != file_cities:
		_fail("from_snapshot_file nao aplicou o mundo (cidades %d != %d)" % [
			_count(loaded, "cities"), file_cities])
		return

	# A5: a IA econômica vive no núcleo; ligada aqui, roda como fase de cada dia.
	if _ai:
		sim.set_ai_enabled(true)

	var started := Time.get_ticks_msec()
	var status: Dictionary = {}
	for day in _days:
		status = sim.tick_day()
		var violations: Array = status.get("invariant_violations", [])
		if violations.size() > 0:
			_fail("dia %d: invariante violado: %s" % [
				int(status.get("day", day + 1)), String(violations[0])])
			return
	var elapsed := Time.get_ticks_msec() - started

	var dir := _out.get_base_dir()
	if dir != "" and not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	# Escrita pelo serializador da biblioteca (mesmo caminho do CLI headless).
	if not sim.to_snapshot_file(_out):
		_fail("to_snapshot_file nao gravou " + _out)
		return

	# Contagens SÓ para a mensagem (o arquivo já está gravado).
	var result: Dictionary = sim.to_snapshot()
	var final_save: Dictionary = result.get("save", {})
	print("[replay] dias=%d threads=%d day=%d cidades=%d inv_violations=0 ms=%d" % [
		_days, _threads, int(final_save.get("day", 0)),
		_count(final_save, "cities"), elapsed])
	print("[replay] escrito %s (%d bytes)" % [_out, _file_size(_out)])


func _file_size(path: String) -> int:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return 0
	var size := file.get_length()
	file.close()
	return size
