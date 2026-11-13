extends SceneTree

## Destaque de nação do mapa político (API do TerritoryMap): garante que o
## material NUNCA fica com o latão de um destaque antigo — set/clear reescrevem
## o uniforme sozinhos, sem depender de um `apply_nation_highlight` extra.
##   godot --headless --path . --script res://tools/nation_highlight_test.gd

const TerritoryMapScript: Script = preload("res://map/territory_map.gd")
const ATLAS_SHADER: Shader = preload("res://map/map_shaders/atlas_political.gdshader")
const OVERLAY_SHADER: Shader = preload("res://map/map_shaders/political_overlay.gdshader")


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var failures: Array[String] = []
	var territory: TerritoryMap = TerritoryMapScript.new()
	territory.load_data()
	if territory.owner_index("FRA") <= 0:
		push_error("[highlight] TerritoryMap não carregou (FRA sem índice)")
		quit(1)
		return

	var atlas := ShaderMaterial.new()
	atlas.shader = ATLAS_SHADER
	territory.apply_nation_highlight(atlas)
	_expect(failures, "sem destaque, highlight_nation = -1",
			int(atlas.get_shader_parameter("highlight_nation")), -1)
	_expect(failures, "sem destaque, highlight_color = latão",
			atlas.get_shader_parameter("highlight_color"), TerritoryMap.HIGHLIGHT_BRASS)

	# (a) set escreve no material registrado (sem apply manual).
	var fra: int = territory.set_highlight_nation("FRA")
	if fra <= 0:
		failures.append("set_highlight_nation(FRA) devolveu %d" % fra)
	_expect(failures, "set escreve no material sozinho",
			int(atlas.get_shader_parameter("highlight_nation")), fra)

	# (b) clear zera o latão no material SEM apply manual (o bug do latão preso).
	territory.clear_highlight()
	_expect(failures, "clear zera o material sozinho",
			int(atlas.get_shader_parameter("highlight_nation")), -1)
	_expect(failures, "clear zera o índice interno", territory.highlight_nation_index(), -1)

	# (c) tag desconhecida limpa tudo.
	var unknown: int = territory.set_highlight_nation("ZZZ")
	_expect(failures, "tag desconhecida devolve -1", unknown, -1)
	_expect(failures, "tag desconhecida limpa o material",
			int(atlas.get_shader_parameter("highlight_nation")), -1)

	# (d) registrar o mesmo material duas vezes não duplica nem quebra.
	territory.apply_nation_highlight(atlas)
	territory.apply_nation_highlight(atlas)
	var target: int = territory.set_highlight_nation("CAN")
	_expect(failures, "registro duplicado mantém o valor",
			int(atlas.get_shader_parameter("highlight_nation")), target)

	# (e) material cujo shader não declara o uniforme (overlay) é ignorado.
	var overlay := ShaderMaterial.new()
	overlay.shader = OVERLAY_SHADER
	territory.apply_nation_highlight(overlay)
	if overlay.get_shader_parameter("highlight_nation") != null:
		failures.append("overlay recebeu highlight_nation (deveria ser ignorado)")

	# (f) material que perdeu a última referência sai da lista sem erro.
	_apply_temporary(territory, failures)
	territory.clear_highlight()
	_expect(failures, "material liberado é podado",
			int(atlas.get_shader_parameter("highlight_nation")), -1)

	if failures.is_empty():
		print("[highlight] OK: set/clear reescrevem o material e o latão nunca fica preso.")
		quit(0)
	else:
		for failure: String in failures:
			push_error("[highlight] " + failure)
		quit(1)


func _expect(failures: Array[String], label: String, got: Variant, want: Variant) -> void:
	if got != want:
		failures.append("%s (obtido %s, esperado %s)" % [label, got, want])


## Material temporário que morre ao sair do escopo: exercita a poda da lista de
## referências fracas.
func _apply_temporary(territory: TerritoryMap, _failures: Array[String]) -> void:
	var temporary := ShaderMaterial.new()
	temporary.shader = ATLAS_SHADER
	territory.apply_nation_highlight(temporary)
