extends SceneTree

## Teste headless do painel econômico da cidade.
##
## Verifica que o painel é montado com as seis seções, que os números exibidos
## batem com o status publicado pelo EconomySim e que todos os gráficos usam
## histórico real dos ticks (sem dados fictícios).

var _failures: Array[String] = []


func _init() -> void:
	root.size = Vector2i(1900, 1000)
	UiKit.set_locale("pt_BR")

	var scene: PackedScene = load("res://game/economy_panel_demo.tscn")
	var instance: Node = scene.instantiate()
	root.add_child(instance)

	# A janela headless não define o tamanho; fixamos a área do painel para
	# validar o layout largo (três colunas) usado no mapa.
	instance.set_anchors_and_offsets_preset(Control.PRESET_TOP_LEFT)
	(instance as Control).size = Vector2(1900.0, 1000.0)
	await process_frame

	instance.call("_advance", 30)
	await process_frame
	await process_frame

	var panel: Control = instance.get("_panel")
	if panel == null:
		print("[panel] painel ausente")
		quit(1)
		return

	_check_layout(panel)
	_check_content(panel, instance.get("_status"))
	_check_charts(panel)
	_check_performance(panel, instance.get("_status"))
	await _check_city_switch(panel, instance.get("_status"))

	if _failures.is_empty():
		print("[panel] OK: layout, conteúdo e séries validados.")
		quit(0)
		return
	print("[panel] FALHAS (%d):" % _failures.size())
	for failure in _failures:
		print(" - " + failure)
	quit(1)


func _check_layout(panel: Control) -> void:
	print("[panel] painel=%.0fx%.0f min=%.0fx%.0f" % [
		panel.size.x, panel.size.y,
		panel.get_combined_minimum_size().x, panel.get_combined_minimum_size().y])

	var grid := _find(panel, "GridContainer") as GridContainer
	if grid == null:
		_failures.append("grade de seções ausente")
		return
	print("[panel] grade=%.0fx%.0f colunas=%d" % [grid.size.x, grid.size.y, grid.columns])

	if grid.columns != CityEconomyPanel.WIDE_COLUMNS:
		_failures.append("layout largo deveria usar %d colunas" % CityEconomyPanel.WIDE_COLUMNS)
	if grid.size.x > panel.size.x + 1.0:
		_failures.append("grade mais larga que o painel (%.0f > %.0f)" % [
			grid.size.x, panel.size.x])

	var sections: Array[Control] = []
	for child in grid.get_children():
		if child is UiSection:
			sections.append(child)
	if sections.size() != 6:
		_failures.append("esperadas 6 seções, encontradas %d" % sections.size())

	for index in sections.size():
		var section: Control = sections[index]
		if section.size.x <= 1.0 or section.size.y <= 1.0:
			_failures.append("seção %d sem tamanho" % index)

	var scroll := _find(panel, "ScrollContainer") as ScrollContainer
	if scroll == null:
		_failures.append("ScrollContainer ausente")
	elif grid.size.y < scroll.size.y - 1.0:
		_failures.append("conteúdo não preenche a área visível (%.0f < %.0f)" % [
			grid.size.y, scroll.size.y])


