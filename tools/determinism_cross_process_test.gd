extends SceneTree

## D-131: determinismo ENTRE PROCESSOS.
##
## Duas execuções headless separadas do mesmo mundo (cenário AGRIA/PORTUS com
## IA do C++ por 12 dias) têm de produzir o mesmo digest. O filho
## (`tools/determinism_digest_child.gd`) imprime `[det] sha256=...`; este teste
## roda o filho duas vezes com `OS.execute` e compara as saídas.
##
##   godot --headless --path . --script res://tools/determinism_cross_process_test.gd

const CHILD := "res://tools/determinism_digest_child.gd"
const MARK := "[det] sha256="

var _failures: Array[String] = []


func _init() -> void:
	var exe := OS.get_executable_path()
	var project := ProjectSettings.globalize_path("res://")
	var args := PackedStringArray([
		"--headless", "--disable-crash-handler", "--path", project,
		"--script", CHILD,
	])
	var first := _run(exe, args)
	var second := _run(exe, args)
	print("[det] processo 1: " + first)
	print("[det] processo 2: " + second)
	_check("duas execuções separadas com o mesmo seed",
		not first.is_empty() and first == second,
		"%s != %s" % [first, second])
	if _failures.is_empty():
		print("[det] OK: digest idêntico entre processos (%s)." % first)
		quit(0)
	else:
		for failure in _failures:
			print("[det] FALHA: " + failure)
		quit(1)


func _run(exe: String, args: PackedStringArray) -> String:
	var output: Array = []
	var code := OS.execute(exe, args, output, true)
	if code != 0:
		_fail("processo filho saiu com %d" % code)
		return ""
	for line: Variant in output:
		var text := String(line)
		var at := text.find(MARK)
		if at >= 0:
			return text.substr(at + MARK.length()).strip_edges()
	_fail("filho não imprimiu o digest")
	return ""


func _check(name: String, ok: bool, detail: String = "") -> void:
	if ok:
		print("[det] ok: " + name)
	else:
		_fail(name + (": " + detail if detail != "" else ""))


func _fail(message: String) -> void:
	_failures.append(message)
	print("[det] FALHA: " + message)