func _check_content(panel: Control, status: Dictionary) -> void:
	var texts := _collect_texts(panel)
	print("[panel] rótulos=%d" % texts.size())

	var required := [
		"POPULAÇÃO E EMPREGO", "RENDA E PADRÃO DE VIDA", "PRODUTO INTERNO BRUTO",
		"PRODUÇÃO POR SETOR", "MERCADO LOCAL", "COMÉRCIO EXTERIOR",
		"PIB do dia", "PIB per capita", "Padrão de vida", "Saldo comercial",
		"Exportações", "Importações", "Desempregados", "Renda disponível",
		"População", "ESCASSEZ", "PREÇO", "PRODUÇÃO", "ESTOQUE",
	]
	for item in required:
		if not texts.has(item):
			_failures.append("rótulo ausente: %s" % item)

	var cities: Array = status.get("cities", [])
	if cities.is_empty():
		_failures.append("status sem cidades")
		return
	var city: Dictionary = cities[0]

	if not texts.has(UiKit.number(float(city["gdp"]), 1)):
		_failures.append("PIB formatado não encontrado na interface")
	if not texts.has(UiKit.number(float(city["markets"][0]["price"]), 1)):
		_failures.append("preço do trigo não encontrado na interface")

	var imported_total := _sum(city.get("import_value", []))
	var exported_total := _sum(city.get("export_value", []))
	if exported_total > 0.0 and not texts.has(UiKit.number(exported_total)):
		_failures.append("valor exportado não encontrado na interface")
	if imported_total > 0.0 and not texts.has(UiKit.number(imported_total)):
		_failures.append("valor importado não encontrado na interface")

	print("[panel] cidade=%s export=%.1f import=%.1f" % [
		city["id"], exported_total, imported_total])


func _check_charts(panel: Control) -> void:
	var charts: Array[UiChart] = []
	_collect_charts(panel, charts)
	if charts.is_empty():
		_failures.append("nenhum gráfico criado")
		return

	for index in charts.size():
		var series: Array = charts[index].get("_series")
		if series.is_empty():
			_failures.append("gráfico %d sem séries" % index)
			continue
		for entry in series:
			var values: PackedFloat32Array = entry.get("values", PackedFloat32Array())
			if values.size() < 2:
				_failures.append("gráfico %d com histórico insuficiente (%d)" % [
					index, values.size()])
			for value in values:
				if is_nan(value) or is_inf(value):
					_failures.append("gráfico %d com valor inválido" % index)
					break
	print("[panel] gráficos=%d" % charts.size())


func _check_performance(panel: Control, status: Dictionary) -> void:
	var start := Time.get_ticks_usec()
	for _index in 2000:
		panel.update(status, false)
	var sample_ms := float(Time.get_ticks_usec() - start) / 2000.0 / 1000.0

	start = Time.get_ticks_usec()
	for _index in 100:
		panel.update(status, true)
	var rebuild_ms := float(Time.get_ticks_usec() - start) / 100.0 / 1000.0

	print("[panel] amostra=%.4f ms atualização=%.2f ms" % [sample_ms, rebuild_ms])
	if sample_ms > 0.2:
		_failures.append("amostragem lenta (%.3f ms por tick)" % sample_ms)
	if rebuild_ms > 5.0:
		_failures.append("atualização lenta (%.2f ms por tick)" % rebuild_ms)


## Trocar de cidade deve refletir os números da nova cidade, sem estado antigo.
func _check_city_switch(panel: Control, status: Dictionary) -> void:
	var cities: Array = status.get("cities", [])
	if cities.size() < 2:
		return
	var target: Dictionary = cities[1]
	panel.call("set_city", String(target["id"]))
	await process_frame

	var texts := _collect_texts(panel)
	if not texts.has(String(target["name"])):
		_failures.append("troca de cidade não atualizou o título")
	if not texts.has(UiKit.number(float(target["gdp"]), 1)):
		_failures.append("troca de cidade não atualizou o PIB")
	print("[panel] troca de cidade=%s OK" % target["id"])


func _find(node: Node, type_name: String) -> Node:
	for child in node.get_children():
		if child.get_class() == type_name:
			return child
		var found := _find(child, type_name)
		if found != null:
			return found
	return null


func _collect_texts(node: Node, texts: Array[String] = []) -> Array[String]:
	if node is Label:
		var text := (node as Label).text
		if not text.is_empty():
			texts.append(text)
	for child in node.get_children():
		_collect_texts(child, texts)
	return texts


func _collect_charts(node: Node, charts: Array[UiChart]) -> void:
	if node is UiChart:
		charts.append(node)
	for child in node.get_children():
		_collect_charts(child, charts)


static func _sum(values: Array) -> float:
	var total := 0.0
	for value in values:
		total += float(value)
	return total
