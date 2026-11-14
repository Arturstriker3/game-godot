extends RefCounted
class_name UiTemplateBuilder

## Gerador de telas a partir de uma especificação (Dictionary/JSON).
##
## A spec descreve o "envelope" (`shell`) e uma lista de blocos. O gerador não
## conhece economia: todo valor chega já formatado (literal, `$caminho` em
## `data` ou `@chave` de i18n) e nenhum bloco calcula regra de jogo.
##
## Formato resumido:
##   {
##     "id": "dossie",
##     "shell": {"kind": "window|side_panel|telegram", ...},
##     "data": {...},              # valores já formatados
##     "blocks": [ {bloco}, ... ]  # corpo (window/telegram) ou coluna (panel)
##   }
##
## Texto: `@chave` → `I18n.t(chave, params)`; `$caminho` → `data.caminho`;
## qualquer outra string é literal. `params` de um bloco é resolvido antes da
## tradução. Blocos aceitam `width`/`height` (mínimos), `w` (peso no HBox),
## `grow` (expande no eixo do contêiner) e `align`.

const TEMPLATE_DIR := "res://data/ui/templates"
const TEMPLATE_IDS := ["dossie", "registro", "ficha", "telegrama", "quadro",
		"menu"]
const TOP_BAR_CLEARANCE := 68.0
const VIEW_W := 1920.0
const VIEW_H := 1080.0

const RESOURCE_NAMES := ["resource.coal", "resource.gas", "resource.iron",
		"resource.oil", "resource.wheat"]


## Raiz devolvida por `build`: avisa o host quando o jogador fecha ou escolhe.
class TemplateRoot extends Control:
	signal close_requested
	signal choice_selected(id: String)


# --- Peças desenhadas -------------------------------------------------------

## Linha reta ou tracejada; usada para filetes duplos e réguas pontilhadas.
class Hairline extends Control:
	var color := UiKit.BARRA_FILETE
	var thickness := 1.0
	var dashed := false
	var dash := 4.0

	func _init(line_color: Color, line_thickness: float = 1.0,
			is_dashed: bool = false) -> void:
		color = line_color
		thickness = line_thickness
		dashed = is_dashed
		custom_minimum_size.y = maxf(line_thickness, 1.0)
		mouse_filter = Control.MOUSE_FILTER_IGNORE

	func _draw() -> void:
		var y := size.y * 0.5
		if dashed:
			draw_dashed_line(Vector2(0.0, y), Vector2(size.x, y), color,
					thickness, dash)
		else:
			draw_line(Vector2(0.0, y), Vector2(size.x, y), color, thickness)


## Barra de proporção com moldura de tinta (10 px em fichas, 22 px em rankings).
class MeterBar extends Control:
	var ratio := 0.0
	var fill := UiKit.TINTA

	func _init(value: float, color: Color, height: float = 10.0) -> void:
		ratio = clampf(value, 0.0, 1.0)
		fill = color
		custom_minimum_size.y = height
		size_flags_horizontal = Control.SIZE_EXPAND_FILL
		size_flags_vertical = Control.SIZE_SHRINK_CENTER
		mouse_filter = Control.MOUSE_FILTER_IGNORE

	func _draw() -> void:
		var full := Rect2(Vector2.ZERO, size)
		draw_rect(full, fill, false, 1.0)
		var inner := Vector2(maxf(size.x - 3.0, 0.0), maxf(size.y - 3.0, 0.0))
		var filled := Vector2(inner.x * ratio, inner.y)
		if filled.x > 0.0:
			draw_rect(Rect2(Vector2(1.5, 1.5), filled), fill)


## Líder pontilhado entre a chave e o valor de uma linha `kv_row`.
class DottedLeader extends Control:
	var color := UiKit.TINTA_SUAVE

	func _init() -> void:
		custom_minimum_size = Vector2(24.0, 18.0)
		size_flags_horizontal = Control.SIZE_EXPAND_FILL
		size_flags_vertical = Control.SIZE_SHRINK_CENTER
		mouse_filter = Control.MOUSE_FILTER_IGNORE

	func _draw() -> void:
		var y := size.y - 5.0
		draw_dashed_line(Vector2(0.0, y), Vector2(size.x, y), color, 1.0, 2.0)


## Barras verticais (abastecimento por dia, distribuições curtas).
##
## O gráfico pinta o próprio papel antes das barras: um `Control` sem StyleBox
## não herda fundo e, sem essa placa, uma série de uma amostra virava um bloco
## escuro de ponta a ponta. Abaixo de `min_samples` só o papel e a base ficam.
class BarChart extends Control:
	var values: Array[float] = []
	var tones: Array[Color] = []
	var baseline := UiKit.TINTA
	var gap := 6.0
	## Escala fixa (ex.: 7 dias de cobertura). 0 = normaliza pelo pico.
	var scale_max := 0.0
	## Papel claro da placa: nunca o fundo escuro do tema.
	var paper := UiKit.PAPEL_CLARO
	var frame := Color(UiKit.TINTA, 0.35)
	## Amostras mínimas para desenhar barras (1 ponto não é série).
	var min_samples := 2

	func _paint_paper() -> void:
		if size.x <= 0.0 or size.y <= 0.0:
			return
		draw_rect(Rect2(Vector2.ZERO, size), paper)
		draw_rect(Rect2(Vector2(0.5, 0.5), size - Vector2(1.0, 1.0)),
				frame, false, 1.0)

	func _draw() -> void:
		_paint_paper()
		if size.x <= 0.0 or size.y <= 0.0:
			return
		draw_line(Vector2(2.0, size.y - 0.5), Vector2(size.x - 2.0, size.y - 0.5),
				baseline, 1.0)
		if values.size() < min_samples:
			return
		var peak := 0.0001
		if scale_max > 0.0:
			peak = scale_max
		else:
			for value in values:
				peak = maxf(peak, value)
		var count := values.size()
		var bar_width := maxf((size.x - gap * (count - 1)) / count, 1.0)
		for index in count:
			var height := (size.y - 3.0) * clampf(values[index] / peak, 0.0, 1.0)
			var rect := Rect2(Vector2(1.0 + index * (bar_width + gap),
					size.y - 1.5 - height), Vector2(bar_width, height))
			var color := UiKit.TINTA
			if index < tones.size():
				color = tones[index]
			if color.a <= 0.0:
				color = UiKit.TINTA
			draw_rect(rect, color)


## Linha de tendência sobre papel claro: duas réguas tracejadas finas, base de
## tinta e marcador no último valor.
class LineChart extends Control:
	var values: Array[float] = []
	var line_color := UiKit.TINTA
	var guide := Color(UiKit.TINTA_SUAVE, 0.45)
	var thickness := 1.6
	## `true` desenha a faixa mín..máx da própria série (população absoluta);
	## `false` trata os valores como índice 0..1 (renda, posições).
	var autoscale := false
	var paper := UiKit.PAPEL_CLARO
	var frame := Color(UiKit.TINTA, 0.35)
	var min_samples := 2

	func _paint_paper() -> void:
		if size.x <= 0.0 or size.y <= 0.0:
			return
		draw_rect(Rect2(Vector2.ZERO, size), paper)
		draw_rect(Rect2(Vector2(0.5, 0.5), size - Vector2(1.0, 1.0)),
				frame, false, 1.0)

	func _draw() -> void:
		_paint_paper()
		if size.x <= 0.0 or size.y <= 0.0:
			return
		var inner := size.y - 4.0
		for step: float in [1.0 / 3.0, 2.0 / 3.0]:
			var y: float = 2.0 + inner * step
			draw_dashed_line(Vector2(2.0, y), Vector2(size.x - 2.0, y), guide,
					1.0, 3.0)
		draw_line(Vector2(2.0, size.y - 1.5), Vector2(size.x - 2.0, size.y - 1.5),
				UiKit.TINTA, 1.0)
		if values.size() < min_samples:
			return
		var low := 0.0
		var high := 1.0
		if autoscale:
			low = values[0]
			high = values[0]
			for value in values:
				low = minf(low, value)
				high = maxf(high, value)
			if high - low < 1e-9:
				high = low + 1.0
		var span := maxf(high - low, 1e-9)
		var points := PackedVector2Array()
		for index in values.size():
			var x := 2.0 + (size.x - 4.0) * float(index) \
					/ float(values.size() - 1)
			var ratio := clampf((values[index] - low) / span, 0.0, 1.0)
			var y := size.y - 1.5 - ratio * inner
			points.append(Vector2(x, y))
		draw_polyline(points, line_color, thickness, true)
		draw_circle(points[points.size() - 1], 2.0, line_color)


## Papel com borda opcionalmente tracejada (moldura do telegrama, notas).
class PaperPanel extends PanelContainer:
	var dashed := false
	var dash_color := UiKit.TINTA

	func _draw() -> void:
		if not dashed:
			return
		var rect := Rect2(Vector2(0.5, 0.5), size - Vector2(1.0, 1.0))
		draw_dashed_line(rect.position, Vector2(rect.end.x, rect.position.y),
				dash_color, 1.0, 4.0)
		draw_dashed_line(Vector2(rect.end.x, rect.position.y), rect.end,
				dash_color, 1.0, 4.0)
		draw_dashed_line(rect.end, Vector2(rect.position.x, rect.end.y),
				dash_color, 1.0, 4.0)
		draw_dashed_line(Vector2(rect.position.x, rect.end.y), rect.position,
				dash_color, 1.0, 4.0)


## Linha de tabela em papel: seleção com tarja de carimbo e base pontilhada.
## `pressed` seleciona; `activated` (Enter, duplo clique) abre o detalhe.
class PaperRow extends PanelContainer:
	signal pressed(index: int)
	signal activated(index: int)

	var index := 0
	var selected := false

	func _draw() -> void:
		if selected:
			draw_rect(Rect2(Vector2.ZERO, Vector2(3.0, size.y)), UiKit.CARIMBO)
		draw_dashed_line(Vector2(0.0, size.y - 0.5),
				Vector2(size.x, size.y - 0.5), UiKit.TINTA_SUAVE, 1.0, 2.0)

	func _gui_input(event: InputEvent) -> void:
		if event is InputEventMouseButton \
				and event.button_index == MOUSE_BUTTON_LEFT and event.pressed:
			pressed.emit(index)
			if event.double_click:
				activated.emit(index)
		elif event is InputEventKey and event.pressed and not event.echo \
				and event.is_action("ui_accept"):
			activated.emit(index)


## Gira um selo sem afetar a medida do layout (o contêiner externo mede).
class Tilt extends MarginContainer:
	var angle := 0.0

	func _apply_angle() -> void:
		if get_child_count() == 0:
			return
		var child := get_child(0) as Control
		child.pivot_offset = child.size * 0.5
		child.rotation_degrees = angle

	func _notification(what: int) -> void:
		if what == NOTIFICATION_RESIZED:
			_apply_angle()


## Tabela com ordenação, seleção e paginação — só estado de exibição.
## `activate: "single"` (padrão) abre no primeiro clique; `"double"` só abre no
## Enter ou no duplo clique, deixando o primeiro clique selecionar a linha.
class DataTableView extends PanelContainer:
	signal row_activated(index: int)
	signal row_selected(index: int)

	var columns: Array = []
	var rows: Array = []
	var selected := -1
	var page_size := 9
	var page := 0
	var sort_col := 1
	var descending := true
	var footer_left := ""
	var activate := "single"
	## `fill_height`: quantas linhas cabem na altura disponível viram o tamanho
	## da página (o `page_size` da spec fica só como medida de segurança antes
	## do primeiro layout). Mantém a tabela cheia em qualquer resolução.
	var fill_height := false

	var _rows_holder: VBoxContainer
	var _page_label: Label
	var _prev: Button
	var _next: Button
	var _ctx: Dictionary = {}
	var _header: Control
	var _footer: Control
	var _row_height := 0.0
	var _refit_queued := false


	func setup(spec: Dictionary, ctx: Dictionary) -> void:
		_ctx = ctx
		columns = spec.get("columns", [])
		var source_rows: Array = spec.get("rows", [])
		rows = _resolve_rows(source_rows, ctx)
		selected = int(spec.get("selected", -1))
		page_size = maxi(int(spec.get("page_size", 9)), 1)
		page = maxi(int(spec.get("page", 1)) - 1, 0)
		sort_col = int(spec.get("sort_col", 1))
		descending = bool(spec.get("descending", true))
		footer_left = UiTemplateBuilder._text(spec, "footer_left", ctx)
		activate = String(spec.get("activate", "single"))
		fill_height = bool(spec.get("fill_height", false))

		add_theme_stylebox_override("panel",
				UiTemplateBuilder._paper_style(UiKit.PAPEL_CLARO, UiKit.TINTA,
						0.0, 1, ["all"]))
		mouse_filter = Control.MOUSE_FILTER_PASS

		var box := VBoxContainer.new()
		box.add_theme_constant_override("separation", 0)
		add_child(box)
		_header = _build_header()
		box.add_child(_header)

		_rows_holder = VBoxContainer.new()
		_rows_holder.add_theme_constant_override("separation", 0)
		_rows_holder.size_flags_vertical = Control.SIZE_EXPAND_FILL
		box.add_child(_rows_holder)

		# A tabela que calcula as linhas pela altura não precisa do espaçador:
		# o próprio bloco de linhas fica com o espaço que sobra.
		if not fill_height:
			var filler := Control.new()
			filler.size_flags_vertical = Control.SIZE_EXPAND_FILL
			filler.mouse_filter = Control.MOUSE_FILTER_IGNORE
			box.add_child(filler)
		box.add_child(UiTemplateBuilder._line(UiKit.TINTA, 1.0))
		_footer = _build_footer()
		box.add_child(_footer)
		_refresh()


	## Substitui as linhas no lugar (o host atualiza a tabela a cada dia).
	## `index` != -2 já deixa a seleção na linha pedida (lista de origem).
	func update_rows(source: Array, index: int = -2) -> void:
		var keep_focus := _focused_row() >= 0
		if index != -2:
			selected = index
		rows = _resolve_rows(source, _ctx)
		_refresh()
		if keep_focus:
			_focus_row(selected)


	## Índice da linha que está com o foco de teclado (-1 se nenhuma).
	func _focused_row() -> int:
		for child in _rows_holder.get_children():
			if child is PaperRow and (child as Control).has_focus():
				return (child as PaperRow).index
		return -1


	func _focus_row(index: int) -> void:
		for child in _rows_holder.get_children():
			if child is PaperRow and (child as PaperRow).index == index:
				(child as Control).grab_focus()
				return


	## Marca a linha selecionada pelo índice da lista de origem (não da página).
	func set_selected(index: int) -> void:
		selected = index
		_refresh()


	## Volta para a primeira página (busca e filtros mudaram a lista).
	func reset_page() -> void:
		page = 0
		_refresh()


	## Resolve `@chave`/`$valor` uma vez; a tabela guarda só texto pronto.
	func _resolve_rows(source: Array, ctx: Dictionary) -> Array:
		var out: Array = []
		for row in source:
			var cells: Array = []
			for cell in row:
				if cell is Dictionary:
					var copy: Dictionary = (cell as Dictionary).duplicate()
					copy["text"] = UiTemplateBuilder._resolve(cell.get("text", ""), ctx)
					cells.append(copy)
				else:
					cells.append({"text": UiTemplateBuilder._resolve(cell, ctx)})
			out.append(cells)
		return out


	func _build_header() -> Control:
		var wrap := MarginContainer.new()
		wrap.add_theme_constant_override("margin_left", 16)
		wrap.add_theme_constant_override("margin_right", 16)
		wrap.add_theme_constant_override("margin_top", 10)
		wrap.add_theme_constant_override("margin_bottom", 10)
		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", 12)
		wrap.add_child(row)

		for index in columns.size():
			var column: Dictionary = columns[index]
			var button := Button.new()
			button.flat = true
			button.focus_mode = Control.FOCUS_ALL
			button.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
			button.text = UiTemplateBuilder._text(column, "label", _ctx)
			if index == sort_col:
				button.text += " ▾" if descending else " ▴"
			button.alignment = UiTemplateBuilder._alignment(column)
			button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			button.size_flags_stretch_ratio = float(column.get("weight", 1.0))
			button.add_theme_font_override("font", UiKit.PAPER_BODY_FONT)
			button.add_theme_font_size_override("font_size", 12)
			button.add_theme_color_override("font_color",
					UiKit.TINTA if index == sort_col else UiKit.TINTA_SUAVE)
			button.add_theme_color_override("font_hover_color", UiKit.CARIMBO)
			button.add_theme_stylebox_override("normal",
					UiTemplateBuilder._flat(Color(0, 0, 0, 0), Color(0, 0, 0, 0)))
			button.add_theme_stylebox_override("hover",
					UiTemplateBuilder._flat(Color(0, 0, 0, 0), Color(0, 0, 0, 0)))
			button.add_theme_stylebox_override("pressed",
					UiTemplateBuilder._flat(Color(0, 0, 0, 0), Color(0, 0, 0, 0)))
			button.add_theme_stylebox_override("focus",
					UiTemplateBuilder._flat(Color(0, 0, 0, 0), UiKit.CARIMBO))
			button.pressed.connect(_on_sort.bind(index))
			row.add_child(button)

		var holder := VBoxContainer.new()
		holder.add_theme_constant_override("separation", 0)
		holder.add_child(wrap)
		holder.add_child(UiTemplateBuilder._line(UiKit.TINTA, 2.0))
		return holder


	func _build_footer() -> Control:
		var wrap := MarginContainer.new()
		wrap.add_theme_constant_override("margin_left", 16)
		wrap.add_theme_constant_override("margin_right", 16)
		wrap.add_theme_constant_override("margin_top", 8)
		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", 8)
		wrap.add_child(row)

		var hint := UiTemplateBuilder._typed(footer_left, 12, UiKit.TINTA_SUAVE)
		hint.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		row.add_child(hint)

		var spacer := Control.new()
		spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(spacer)

		_prev = UiTemplateBuilder._mini_paper_button("‹")
		_prev.pressed.connect(_on_page.bind(-1))
		row.add_child(_prev)

		_page_label = UiTemplateBuilder._typed("", 12, UiKit.TINTA_SUAVE)
		_page_label.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		row.add_child(_page_label)

		_next = UiTemplateBuilder._mini_paper_button("›")
		_next.pressed.connect(_on_page.bind(1))
		row.add_child(_next)
		return wrap


	func _refresh() -> void:
		_refit_queued = false
		for child in _rows_holder.get_children():
			_rows_holder.remove_child(child)
			child.queue_free()

		if rows.is_empty():
			selected = -1
		else:
			selected = clampi(selected, 0, rows.size() - 1)

		var order: Array[int] = []
		for index in rows.size():
			order.append(index)
		if sort_col >= 0 and sort_col < columns.size():
			order.sort_custom(func(a: int, b: int) -> bool:
				var left := _cell_text(rows[a], sort_col)
				var right := _cell_text(rows[b], sort_col)
				var result := UiTemplateBuilder._compare_cells(left, right)
				return result > 0 if descending else result < 0)

		# Altura disponível / altura de uma linha: a página cresce com a janela.
		var per_page := page_size
		if fill_height:
			var capacity := _capacity()
			if capacity > 0:
				per_page = capacity
		var pages := maxi(int(ceil(float(rows.size()) / float(per_page))), 1)
		page = clampi(page, 0, pages - 1)
		var start := page * per_page
		var end := mini(start + per_page, order.size())
		for offset in range(start, end):
			var row_index: int = order[offset]
			_rows_holder.add_child(_build_row(row_index, rows[row_index]))
		if fill_height:
			_measure_row_height()
			_fill_page_rows(end - start, per_page)

		_page_label.text = I18n.t("tpl.common.page",
				{"page": page + 1, "pages": pages})
		_prev.disabled = page <= 0
		_next.disabled = page >= pages - 1


	## Linhas que cabem entre o cabeçalho e o rodapé (0 até medir a primeira).
	func _capacity() -> int:
		if _row_height <= 0.0 or size.y <= 0.0:
			return 0
		var header_height := _header.get_combined_minimum_size().y \
				if _header != null else 0.0
		var footer_height := _footer.get_combined_minimum_size().y \
				if _footer != null else 0.0
		var available := size.y - header_height - footer_height - 1.0
		if available <= 0.0:
			return 0
		return maxi(int(floor(available / _row_height)), 1)


	## Altura real de uma linha (fonte + margens), medida uma vez por tabela.
	func _measure_row_height() -> void:
		for child in _rows_holder.get_children():
			if child is PaperRow:
				var measured := (child as Control).get_combined_minimum_size().y
				if measured > 0.0 and not is_equal_approx(measured, _row_height):
					_row_height = measured
					_queue_refit()
				return


	## Página cheia: as linhas dividem a sobra de altura (menor que uma linha)
	## para o corpo da tabela não terminar numa faixa vazia. A última página,
	## quando parcial, continua alinhada ao topo.
	func _fill_page_rows(count: int, per_page: int) -> void:
		if per_page <= 0 or count < per_page:
			return
		for child in _rows_holder.get_children():
			if child is PaperRow:
				(child as Control).size_flags_vertical = Control.SIZE_EXPAND_FILL


	func _queue_refit() -> void:
		if not fill_height or _refit_queued or not is_inside_tree():
			return
		_refit_queued = true
		_refresh.call_deferred()


	func _notification(what: int) -> void:
		if what == NOTIFICATION_RESIZED:
			_queue_refit()


	func _build_row(row_index: int, cells: Array) -> Control:
		var panel := PaperRow.new()
		panel.index = row_index
		panel.focus_mode = Control.FOCUS_ALL
		# A primeira linha é o alvo preferido do foco ao abrir a tela: melhor que
		# um botão isolado no cromo e infinitamente melhor que o fechar.
		if row_index == 0:
			panel.set_meta(UiKit.UI_FOCUS_PREFERRED, true)
		panel.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
		UiTemplateBuilder._apply_row_style(panel, selected)
		panel.pressed.connect(_on_row_pressed)
		panel.activated.connect(_on_row_activated)
		panel.mouse_entered.connect(func() -> void:
			if panel.index != selected:
				panel.add_theme_stylebox_override("panel",
						UiTemplateBuilder._flat(UiKit.PAPEL, Color(0, 0, 0, 0))))
		panel.mouse_exited.connect(func() -> void:
			UiTemplateBuilder._apply_row_style(panel, selected))

		var margin := MarginContainer.new()
		margin.add_theme_constant_override("margin_left", 16)
		margin.add_theme_constant_override("margin_right", 16)
		margin.add_theme_constant_override("margin_top", 10)
		margin.add_theme_constant_override("margin_bottom", 10)
		margin.mouse_filter = Control.MOUSE_FILTER_IGNORE
		panel.add_child(margin)

		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", 12)
		row.mouse_filter = Control.MOUSE_FILTER_IGNORE
		margin.add_child(row)

		for index in columns.size():
			var column: Dictionary = columns[index]
			var cell: Dictionary = cells[index] if index < cells.size() else {}
			var label := UiTemplateBuilder._cell_label(cell, column)
			label.mouse_filter = Control.MOUSE_FILTER_IGNORE
			row.add_child(label)
		return panel


	func _cell_text(cells: Array, index: int) -> String:
		if index >= cells.size():
			return ""
		var cell: Variant = cells[index]
		if cell is Dictionary:
			return String(cell.get("text", ""))
		return str(cell)


	func _on_sort(index: int) -> void:
		if sort_col == index:
			descending = not descending
		else:
			sort_col = index
			descending = true
		UiSfx.play(UiSfx.Sound.SWITCH)
		_refresh()


	func _on_page(delta: int) -> void:
		page += delta
		UiSfx.play(UiSfx.Sound.SWITCH)
		_refresh()


	func _on_row_pressed(index: int) -> void:
		UiSfx.play(UiSfx.Sound.SELECT)
		_select_row(index)
		row_selected.emit(index)
		if activate != "double":
			row_activated.emit(index)


	func _on_row_activated(index: int) -> void:
		UiSfx.play(UiSfx.Sound.SELECT)
		_select_row(index)
		row_activated.emit(index)


	## Marca a linha sem reconstruir a tabela: o foco (e o Enter) fica na linha.
	func _select_row(index: int) -> void:
		selected = index
		for child in _rows_holder.get_children():
			if child is PaperRow:
				UiTemplateBuilder._apply_row_style(child, selected)


## Seletor de papel: botão que abre um menu de opções no próprio estilo da
## ficha. Usado onde a lista é longa demais para chips (nação, parceiro, bem)
## e o host popula os itens depois de construir a spec.
class PaperSelect extends Button:
	signal selected(id: String)

	var items: Array = []
	var selected_id := ""
	var placeholder := "—"
	var _menu: PopupMenu


	func _init() -> void:
		focus_mode = Control.FOCUS_ALL
		mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
		custom_minimum_size = Vector2(240.0, 38.0)
		add_theme_font_override("font", UiKit.PAPER_BODY_FONT)
		add_theme_font_size_override("font_size", 14)
		add_theme_stylebox_override("normal", UiTemplateBuilder._paper_style(
				UiKit.PAPEL_CLARO, UiKit.TINTA, 12.0, 1, ["all"]))
		add_theme_stylebox_override("hover", UiTemplateBuilder._paper_style(
				UiKit.PAPEL, UiKit.TINTA, 12.0, 1, ["all"]))
		add_theme_stylebox_override("pressed", UiTemplateBuilder._paper_style(
				UiKit.ABA_INATIVA, UiKit.TINTA, 12.0, 1, ["all"]))
		add_theme_stylebox_override("focus", UiKit.outline(UiKit.CARIMBO))
		add_theme_color_override("font_color", UiKit.TINTA)
		add_theme_color_override("font_hover_color", UiKit.TINTA)
		add_theme_color_override("font_pressed_color", UiKit.TINTA)
		UiKit._bind_sfx(self, UiSfx.Sound.TOGGLE)
		pressed.connect(_open)
		_set_text()


	func set_items(source: Array, keep: String = "") -> void:
		items = source.duplicate()
		var wanted := keep if not keep.is_empty() else selected_id
		selected_id = ""
		for item in items:
			if String((item as Dictionary).get("id", "")) == wanted:
				selected_id = wanted
				break
		if selected_id.is_empty() and not items.is_empty():
			selected_id = String((items[0] as Dictionary).get("id", ""))
		_set_text()


	func select(id: String) -> void:
		selected_id = id
		_set_text()


	func _set_text() -> void:
		var label := placeholder
		for item in items:
			if String((item as Dictionary).get("id", "")) == selected_id:
				label = String((item as Dictionary).get("label", placeholder))
				break
		text = label + "  ▾"
		tooltip_text = label


	func _open() -> void:
		if items.is_empty():
			return
		if _menu != null and is_instance_valid(_menu):
			_menu.queue_free()
		_menu = PopupMenu.new()
		_menu.add_theme_font_override("font", UiKit.PAPER_BODY_FONT)
		_menu.add_theme_font_size_override("font_size", 14)
		_menu.add_theme_color_override("font_color", UiKit.TINTA)
		_menu.add_theme_color_override("font_hover_color", UiKit.PAPEL_CLARO)
		_menu.add_theme_stylebox_override("panel", UiTemplateBuilder._paper_style(
				UiKit.PAPEL_CLARO, UiKit.TINTA, 6.0, 1, ["all"]))
		_menu.add_theme_stylebox_override("hover", UiTemplateBuilder._flat(
				UiKit.TINTA, UiKit.TINTA))
		for index in items.size():
			var item: Dictionary = items[index]
			_menu.add_item(String(item.get("label", "")), index)
			if String(item.get("id", "")) == selected_id:
				_menu.set_item_checked(index, true)
		_menu.id_pressed.connect(_on_pick)
		add_child(_menu)
		_menu.popup(Rect2i(Vector2i(global_position + Vector2(0.0, size.y)),
				Vector2i(maxi(int(size.x), 220), 0)))


	func _on_pick(index: int) -> void:
		if index < 0 or index >= items.size():
			return
		selected_id = String((items[index] as Dictionary).get("id", ""))
		_set_text()
		selected.emit(selected_id)


## Abas de pasta com seleção programável: o host troca de aba sem reconstruir a
## tela (ficha de cidade) e recebe `tab_changed` quando o usuário clica.
class FolderTabs extends VBoxContainer:
	signal tab_changed(index: int)

	var active := 0
	var pages: Array[Control] = []
	var buttons: Array[Button] = []

	func select(index: int) -> void:
		if pages.is_empty():
			return
		active = clampi(index, 0, pages.size() - 1)
		for page_index in pages.size():
			pages[page_index].visible = page_index == active
		for button_index in buttons.size():
			UiTemplateBuilder._folder_tab_style(buttons[button_index],
					button_index == active)
		tab_changed.emit(active)


# --- API pública ------------------------------------------------------------

## Nó com `slot` na spec (o host atualiza o valor no lugar, sem reconstruir).
static func slot_node(root: Node, slot: String) -> Control:
	var pending: Array[Node] = [root]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node is Control and (node as Control).get_meta("tpl_slot", "") == slot:
			return node as Control
		for child in node.get_children():
			pending.append(child)
	return null


## Lê a spec de `data/ui/templates/<id>.json`.
static func load_spec(id: String) -> Dictionary:
	var path := "%s/%s.json" % [TEMPLATE_DIR, id]
	if not FileAccess.file_exists(path):
		push_error("UiTemplateBuilder: spec ausente %s" % path)
		return {}
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {}
	var parsed: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(parsed) != TYPE_DICTIONARY:
		push_error("UiTemplateBuilder: spec inválida %s" % path)
		return {}
	return parsed


## Constrói uma tela; `overrides` substitui valores de `spec.data`.
static func build(spec: Dictionary, overrides: Dictionary = {}) -> TemplateRoot:
	var merged := spec.duplicate(true)
	var data: Dictionary = merged.get("data", {})
	for key in overrides:
		data[key] = overrides[key]
	merged["data"] = data

	var ctx := {"data": data, "spec": merged}
	var root := TemplateRoot.new()
	root.name = "Template_%s" % String(merged.get("id", "custom"))
	root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	root.mouse_filter = Control.MOUSE_FILTER_PASS
	root.add_theme_font_override("font", UiKit.BODY_FONT)

	var shell: Dictionary = merged.get("shell", {})
	var kind := String(shell.get("kind", "window"))
	match kind:
		"side_panel":
			root.add_child(_side_panel(merged, ctx))
		"telegram":
			root.add_child(_telegram_shell(merged, ctx))
		"dossier_menu":
			root.add_child(_dossier_shell(merged, ctx))
		"bare":
			root.add_child(_body_column(merged.get("blocks", []), ctx, 12))
		_:
			root.add_child(_window_shell(merged, ctx))
	# Regra do design system: todo controle clicável da tela usa a mão.
	UiKit.pointer_cursor(root)
	return root


## Constrói um bloco isolado (ex.: linha de ranking que o host atualiza no
## lugar). Os valores podem chegar prontos em `data`; nenhuma regra é aplicada.
static func build_block(block: Dictionary, data: Dictionary = {}) -> Control:
	var node := _build_block(block, {"data": data, "spec": {"data": data}})
	UiKit.pointer_cursor(node)
	return node


static func dossie(overrides: Dictionary = {}) -> TemplateRoot:
	return build(load_spec("dossie"), overrides)


static func registro(overrides: Dictionary = {}) -> TemplateRoot:
	return build(load_spec("registro"), overrides)


static func ficha(overrides: Dictionary = {}) -> TemplateRoot:
	return build(load_spec("ficha"), overrides)


static func telegrama(overrides: Dictionary = {}) -> TemplateRoot:
	return build(load_spec("telegrama"), overrides)


static func quadro(overrides: Dictionary = {}) -> TemplateRoot:
	return build(load_spec("quadro"), overrides)


static func menu(overrides: Dictionary = {}) -> TemplateRoot:
	return build(load_spec("menu"), overrides)


## O host (HUD) deve pausar a simulação ao mostrar esta spec.
static func pauses_game(spec: Dictionary) -> bool:
	var shell: Dictionary = spec.get("shell", {})
	return bool(shell.get("pauses_game", false))


# --- Envelopes --------------------------------------------------------------

static func _window_shell(spec: Dictionary, ctx: Dictionary) -> Control:
	var layer := Control.new()
	layer.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	layer.mouse_filter = Control.MOUSE_FILTER_STOP

	layer.add_child(_veil())

	var area := Control.new()
	area.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	area.offset_top = TOP_BAR_CLEARANCE
	area.mouse_filter = Control.MOUSE_FILTER_IGNORE
	layer.add_child(area)

	var center := CenterContainer.new()
	center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	center.mouse_filter = Control.MOUSE_FILTER_IGNORE
	area.add_child(center)

	var shell: Dictionary = spec.get("shell", {})
	var frame := _window_frame(_first_block(spec, "window_frame"), ctx)
	var ratio: Array = shell.get("ratio", [0.86, 0.84])
	var minimum: Array = shell.get("min", [960, 560])
	var maximum: Array = shell.get("max", [1800, 1000])
	frame.custom_minimum_size = Vector2(
			clampf(VIEW_W * float(ratio[0]), float(minimum[0]), float(maximum[0])),
			clampf(VIEW_H * float(ratio[1]), float(minimum[1]), float(maximum[1])))
	center.add_child(frame)
	return layer


static func _side_panel(spec: Dictionary, ctx: Dictionary) -> Control:
	var layer := Control.new()
	layer.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	layer.mouse_filter = Control.MOUSE_FILTER_IGNORE

	var shell: Dictionary = spec.get("shell", {})
	var width := float(shell.get("width", 820.0))
	var top := float(shell.get("top", TOP_BAR_CLEARANCE))

	var column := VBoxContainer.new()
	column.set_anchors_preset(Control.PRESET_RIGHT_WIDE)
	column.offset_left = -width
	column.offset_top = top
	column.offset_right = 0.0
	column.offset_bottom = 0.0
	column.add_theme_constant_override("separation", 0)
	column.mouse_filter = Control.MOUSE_FILTER_STOP
	layer.add_child(column)

	for block in spec.get("blocks", []):
		var node := _build_block(block, ctx)
		if node == null:
			continue
		_apply_flags(node, block)
		column.add_child(node)
	return layer


static func _telegram_shell(spec: Dictionary, ctx: Dictionary) -> Control:
	var layer := Control.new()
	layer.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	layer.mouse_filter = Control.MOUSE_FILTER_STOP
	layer.add_child(_veil())

	var area := Control.new()
	area.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	area.offset_top = TOP_BAR_CLEARANCE
	area.mouse_filter = Control.MOUSE_FILTER_IGNORE
	layer.add_child(area)

	var center := CenterContainer.new()
	center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	center.mouse_filter = Control.MOUSE_FILTER_IGNORE
	area.add_child(center)

	var shell: Dictionary = spec.get("shell", {})
	var size: Array = shell.get("size", [720, 800])
	# O lacre de cera ocupa o canto superior direito do papel; o cabeçalho do
	# telegrama reserva essa faixa para o texto não passar por baixo do disco.
	ctx["close_gutter"] = float(UiKit.SEAL_SIZE_PAPER) + 12.0

	var holder := Control.new()
	holder.custom_minimum_size = Vector2(float(size[0]), float(size[1]))
	holder.size_flags_vertical = Control.SIZE_EXPAND_FILL
	holder.mouse_filter = Control.MOUSE_FILTER_STOP

	var card := PaperPanel.new()
	card.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	card.add_theme_stylebox_override("panel",
			_paper_style(UiKit.PAPEL, UiKit.TINTA, 0.0, 1, ["all"]))
	card.mouse_filter = Control.MOUSE_FILTER_STOP
	holder.add_child(card)

	var body := VBoxContainer.new()
	body.add_theme_constant_override("separation", 0)
	card.add_child(body)
	for block in spec.get("blocks", []):
		var node := _build_block(block, ctx)
		if node == null:
			continue
		_apply_flags(node, block)
		body.add_child(node)

	var seal := float(UiKit.SEAL_SIZE_PAPER)
	var close := UiKit.close_seal_button(UiKit.SEAL_SIZE_PAPER)
	close.set_anchors_and_offsets_preset(Control.PRESET_TOP_RIGHT)
	close.offset_left = -(seal + 14.0)
	close.offset_top = 14.0
	close.offset_right = -14.0
	close.offset_bottom = 14.0 + seal
	close.pressed.connect(func() -> void: _emit_close(close))
	holder.add_child(close)

	var tilt := Tilt.new()
	tilt.angle = float(shell.get("angle", -0.6))
	tilt.add_theme_constant_override("margin_left", 16)
	tilt.add_theme_constant_override("margin_right", 16)
	tilt.add_theme_constant_override("margin_top", 16)
	tilt.add_theme_constant_override("margin_bottom", 16)
	tilt.add_child(holder)
	center.add_child(tilt)
	return layer


static func _dossier_shell(spec: Dictionary, ctx: Dictionary) -> Control:
	var layer := Control.new()
	layer.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	layer.mouse_filter = Control.MOUSE_FILTER_STOP
	layer.add_child(_veil())
	layer.gui_input.connect(func(event: InputEvent) -> void:
		if event is InputEventMouseButton \
				and event.button_index == MOUSE_BUTTON_LEFT and event.pressed:
			_emit_close(layer))

	var area := Control.new()
	area.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	area.offset_top = TOP_BAR_CLEARANCE
	area.mouse_filter = Control.MOUSE_FILTER_IGNORE
	layer.add_child(area)

	var center := CenterContainer.new()
	center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	center.mouse_filter = Control.MOUSE_FILTER_IGNORE
	area.add_child(center)

	var shell: Dictionary = spec.get("shell", {})
	var width := float(shell.get("width", 480.0))
	var height := clampf(float(shell.get("height", 800.0)), 380.0,
			VIEW_H - TOP_BAR_CLEARANCE - 56.0)

	# A folha do menu é a própria ficha de papel: o lacre de cera fica solto no
	# canto superior direito, por cima do papel (52 px, sem empurrar o conteúdo
	# centralizado). O contêiner externo mede a ficha para o CenterContainer.
	var sheet := Control.new()
	sheet.custom_minimum_size = Vector2(width, height)
	sheet.mouse_filter = Control.MOUSE_FILTER_STOP
	center.add_child(sheet)

	var card := VBoxContainer.new()
	card.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	card.add_theme_constant_override("separation", 0)
	card.mouse_filter = Control.MOUSE_FILTER_STOP
	sheet.add_child(card)
	# Faixa superior livre para o lacre: o texto da ficha começa abaixo do disco,
	# nunca por baixo dele.
	card.add_child(_dossier_body(spec, shell, ctx, 88.0))

	var close := UiKit.close_seal_button(UiKit.SEAL_SIZE_PAPER)
	var seal := float(UiKit.SEAL_SIZE_PAPER)
	close.set_anchors_and_offsets_preset(Control.PRESET_TOP_RIGHT)
	close.offset_left = -(seal + 18.0)
	close.offset_top = 18.0
	close.offset_right = -18.0
	close.offset_bottom = 18.0 + seal
	close.pressed.connect(func() -> void: _emit_close(close))
	sheet.add_child(close)
	return layer


## Folha de papel da ficha: começa na borda de cima da ficha (sem a antiga
## etiqueta manila "arquivo do gabinete / dia") — o T6 abre direto no papel.
static func _dossier_body(spec: Dictionary, shell: Dictionary,
		ctx: Dictionary, top_pad: float = 26.0) -> Control:
	var body := PanelContainer.new()
	body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.mouse_filter = Control.MOUSE_FILTER_STOP
	var style := _paper_style(UiKit.PAPEL, UiKit.TINTA, 0.0, 1,
			["top", "left", "right", "bottom"])
	style.content_margin_left = 34.0
	style.content_margin_right = 34.0
	style.content_margin_top = top_pad
	style.content_margin_bottom = 22.0
	body.add_theme_stylebox_override("panel", style)

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 10)
	column.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.add_child(column)

	for block in spec.get("blocks", []):
		var node := _build_block(block, ctx)
		if node == null:
			continue
		_apply_flags(node, block)
		column.add_child(node)

	var footer: Array = shell.get("footer", [])
	if not footer.is_empty():
		column.add_child(_spacer_height(4))
		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", 12)
		for item in footer:
			if bool(item.get("spacer", false)):
				row.add_child(UiKit.spacer())
				continue
			var hint := _typed(_text(item, "text", ctx), 12, UiKit.TINTA_SUAVE)
			hint.size_flags_vertical = Control.SIZE_SHRINK_CENTER
			row.add_child(hint)
		column.add_child(row)
	return body


static func _veil() -> ColorRect:
	var veil := ColorRect.new()
	veil.color = UiKit.PAPER_VEU
	veil.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	veil.offset_top = TOP_BAR_CLEARANCE
	veil.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return veil


static func _window_frame(block: Dictionary, ctx: Dictionary) -> Control:
	var frame := PanelContainer.new()
	frame.add_theme_stylebox_override("panel",
			_paper_style(UiKit.BARRA_FUNDO, UiKit.BARRA_FILETE, 0.0, 1, ["all"]))
	frame.mouse_filter = Control.MOUSE_FILTER_STOP

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 0)
	frame.add_child(column)

	# Cabeçalho escuro: breadcrumb, título, selo e fechar.
	var header_wrap := MarginContainer.new()
	header_wrap.custom_minimum_size.y = 56.0
	header_wrap.add_theme_constant_override("margin_left", 20)
	header_wrap.add_theme_constant_override("margin_right", 20)
	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", 16)
	header_wrap.add_child(header)
	column.add_child(header_wrap)

	var crumb := _typed(_text(block, "crumb", ctx), 13, UiKit.BARRA_ROTULO)
	crumb.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	_tag_slot(crumb, block, "crumb_slot")
	header.add_child(crumb)

	var title := _display(_text(block, "title", ctx), 24, UiKit.BARRA_TEXTO)
	title.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	title.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	_tag_slot(title, block, "title_slot")
	header.add_child(title)

	header.add_child(UiKit.spacer())

	var seal_text := _text(block, "seal", ctx)
	if not seal_text.is_empty():
		var seal := _typed(seal_text, 12, UiKit.BARRA_ROTULO)
		seal.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		header.add_child(seal)

	var close := UiKit.close_seal_button(UiKit.SEAL_SIZE_BAR)
	close.pressed.connect(func() -> void: _emit_close(close))
	header.add_child(close)

	column.add_child(_double_rule(UiKit.BARRA_FILETE))

	# Corpo em papel. A textura procedural tem o tom de PAPEL (#e6dbbd);
	# sem modulate, as fichas internas são PAPEL_CLARO (#efe6cc) sobre
	# PAPEL com borda TINTA, como nos mockups HTML.
	var style := UiKit.paper_stylebox()
	style.content_margin_left = 16.0
	style.content_margin_right = 16.0
	style.content_margin_top = 16.0
	style.content_margin_bottom = 16.0
	var body := PanelContainer.new()
	body.add_theme_stylebox_override("panel", style)
	body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.mouse_filter = Control.MOUSE_FILTER_PASS
	body.add_child(_body_column(block.get("body", []), ctx, 16))
	column.add_child(body)

	# Rodapé de atalhos com selo ESC.
	column.add_child(_line(UiKit.BARRA_FILETE, 1.0))
	var footer_wrap := MarginContainer.new()
	footer_wrap.custom_minimum_size.y = 34.0
	footer_wrap.add_theme_constant_override("margin_left", 20)
	footer_wrap.add_theme_constant_override("margin_right", 20)
	var footer := HBoxContainer.new()
	footer.add_theme_constant_override("separation", 18)
	footer_wrap.add_child(footer)
	column.add_child(footer_wrap)

	for item in block.get("footer", []):
		if bool(item.get("spacer", false)):
			footer.add_child(UiKit.spacer())
			continue
		if item.has("seal"):
			footer.add_child(_seal(String(item["seal"])))
			continue
		var hint := _typed(_text(item, "text", ctx), 12, UiKit.BARRA_ROTULO)
		hint.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		footer.add_child(hint)

	return frame


static func _body_column(blocks: Array, ctx: Dictionary, gap: int) -> VBoxContainer:
	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", gap)
	column.size_flags_vertical = Control.SIZE_EXPAND_FILL
	column.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for block in blocks:
		var node := _build_block(block, ctx)
		if node == null:
			continue
		_apply_flags(node, block)
		column.add_child(node)
	return column


# --- Blocos -----------------------------------------------------------------

static func _build_block(block: Dictionary, ctx: Dictionary) -> Control:
	if block.is_empty():
		return null
	match String(block.get("type", "")):
		"row":
			return _layout(block, ctx, true)
		"column":
			return _layout(block, ctx, false)
		"grid":
			var grid := GridContainer.new()
			grid.columns = maxi(int(block.get("columns", 2)), 1)
			grid.add_theme_constant_override("h_separation",
					int(block.get("gap", 12)))
			grid.add_theme_constant_override("v_separation",
					int(block.get("gap", 12)))
			for child in block.get("blocks", []):
				var node := _build_block(child, ctx)
				if node == null:
					continue
				_apply_flags(node, child)
				grid.add_child(node)
			return grid
		"scroll":
			return _scroll(block, ctx)
		"spacer":
			var spacer := Control.new()
			spacer.mouse_filter = Control.MOUSE_FILTER_IGNORE
			spacer.custom_minimum_size = Vector2(float(block.get("width", 0.0)),
					float(block.get("height", 0.0)))
			return spacer
		"rule":
			return _rule_block(block)
		"window_frame":
			return _window_frame(block, ctx)
		"paper_card":
			return _paper_card(block, ctx)
		"note_card":
			return _note_card(block, ctx)
		"kpi_card":
			return _kpi_card(block, ctx)
		"meter_row":
			return _meter_row(block, ctx)
		"kv_row":
			return _kv_row(block, ctx)
		"ruled_heading":
			return _ruled_heading(_text(block, "text", ctx),
					float(block.get("size", 20.0)))
		"stamp":
			return _stamp(_text(block, "text", ctx),
					float(block.get("size", 20.0)),
					float(block.get("angle", -3.0)),
					String(block.get("align", "start")))
		"tag":
			return _tag(_text(block, "text", ctx),
					String(block.get("tone", "neutral")))
		"tag_row":
			var tags := HBoxContainer.new()
			tags.add_theme_constant_override("separation", 8)
			for item in block.get("tags", []):
				var text := _text(item, "text", ctx)
				if text.is_empty():
					continue
				var tag := _tag(text, String(item.get("tone", "neutral")))
				_tag_slot(tag, item, "slot")
				tags.add_child(tag)
			return tags
		"chip_filter":
			var chip := _chip(_text(block, "text", ctx),
					bool(block.get("active", false)))
			return chip
		"select_field":
			var select := PaperSelect.new()
			select.custom_minimum_size.x = float(block.get("width", 260.0))
			select.placeholder = _text(block, "placeholder", ctx)
			select._set_text()
			return select
		"search_field":
			return _search(block, ctx)
		"button":
			return _paper_action(block, ctx)
		"menu_button":
			return _menu_button(block, ctx)
		"data_table":
			return _data_table(block, ctx)
		"folder_tabs":
			return _folder_tabs(block, ctx)
		"bar_chart":
			return _bar_chart(block)
		"line_chart":
			return _line_chart(block)
		"choice_button":
			return _choice_button(block, ctx)
		"telegram_card":
			return _telegram_card(block, ctx)
		"ranking_row":
			return _ranking_row(block, ctx)
		"flag":
			return _flag(block, ctx)
		"label":
			return _text_label(block, ctx)
		"heading_row":
			return _heading_row(block, ctx)
	return null


static func _layout(block: Dictionary, ctx: Dictionary, horizontal: bool) -> Control:
	var box: BoxContainer = HBoxContainer.new() if horizontal \
			else VBoxContainer.new()
	box.add_theme_constant_override("separation", int(block.get("gap", 12)))
	for child in block.get("blocks", []):
		var node := _build_block(child, ctx)
		if node == null:
			continue
		_apply_flags(node, child)
		box.add_child(node)
	return box


static func _scroll(block: Dictionary, ctx: Dictionary) -> Control:
	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	if block.has("height"):
		scroll.custom_minimum_size.y = float(block["height"])
	var column := _body_column(block.get("blocks", []), ctx,
			int(block.get("gap", 12)))
	column.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(column)
	UiKit.bind_scroll_sfx(scroll)
	return scroll


static func _rule_block(block: Dictionary) -> Control:
	var color := _tone(String(block.get("color", "tinta")))
	var thickness := float(block.get("thickness", 1.0))
	if bool(block.get("double", false)):
		return _double_rule(color)
	return _line(color, thickness,
			bool(block.get("dashed", false)))


static func _paper_card(block: Dictionary, ctx: Dictionary) -> Control:
	var panel := PaperPanel.new()
	panel.dashed = String(block.get("border", "solid")) == "dashed"
	panel.dash_color = _tone(String(block.get("border_color", "tinta")))
	var style := _paper_style(_fill(String(block.get("fill", "claro"))),
			_tone(String(block.get("border_color", "tinta"))),
			float(block.get("pad", 18.0)), 1,
			block.get("edges", ["all"]) as Array)
	if panel.dashed:
		style.set_border_width_all(0)
	panel.add_theme_stylebox_override("panel", style)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", int(block.get("gap", 10)))
	panel.add_child(box)

	if block.has("title"):
		box.add_child(_ruled_heading(_text(block, "title", ctx),
				float(block.get("title_size", 20.0))))

	for child in block.get("blocks", []):
		var node := _build_block(child, ctx)
		if node == null:
			continue
		_apply_flags(node, child)
		box.add_child(node)
	return panel


## Nota em papel: título curto + corpo datilografado (abas ainda sem dados).
static func _note_card(block: Dictionary, ctx: Dictionary) -> Control:
	var panel := PaperPanel.new()
	panel.dashed = String(block.get("border", "dashed")) == "dashed"
	var style := _paper_style(_fill(String(block.get("fill", "papel"))),
			UiKit.TINTA_SUAVE, float(block.get("pad", 18.0)), 1, ["all"])
	if panel.dashed:
		style.set_border_width_all(0)
	panel.add_theme_stylebox_override("panel", style)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 8)
	panel.add_child(box)

	var title := _display(_text(block, "title", ctx), 22, UiKit.TINTA)
	box.add_child(title)

	var body := _typed(_text(block, "body", ctx), 14, UiKit.TINTA_SUAVE)
	body.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	body.custom_minimum_size.x = float(block.get("width", 540.0))
	box.add_child(body)
	return panel


static func _kpi_card(block: Dictionary, ctx: Dictionary) -> Control:
	var panel := PaperPanel.new()
	var style := _paper_style(_fill(String(block.get("fill", "claro"))),
			UiKit.TINTA, 0.0, 1, ["all"])
	style.content_margin_left = 16.0
	style.content_margin_right = 16.0
	style.content_margin_top = 14.0
	style.content_margin_bottom = 14.0
	panel.add_theme_stylebox_override("panel", style)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 4)
	panel.add_child(box)

	var caption := _typed(_text(block, "label", ctx).to_upper(), 12,
			UiKit.TINTA_SUAVE)
	box.add_child(caption)

	var value := _display(_text(block, "value", ctx),
			float(block.get("size", 30.0)), _tone(String(block.get("value_tone",
					"tinta"))))
	_tag_slot(value, block, "value_slot")
	box.add_child(value)

	var delta := _text(block, "delta", ctx)
	if not delta.is_empty():
		var line := _body_label(delta, float(block.get("delta_size", 14.0)),
				_tone(String(block.get("tone", "positive"))))
		_tag_slot(line, block, "delta_slot")
		box.add_child(line)
	return panel


static func _meter_row(block: Dictionary, ctx: Dictionary) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 10)

	var caption := _body_label(_text(block, "label", ctx),
			float(block.get("label_size", 16.0)), UiKit.TINTA)
	caption.custom_minimum_size.x = float(block.get("label_width", 110.0))
	caption.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	row.add_child(caption)

	var ratio := float(block.get("value", 0.0))
	if ratio > 1.0:
		ratio /= 100.0
	var bar := MeterBar.new(ratio, _tone(String(block.get("tone", "tinta"))),
			float(block.get("height", 10.0)))
	_tag_slot(bar, block, "meter_slot")
	row.add_child(bar)

	var value := _body_label(_text(block, "text", ctx),
			float(block.get("value_size", 16.0)), UiKit.TINTA)
	_tag_slot(value, block, "value_slot")
	value.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	value.custom_minimum_size.x = float(block.get("value_width", 60.0))
	value.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	row.add_child(value)
	return row


static func _kv_row(block: Dictionary, ctx: Dictionary) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)

	var key := _body_label(_text(block, "key", ctx),
			float(block.get("size", 17.0)), UiKit.TINTA_SUAVE)
	row.add_child(key)
	row.add_child(DottedLeader.new())

	var value := _body_label(_text(block, "value", ctx),
			float(block.get("size", 17.0)),
			_tone(String(block.get("tone", "tinta"))))
	_tag_slot(value, block, "value_slot")
	value.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	value.custom_minimum_size.x = float(block.get("value_width", 0.0))
	row.add_child(value)
	return row


static func _ruled_heading(text: String, size: float) -> Control:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	var heading := _display(text, size, UiKit.TINTA)
	box.add_child(heading)
	box.add_child(_line(UiKit.TINTA, 2.0))
	return box


static func _heading_row(block: Dictionary, ctx: Dictionary) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 12)
	var heading := _display(_text(block, "text", ctx),
			float(block.get("size", 20.0)), UiKit.TINTA)
	row.add_child(heading)
	row.add_child(UiKit.spacer())
	var trailing := _text(block, "trailing", ctx)
	if not trailing.is_empty():
		var label := _typed(trailing, 13, UiKit.TINTA_SUAVE)
		label.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		row.add_child(label)
	return row


static func _stamp(text: String, size: float, angle: float,
		align: String) -> Control:
	var holder := HBoxContainer.new()
	holder.alignment = BoxContainer.ALIGNMENT_BEGIN
	if align == "center":
		holder.alignment = BoxContainer.ALIGNMENT_CENTER
	elif align == "end":
		holder.alignment = BoxContainer.ALIGNMENT_END

	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel",
			_paper_style(Color(0, 0, 0, 0), UiKit.CARIMBO, 0.0, 2, ["all"]))
	var style := panel.get_theme_stylebox("panel") as StyleBoxFlat
	style.content_margin_left = 12.0
	style.content_margin_right = 12.0
	style.content_margin_top = 6.0
	style.content_margin_bottom = 6.0

	var label := _display(text, size, UiKit.CARIMBO)
	panel.add_child(label)

	var tilt := Tilt.new()
	tilt.angle = angle
	tilt.add_theme_constant_override("margin_left", 12)
	tilt.add_theme_constant_override("margin_right", 12)
	tilt.add_theme_constant_override("margin_top", 10)
	tilt.add_theme_constant_override("margin_bottom", 10)
	tilt.add_child(panel)
	holder.add_child(tilt)
	return holder


static func _tag(text: String, tone: String) -> Control:
	var panel := PanelContainer.new()
	var color := UiKit.TINTA
	if tone == "carimbo":
		color = UiKit.CARIMBO
	elif tone == "suave":
		color = UiKit.TINTA_SUAVE
	var style := _paper_style(Color(0, 0, 0, 0), color, 0.0, 1, ["all"])
	style.content_margin_left = 8.0
	style.content_margin_right = 8.0
	style.content_margin_top = 2.0
	style.content_margin_bottom = 2.0
	panel.add_theme_stylebox_override("panel", style)
	panel.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	var label := _typed(text.to_upper(), 12, color)
	panel.add_child(label)
	return panel


static func _chip(text: String, active: bool) -> Button:
	var button := Button.new()
	button.text = text
	button.focus_mode = Control.FOCUS_ALL
	button.custom_minimum_size.y = 38.0
	button.add_theme_font_override("font", UiKit.PAPER_BODY_FONT)
	button.add_theme_font_size_override("font_size", 14)
	button.add_theme_constant_override("outline_size", 0)
	set_chip_active(button, active)
	UiKit._bind_sfx(button, UiSfx.Sound.TOGGLE)
	return button


## Estado ativo/inativo do `chip_filter` sem recriar o botão (filtros mudam).
static func set_chip_active(button: Button, active: bool) -> void:
	button.add_theme_stylebox_override("normal", _paper_style(
			UiKit.TINTA if active else UiKit.PAPEL_CLARO,
			UiKit.TINTA, 14.0, 1, ["all"]))
	button.add_theme_stylebox_override("hover", _paper_style(
			UiKit.TINTA if active else UiKit.PAPEL,
			UiKit.TINTA, 14.0, 1, ["all"]))
	button.add_theme_stylebox_override("pressed", _paper_style(
			UiKit.TINTA, UiKit.CARIMBO, 14.0, 1, ["all"]))
	button.add_theme_stylebox_override("focus",
			UiKit.outline(UiKit.CARIMBO))
	button.add_theme_color_override("font_color",
			UiKit.PAPEL_CLARO if active else UiKit.TINTA)
	button.add_theme_color_override("font_hover_color",
			UiKit.PAPEL_CLARO if active else UiKit.TINTA)


static func _search(block: Dictionary, ctx: Dictionary) -> Control:
	var panel := PanelContainer.new()
	var style := _paper_style(UiKit.PAPEL_CLARO, UiKit.TINTA, 0.0, 1, ["all"])
	style.content_margin_left = 12.0
	style.content_margin_right = 12.0
	panel.add_theme_stylebox_override("panel", style)
	panel.custom_minimum_size = Vector2(float(block.get("width", 340.0)), 38.0)

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	panel.add_child(row)

	var glyph := _typed("⌕", 16, UiKit.TINTA_SUAVE)
	glyph.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	row.add_child(glyph)

	var field := LineEdit.new()
	field.placeholder_text = _text(block, "placeholder", ctx)
	field.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	field.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	field.add_theme_font_override("font", UiKit.PAPER_BODY_FONT)
	field.add_theme_font_size_override("font_size", 14)
	field.add_theme_color_override("font_color", UiKit.TINTA)
	field.add_theme_color_override("font_placeholder_color", UiKit.TINTA_SUAVE)
	field.add_theme_color_override("caret_color", UiKit.CARIMBO)
	field.add_theme_stylebox_override("normal",
			_flat(Color(0, 0, 0, 0), Color(0, 0, 0, 0)))
	field.add_theme_stylebox_override("focus",
			_flat(Color(0, 0, 0, 0), Color(0, 0, 0, 0)))
	field.focus_entered.connect(func() -> void:
		panel.add_theme_stylebox_override("panel",
				_paper_style(UiKit.PAPEL_CLARO, UiKit.CARIMBO, 0.0, 2, ["all"])))
	field.focus_exited.connect(func() -> void:
		panel.add_theme_stylebox_override("panel",
				_paper_style(UiKit.PAPEL_CLARO, UiKit.TINTA, 0.0, 1, ["all"])))
	row.add_child(field)
	return panel


static func _paper_action(block: Dictionary, ctx: Dictionary) -> Button:
	var text := _text(block, "text", ctx)
	var variant := String(block.get("variant", "paper"))
	var button := Button.new()
	button.text = text
	button.custom_minimum_size.y = 44.0 if variant != "paper" else 38.0
	button.focus_mode = Control.FOCUS_ALL
	button.add_theme_font_override("font",
			UiKit.PAPER_DISPLAY_FONT if variant != "paper" \
			else UiKit.PAPER_BODY_FONT)
	button.add_theme_font_size_override("font_size",
			16 if variant != "paper" else 14)
	button.add_theme_stylebox_override("focus", UiKit.outline(UiKit.CARIMBO))

	var primary := variant == "primary"
	var ghost := variant == "ghost"
	var normal_fill := UiKit.TINTA if primary else Color(0, 0, 0, 0) if ghost \
			else UiKit.PAPEL_CLARO
	var normal_text := UiKit.PAPEL_CLARO if primary else UiKit.TINTA_SUAVE \
			if ghost else UiKit.TINTA
	button.add_theme_stylebox_override("normal",
			_paper_style(normal_fill, UiKit.TINTA, 22.0, 0 if ghost else 1, ["all"]))
	button.add_theme_stylebox_override("hover", _paper_style(
			UiKit.PAPEL if not primary else UiKit.BARRA_FUNDO,
			UiKit.TINTA, 22.0, 1, ["all"]))
	button.add_theme_stylebox_override("pressed",
			_paper_style(UiKit.ABA_INATIVA, UiKit.TINTA, 22.0, 1, ["all"]))
	# Sem este override o estado desabilitado cai no StyleBox escuro do tema
	# padrão e o botão vira um bloco preto sobre o papel.
	button.add_theme_stylebox_override("disabled",
			_paper_style(UiKit.PAPEL, Color(UiKit.TINTA, 0.45), 22.0, 1, ["all"]))
	button.add_theme_color_override("font_color", normal_text)
	button.add_theme_color_override("font_hover_color",
			UiKit.PAPEL_CLARO if primary else UiKit.TINTA)
	UiKit._bind_sfx(button)

	if bool(block.get("disabled", false)):
		button.disabled = true
		button.focus_mode = Control.FOCUS_NONE
		button.add_theme_color_override("font_disabled_color", UiKit.TINTA_SUAVE)
	var tip := _text(block, "tooltip", ctx)
	if not tip.is_empty():
		button.tooltip_text = tip
	var id := String(block.get("id", ""))
	if not id.is_empty():
		button.pressed.connect(func() -> void: _emit_choice(button, id))
	return button


## Botão de ficha (T6): rótulo à esquerda, atalho em Special Elite à direita.
static func _menu_button(block: Dictionary, ctx: Dictionary) -> Button:
	var variant := String(block.get("variant", "paper"))
	var primary := variant == "primary"
	var quit := variant == "quit"
	var button := Button.new()
	button.custom_minimum_size.y = float(block.get("height", 50.0))
	button.focus_mode = Control.FOCUS_ALL
	button.add_theme_stylebox_override("focus", UiKit.outline(UiKit.CARIMBO))

	var normal_fill := UiKit.PAPEL_CLARO
	var border := UiKit.TINTA
	var border_width := 1
	var text_color := UiKit.TINTA
	var hint_color := UiKit.TINTA_SUAVE
	if primary:
		normal_fill = UiKit.TINTA
		border = UiKit.TINTA
		text_color = UiKit.PAPEL_CLARO
		hint_color = UiKit.BARRA_FILETE
	elif quit:
		border = UiKit.CARIMBO
		border_width = 2
		text_color = UiKit.CARIMBO
		hint_color = UiKit.CARIMBO

	var hover_fill := UiKit.BARRA_FUNDO if primary else UiKit.PAPEL
	button.add_theme_stylebox_override("normal",
			_paper_style(normal_fill, border, 0.0, border_width, ["all"]))
	button.add_theme_stylebox_override("hover",
			_paper_style(hover_fill, border, 0.0, border_width, ["all"]))
	button.add_theme_stylebox_override("pressed",
			_paper_style(UiKit.ABA_INATIVA, UiKit.TINTA, 0.0, 1, ["all"]))
	button.add_theme_stylebox_override("disabled",
			_paper_style(UiKit.PAPEL, Color(UiKit.TINTA, 0.35), 0.0, 1, ["all"]))
	button.add_theme_color_override("font_color", text_color)
	button.add_theme_color_override("font_hover_color",
			UiKit.PAPEL_CLARO if primary else text_color)
	button.add_theme_color_override("font_disabled_color", UiKit.TINTA_SUAVE)

	var enabled := bool(block.get("enabled", true))
	if not enabled:
		text_color = Color(UiKit.TINTA_SUAVE, 0.72)
		hint_color = Color(UiKit.TINTA_SUAVE, 0.72)
	button.disabled = not enabled
	if not enabled:
		button.focus_mode = Control.FOCUS_NONE
	var tip := _text(block, "tooltip", ctx)
	if not tip.is_empty():
		button.tooltip_text = tip

	var margin := MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	margin.add_theme_constant_override("margin_left", 18)
	margin.add_theme_constant_override("margin_right", 18)
	margin.add_theme_constant_override("margin_top", 2)
	margin.add_theme_constant_override("margin_bottom", 2)
	margin.mouse_filter = Control.MOUSE_FILTER_IGNORE
	button.add_child(margin)

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 10)
	row.mouse_filter = Control.MOUSE_FILTER_IGNORE
	margin.add_child(row)

	var title := _display(_text(block, "text", ctx), 19 if primary else 18,
			text_color)
	title.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	title.mouse_filter = Control.MOUSE_FILTER_IGNORE
	row.add_child(title)

	row.add_child(UiKit.spacer())

	var hint := _text(block, "hint", ctx)
	if not hint.is_empty():
		var hint_label := _typed(hint, 12, hint_color)
		hint_label.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		hint_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
		hint_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
		row.add_child(hint_label)

	var id := String(block.get("id", ""))
	button.pressed.connect(func() -> void: _emit_choice(button, id))
	UiKit._bind_sfx(button, UiSfx.Sound.SELECT)
	return button


static func _data_table(block: Dictionary, ctx: Dictionary) -> Control:
	var table := DataTableView.new()
	table.setup(block, ctx)
	table.tooltip_text = ""
	return table


static func _folder_tabs(block: Dictionary, ctx: Dictionary) -> Control:
	var root := FolderTabs.new()
	root.add_theme_constant_override("separation", 0)
	root.size_flags_vertical = Control.SIZE_EXPAND_FILL
	root.mouse_filter = Control.MOUSE_FILTER_PASS

	var has_close := bool(block.get("close", false))
	var bar_wrap := MarginContainer.new()
	# Com o lacre de cera (52 px, sobre papel) a barra das abas cresce o
	# suficiente para o disco caber inteiro sem apertar as abas.
	bar_wrap.custom_minimum_size.y = 58.0 if has_close else 48.0
	bar_wrap.add_theme_constant_override("margin_left", 24)
	bar_wrap.add_theme_constant_override("margin_right", 16)
	var bar := HBoxContainer.new()
	bar.add_theme_constant_override("separation", 4)
	bar.alignment = BoxContainer.ALIGNMENT_END
	bar_wrap.add_child(bar)
	root.add_child(bar_wrap)

	var body := PaperPanel.new()
	var style := _paper_style(UiKit.PAPEL, UiKit.TINTA, 0.0, 1, ["top", "left"])
	style.content_margin_left = 28.0
	style.content_margin_right = 28.0
	style.content_margin_top = 24.0
	style.content_margin_bottom = 24.0
	body.add_theme_stylebox_override("panel", style)
	body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.mouse_filter = Control.MOUSE_FILTER_PASS
	root.add_child(body)

	# `body` é um PanelContainer e só posiciona UM filho: cabeçalho e páginas
	# precisam de uma coluna única, senão o cabeçalho sobrepõe o conteúdo da aba.
	var stack := VBoxContainer.new()
	stack.add_theme_constant_override("separation",
			int(block.get("header_gap", 10)))
	stack.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.add_child(stack)

	# Cabeçalho fixo (trilha/voltar), acima das páginas e dentro do papel.
	var header: Array = block.get("header", [])
	if not header.is_empty():
		var header_column := _body_column(header, ctx, int(block.get("header_gap", 10)))
		header_column.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
		stack.add_child(header_column)

	var tabs: Array = block.get("tabs", [])
	var active := int(block.get("active", 0))

	for index in tabs.size():
		var tab: Dictionary = tabs[index]
		var page := _body_column(tab.get("blocks", []), ctx,
				int(block.get("gap", 18)))
		page.visible = index == active
		stack.add_child(page)
		root.pages.append(page)
		var button := _folder_tab_button(_text(tab, "label", ctx),
				index == active)
		button.pressed.connect(root.select.bind(index))
		if index == active:
			# A aba ativa é o foco natural ao abrir a ficha.
			button.set_meta(UiKit.UI_FOCUS_PREFERRED, true)
		bar.add_child(button)
		root.buttons.append(button)

	root.active = clampi(active, 0, maxi(root.pages.size() - 1, 0))

	if has_close:
		bar.add_child(UiKit.spacer())
		var close := UiKit.close_seal_button(UiKit.SEAL_SIZE_PAPER)
		close.pressed.connect(func() -> void: _emit_close(close))
		bar.add_child(close)

	return root


static func _folder_tab_button(text: String, active: bool) -> Button:
	var button := Button.new()
	button.text = text
	button.focus_mode = Control.FOCUS_ALL
	_folder_tab_style(button, active)
	return button


static func _folder_tab_style(button: Button, active: bool) -> void:
	button.custom_minimum_size.y = 46.0 if active else 38.0
	button.add_theme_font_override("font",
			UiKit.PAPER_DISPLAY_FONT if active else UiKit.PAPER_BODY_FONT)
	button.add_theme_font_size_override("font_size", 16 if active else 14)
	var style := _paper_style(UiKit.PAPEL_CLARO if active else UiKit.ABA_INATIVA,
			UiKit.TINTA, 20.0, 1, ["top", "left", "right"])
	style.corner_radius_top_left = 8
	style.corner_radius_top_right = 8
	button.add_theme_stylebox_override("normal", style)
	var hover := _paper_style(UiKit.PAPEL, UiKit.TINTA, 20.0, 1,
			["top", "left", "right"])
	hover.corner_radius_top_left = 8
	hover.corner_radius_top_right = 8
	button.add_theme_stylebox_override("hover", hover)
	button.add_theme_stylebox_override("pressed",
			_paper_style(UiKit.PAPEL_CLARO, UiKit.CARIMBO, 20.0, 1,
					["top", "left", "right"]))
	button.add_theme_stylebox_override("focus", UiKit.outline(UiKit.CARIMBO))
	button.add_theme_color_override("font_color",
			UiKit.TINTA if active else UiKit.TINTA_SUAVE)
	button.add_theme_color_override("font_hover_color", UiKit.TINTA)
	button.add_theme_color_override("font_focus_color",
			UiKit.TINTA if active else UiKit.TINTA_SUAVE)
	button.add_theme_color_override("font_pressed_color", UiKit.TINTA)
	button.add_theme_color_override("font_hover_pressed_color", UiKit.TINTA)
	button.add_theme_constant_override("outline_size", 0)


static func _bar_chart(block: Dictionary) -> Control:
	var chart := BarChart.new()
	chart.custom_minimum_size.y = float(block.get("height", 140.0))
	chart.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	chart.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	chart.gap = float(block.get("bar_gap", 6.0))
	chart.scale_max = float(block.get("scale_max", 0.0))
	for value in block.get("values", []):
		chart.values.append(float(value))
	for tone in block.get("tones", []):
		chart.tones.append(_tone(String(tone)))
	return chart


static func _line_chart(block: Dictionary) -> Control:
	var chart := LineChart.new()
	chart.custom_minimum_size.y = float(block.get("height", 160.0))
	chart.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	chart.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	chart.line_color = _tone(String(block.get("tone", "tinta")))
	chart.autoscale = bool(block.get("autoscale", false))
	for value in block.get("values", []):
		chart.values.append(float(value))
	return chart


static func _choice_button(block: Dictionary, ctx: Dictionary) -> Button:
	var button := Button.new()
	button.custom_minimum_size.y = float(block.get("height", 64.0))
	button.focus_mode = Control.FOCUS_ALL
	button.add_theme_stylebox_override("normal",
			_paper_style(UiKit.PAPEL_CLARO, UiKit.TINTA, 0.0, 1, ["all"]))
	button.add_theme_stylebox_override("hover",
			_paper_style(UiKit.PAPEL, UiKit.TINTA, 0.0, 1, ["all"]))
	button.add_theme_stylebox_override("pressed",
			_paper_style(UiKit.ABA_INATIVA, UiKit.CARIMBO, 0.0, 1, ["all"]))
	button.add_theme_stylebox_override("focus", UiKit.outline(UiKit.CARIMBO))
	UiKit._bind_sfx(button, UiSfx.Sound.SELECT)

	var margin := MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	margin.add_theme_constant_override("margin_left", 16)
	margin.add_theme_constant_override("margin_right", 16)
	margin.add_theme_constant_override("margin_top", 10)
	margin.add_theme_constant_override("margin_bottom", 10)
	margin.mouse_filter = Control.MOUSE_FILTER_IGNORE
	button.add_child(margin)

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 2)
	column.alignment = BoxContainer.ALIGNMENT_CENTER
	column.mouse_filter = Control.MOUSE_FILTER_IGNORE
	margin.add_child(column)

	var title := _display(_text(block, "title", ctx), 17, UiKit.TINTA)
	title.mouse_filter = Control.MOUSE_FILTER_IGNORE
	column.add_child(title)

	var consequence := _body_label(_text(block, "consequence", ctx), 15,
			UiKit.TINTA_SUAVE)
	consequence.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	consequence.custom_minimum_size.x = float(block.get("note_width", 560.0))
	consequence.mouse_filter = Control.MOUSE_FILTER_IGNORE
	column.add_child(consequence)

	var id := String(block.get("id", ""))
	button.pressed.connect(func() -> void: _emit_choice(button, id))
	return button


static func _telegram_card(block: Dictionary, ctx: Dictionary) -> Control:
	var panel := PaperPanel.new()
	panel.add_theme_stylebox_override("panel",
			_paper_style(Color(0, 0, 0, 0), UiKit.TINTA, 0.0, 0, ["all"]))
	panel.size_flags_vertical = Control.SIZE_EXPAND_FILL

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 0)
	panel.add_child(column)

	# Faixa manila superior.
	var strip := PanelContainer.new()
	var strip_style := _paper_style(UiKit.MANILA, UiKit.MANILA_VIGNETTE, 0.0, 0,
			["all"])
	strip_style.content_margin_top = 7.0
	strip_style.content_margin_bottom = 7.0
	strip.add_theme_stylebox_override("panel", strip_style)
	strip.custom_minimum_size.y = 14.0
	strip.mouse_filter = Control.MOUSE_FILTER_IGNORE
	column.add_child(strip)
	column.add_child(_line(UiKit.MANILA_VIGNETTE, 1.0, true))

	var inner := MarginContainer.new()
	inner.add_theme_constant_override("margin_left", 40)
	inner.add_theme_constant_override("margin_right", 40)
	inner.add_theme_constant_override("margin_top", 26)
	inner.add_theme_constant_override("margin_bottom", 26)
	inner.size_flags_vertical = Control.SIZE_EXPAND_FILL
	column.add_child(inner)

	var content := VBoxContainer.new()
	content.add_theme_constant_override("separation", 0)
	inner.add_child(content)

	var head := HBoxContainer.new()
	head.add_theme_constant_override("separation", 16)
	var gutter := float(ctx.get("close_gutter", 0.0))
	if gutter > 0.0:
		# Faixa reservada ao lacre de cera no canto do papel.
		var head_wrap := MarginContainer.new()
		head_wrap.add_theme_constant_override("margin_right", int(gutter))
		head_wrap.add_child(head)
		content.add_child(head_wrap)
	else:
		content.add_child(head)
	var title := _display(_text(block, "title", ctx), 30, UiKit.TINTA)
	head.add_child(title)
	var header_rule := Hairline.new(UiKit.TINTA, 2.0)
	header_rule.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header_rule.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	head.add_child(header_rule)
	var meta := _typed(_text(block, "meta", ctx), 13, UiKit.TINTA_SUAVE)
	meta.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	head.add_child(meta)

	content.add_child(_spacer_height(14))

	# DE / PARA / ASSUNTO.
	var grid := GridContainer.new()
	grid.columns = 2
	grid.add_theme_constant_override("h_separation", 12)
	grid.add_theme_constant_override("v_separation", 6)
	content.add_child(grid)
	for field in block.get("fields", []):
		var key := _typed(_text(field, "key", ctx), 15, UiKit.TINTA_SUAVE)
		key.custom_minimum_size.x = 120.0
		grid.add_child(key)
		grid.add_child(_typed(_text(field, "value", ctx), 15, UiKit.TINTA))

	content.add_child(_spacer_height(18))
	content.add_child(_line(UiKit.TINTA, 1.0))
	content.add_child(_spacer_height(18))

	var body := _typed(_text(block, "body", ctx), 18, UiKit.TINTA)
	body.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	body.custom_minimum_size.x = float(block.get("body_width", 540.0))
	body.add_theme_constant_override("line_spacing", 7)
	content.add_child(body)

	var choices: Array = block.get("choices", [])
	if not choices.is_empty():
		content.add_child(_spacer_height(22))
		var caption := _typed(_text(block, "choice_label", ctx).to_upper(), 12,
				UiKit.TINTA_SUAVE)
		content.add_child(caption)
		content.add_child(_spacer_height(10))
		var list := VBoxContainer.new()
		list.add_theme_constant_override("separation", 10)
		content.add_child(list)
		for choice in choices:
			var node := _choice_button(choice, ctx)
			list.add_child(node)

	content.add_child(UiKit.spacer())

	var foot := HBoxContainer.new()
	foot.add_theme_constant_override("separation", 16)
	content.add_child(foot)
	var note := _typed(_text(block, "footer", ctx), 12, UiKit.TINTA_SUAVE)
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.custom_minimum_size.x = float(block.get("footer_width", 380.0))
	note.size_flags_vertical = Control.SIZE_SHRINK_END
	foot.add_child(note)
	foot.add_child(UiKit.spacer())
	var stamp_text := _text(block, "stamp", ctx)
	if not stamp_text.is_empty():
		foot.add_child(_stamp(stamp_text, 16, -8, "end"))
	return panel


static func _ranking_row(block: Dictionary, ctx: Dictionary) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 12)
	var highlight := bool(block.get("highlight", false))

	var rank := _typed(_text(block, "rank", ctx), 18,
			UiKit.CARIMBO if highlight else UiKit.TINTA_SUAVE)
	rank.custom_minimum_size.x = 28.0
	row.add_child(rank)

	var name := _body_label(_text(block, "name", ctx), 18, UiKit.TINTA)
	name.custom_minimum_size.x = 170.0
	if highlight:
		name.add_theme_font_override("font", UiKit.CAPTION_FONT)
		name.text = "★ " + name.text
	row.add_child(name)

	var ratio := float(block.get("ratio", 0.0))
	if ratio > 1.0:
		ratio /= 100.0
	row.add_child(MeterBar.new(ratio,
			UiKit.CARIMBO if highlight else UiKit.TINTA_SUAVE, 22.0))

	var value := _body_label(_text(block, "value", ctx), 18, UiKit.TINTA)
	value.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	value.custom_minimum_size.x = 70.0
	row.add_child(value)
	return row


## Bandeira da nação: usa a arte de 640 px (`assets/flags_hi/`) e encaixa a
## proporção nativa dentro do envelope `width`×`height` — o Reino Unido 2:1 fica
## 160×80 e a Suíça 1:1 fica 96×96, sem esticar e sem cortar. `width`/`height`
## do bloco valem como envelope máximo, não como caixa fixa.
static func _flag(block: Dictionary, ctx: Dictionary) -> Control:
	var code := _text(block, "code", ctx)
	var box := Vector2(float(block.get("width", 160.0)),
			float(block.get("height", 96.0)))
	var holder := UiKit.flag_view(UiKit.flag_texture(code), box)
	holder.set_meta("tpl_flag_envelope", true)
	return holder


static func _text_label(block: Dictionary, ctx: Dictionary) -> Control:
	var text := _text(block, "text", ctx)
	if bool(block.get("uppercase", false)):
		text = text.to_upper()
	var size := float(block.get("size", 16.0))
	var color := _tone(String(block.get("tone", "tinta")))
	var font := String(block.get("font", "body"))
	var node: Label
	if font == "display":
		node = _display(text, size, color)
	elif font == "typed":
		node = _typed(text, size, color)
	else:
		node = _body_label(text, size, color)
	if bool(block.get("wrap", false)):
		node.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		if block.has("wrap_width"):
			node.custom_minimum_size.x = float(block["wrap_width"])
	var align := String(block.get("align", "left"))
	node.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER if align == "center" \
			else HORIZONTAL_ALIGNMENT_RIGHT if align == "right" \
			else HORIZONTAL_ALIGNMENT_LEFT
	return node


# --- Tipografia e utilidades -------------------------------------------------

static func _display(text: String, size: float, color: Color) -> Label:
	return _font_label(text, size, color, UiKit.PAPER_DISPLAY_FONT)


static func _typed(text: String, size: float, color: Color) -> Label:
	return _font_label(text, size, color, UiKit.PAPER_BODY_FONT)


static func _body_label(text: String, size: float, color: Color) -> Label:
	return _font_label(text, size, color, UiKit.BODY_FONT)


static func _font_label(text: String, size: float, color: Color,
		font: Font) -> Label:
	var node := Label.new()
	node.text = text
	node.add_theme_font_override("font", font)
	node.add_theme_font_size_override("font_size", int(size))
	node.add_theme_color_override("font_color", color)
	node.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	return node


static func _line(color: Color, thickness: float,
		dashed: bool = false) -> Control:
	return Hairline.new(color, thickness, dashed)


static func _double_rule(color: Color) -> Control:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 2)
	box.add_child(_line(color, 1.0))
	box.add_child(_line(color, 2.0))
	box.add_child(_line(color, 1.0))
	return box


static func _seal(text: String) -> Control:
	var panel := PanelContainer.new()
	var style := _paper_style(Color(0, 0, 0, 0), UiKit.BARRA_FILETE, 0.0, 1, ["all"])
	style.content_margin_left = 6.0
	style.content_margin_right = 6.0
	style.content_margin_top = 2.0
	style.content_margin_bottom = 2.0
	panel.add_theme_stylebox_override("panel", style)
	panel.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	var label := _typed(text, 12, UiKit.BARRA_ROTULO)
	panel.add_child(label)
	return panel


static func _mini_paper_button(glyph: String) -> Button:
	var button := Button.new()
	button.text = glyph
	button.custom_minimum_size = Vector2(28.0, 26.0)
	button.focus_mode = Control.FOCUS_ALL
	button.add_theme_font_override("font", UiKit.PAPER_BODY_FONT)
	button.add_theme_font_size_override("font_size", 15)
	button.add_theme_stylebox_override("normal",
			_paper_style(UiKit.PAPEL_CLARO, UiKit.TINTA, 0.0, 1, ["all"]))
	button.add_theme_stylebox_override("hover",
			_paper_style(UiKit.ABA_INATIVA, UiKit.TINTA, 0.0, 1, ["all"]))
	button.add_theme_stylebox_override("disabled",
			_paper_style(UiKit.PAPEL, Color(UiKit.TINTA, 0.35), 0.0, 1, ["all"]))
	button.add_theme_stylebox_override("focus", UiKit.outline(UiKit.CARIMBO))
	button.add_theme_color_override("font_color", UiKit.TINTA)
	button.add_theme_color_override("font_disabled_color", UiKit.TINTA_SUAVE)
	UiKit._bind_sfx(button, UiSfx.Sound.TOGGLE)
	return button


static func _spacer_height(height: float) -> Control:
	var node := Control.new()
	node.custom_minimum_size.y = height
	node.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return node


static func _apply_flags(node: Control, block: Dictionary) -> void:
	if block.has("slot"):
		node.set_meta("tpl_slot", String(block["slot"]))
	if bool(block.get("hidden", false)):
		node.visible = false
	if block.has("width") and not node.has_meta("tpl_flag_envelope"):
		node.custom_minimum_size.x = float(block["width"])
	if block.has("height") and not node.has_meta("tpl_flag_envelope"):
		node.custom_minimum_size.y = float(block["height"])
	if block.has("w"):
		node.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		node.size_flags_stretch_ratio = float(block["w"])
	if bool(block.get("grow", false)):
		node.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		node.size_flags_vertical = Control.SIZE_EXPAND_FILL
	if block.has("mh"):
		node.custom_minimum_size.y = maxf(node.custom_minimum_size.y,
				float(block["mh"]))


static func _tag_slot(node: Control, block: Dictionary, key: String) -> void:
	if block.has(key):
		node.set_meta("tpl_slot", String(block[key]))


static func _first_block(spec: Dictionary, type: String) -> Dictionary:
	for block in spec.get("blocks", []):
		if String(block.get("type", "")) == type:
			return block
	return {}


static func _text(block: Dictionary, key: String, ctx: Dictionary,
		fallback: String = "") -> String:
	if not block.has(key):
		return fallback
	return _resolve(block[key], ctx, _params(block, ctx))


static func _params(block: Dictionary, ctx: Dictionary) -> Dictionary:
	var out := {}
	var source: Dictionary = block.get("params", {})
	for key in source:
		out[key] = _resolve(source[key], ctx)
	return out


static func _resolve(value: Variant, ctx: Dictionary,
		params: Dictionary = {}) -> String:
	if value is String:
		var text: String = value
		var data: Dictionary = ctx.get("data", {})
		if _is_token(text, "@"):
			text = I18n.t(text.substr(1), params)
			if _is_token(text, "$"):
				text = _lookup(data, text.substr(1))
			return text
		if _is_token(text, "$"):
			var resolved := _lookup(data, text.substr(1))
			# Um valor de dado pode ser outra chave de i18n (nome de cidade).
			return _resolve(resolved, ctx, params) if _is_token(resolved, "@") \
					else resolved
		# Tokens embutidos ("★ @city.name.GBR-LDN"); valores monetários
		# começam com "$ " e não são tokens (o caminho precisa ser um id).
		if text.contains(" @") or text.contains(" $"):
			var parts := text.split(" ")
			for index in parts.size():
				var part: String = parts[index]
				if _is_token(part, "@") or _is_token(part, "$"):
					parts[index] = _resolve(part, ctx, params)
			return " ".join(parts)
		return text
	if value is float:
		return String.num(value, 0) if is_equal_approx(value, roundf(value)) \
				else str(value)
	if value is int or value is bool:
		return str(value)
	return ""


## Um token é `@chave`/`$caminho` com identificador simples; "$ 4,1 tri"
## (dinheiro já formatado) não é token.
static func _is_token(text: String, prefix: String) -> bool:
	if not text.begins_with(prefix) or text.length() < 2:
		return false
	var body := text.substr(1)
	if body.is_empty():
		return false
	for index in body.length():
		var char := body[index]
		var valid := (char >= "a" and char <= "z") \
				or (char >= "A" and char <= "Z") \
				or (char >= "0" and char <= "9") \
				or char == "_" or char == "." or char == "-"
		if not valid:
			return false
	return true


static func _lookup(data: Dictionary, path: String) -> String:
	var node: Variant = data
	for part in path.split("."):
		if node is Dictionary and (node as Dictionary).has(part):
			node = (node as Dictionary)[part]
		else:
			return ""
	if node is String:
		return node
	if node is float:
		return String.num(node, 0) if is_equal_approx(node, roundf(node)) \
				else str(node)
	return str(node)


static func _alignment(column: Dictionary) -> HorizontalAlignment:
	return HORIZONTAL_ALIGNMENT_RIGHT if String(column.get("align", "left")) \
			== "right" else HORIZONTAL_ALIGNMENT_LEFT


static func _cell_label(cell: Dictionary, column: Dictionary) -> Control:
	var color := _tone(String(cell.get("tone", "tinta")))
	var label := _body_label(String(cell.get("text", "")),
			float(cell.get("size", 17)), color)
	if bool(cell.get("strong", false)):
		label.add_theme_font_override("font", UiKit.CAPTION_FONT)
	label.horizontal_alignment = _alignment(column)
	label.size_flags_vertical = Control.SIZE_SHRINK_CENTER

	# Célula com ícone(s) de produto e/ou chip de cor (culturas): os selos do
	# bem entram antes do texto, na mesma linha. Sem `icon`/`icons`/`chip`, o
	# rótulo continua sendo a própria célula (caminho barato das tabelas grandes).
	var chip := String(cell.get("chip", "")).strip_edges()
	var icon_specs := _cell_icons(cell)
	if icon_specs.is_empty() and chip.is_empty():
		label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		label.size_flags_stretch_ratio = float(column.get("weight", 1.0))
		return label

	var box := HBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.size_flags_stretch_ratio = float(column.get("weight", 1.0))
	box.mouse_filter = Control.MOUSE_FILTER_IGNORE
	box.alignment = BoxContainer.ALIGNMENT_END \
			if _alignment(column) == HORIZONTAL_ALIGNMENT_RIGHT \
			else BoxContainer.ALIGNMENT_BEGIN

	for spec in icon_specs:
		var icon_id := String((spec as Dictionary).get("id", ""))
		if icon_id.is_empty():
			continue
		# Um tratamento só para todo bem: selo de 20 px com a cor do SETOR e o
		# glifo de tinta do Icon Forge (nada de tingir o ícone com a cor do bem).
		var sector := String((spec as Dictionary).get("sector",
				cell.get("sector", "")))
		box.add_child(ProductIcons.seal(icon_id, sector))

	if not chip.is_empty():
		var swatch := PanelContainer.new()
		swatch.custom_minimum_size = Vector2(9.0, 9.0)
		swatch.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		swatch.size_flags_horizontal = Control.SIZE_SHRINK_CENTER
		swatch.mouse_filter = Control.MOUSE_FILTER_IGNORE
		swatch.add_theme_stylebox_override("panel",
				_flat(Color.from_string(chip, UiKit.TINTA_SUAVE), UiKit.TINTA_SUAVE))
		box.add_child(swatch)
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(label)
	return box


## Lista normalizada de ícones de uma célula: `icon` (um) ou `icons` (vários).
static func _cell_icons(cell: Dictionary) -> Array:
	var out: Array = []
	if cell.has("icons"):
		for item in cell.get("icons", []):
			if item is Dictionary:
				out.append(item)
			elif item is String and not String(item).is_empty():
				out.append({"id": String(item)})
	var single := String(cell.get("icon", "")).strip_edges()
	if not single.is_empty():
		out.append({"id": single})
	return out


static func _compare_cells(left: String, right: String) -> int:
	var a: Variant = _numeric(left)
	var b: Variant = _numeric(right)
	if a != null and b != null:
		if is_equal_approx(a, b):
			return 0
		return 1 if a > b else -1
	return left.naturalnocasecmp_to(right)


## Extrai um número para ordenar sem tocar em lógica de jogo.
static func _numeric(text: String) -> Variant:
	var digits := ""
	var has_digit := false
	for index in text.length():
		var char := text[index]
		if char >= "0" and char <= "9":
			digits += char
			has_digit = true
		elif char == "-" and digits.is_empty():
			digits += char
	if not has_digit:
		return null
	return float(digits)


static func _fill(name: String) -> Color:
	match name:
		"papel":
			return UiKit.PAPEL
		"claro":
			return UiKit.PAPEL_CLARO
		"inativa":
			return UiKit.ABA_INATIVA
		"barra":
			return UiKit.BARRA_FUNDO
		"none", "transparent":
			return Color(0, 0, 0, 0)
	return UiKit.PAPEL_CLARO


static func _tone(name: String) -> Color:
	match name:
		"positive", "positivo":
			return UiKit.POSITIVO
		"negative", "carimbo":
			return UiKit.CARIMBO
		"warning", "alerta":
			return UiKit.ALERTA
		"suave", "tinta_suave":
			return UiKit.TINTA_SUAVE
		"filete":
			return UiKit.BARRA_FILETE
		"barra_texto":
			return UiKit.BARRA_TEXTO
		"barra_rotulo":
			return UiKit.BARRA_ROTULO
		"carimbo_barra":
			return UiKit.CARIMBO_BARRA
		"none":
			return Color(0, 0, 0, 0)
	return UiKit.TINTA


static func _paper_style(fill: Color, border: Color, pad: float,
		width: int = 1, edges: Array = ["all"], radius: int = 0,
		inset: float = 0.0) -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = fill
	style.border_color = border
	var top := width if edges.has("all") or edges.has("top") else 0
	var bottom := width if edges.has("all") or edges.has("bottom") else 0
	var left := width if edges.has("all") or edges.has("left") else 0
	var right := width if edges.has("all") or edges.has("right") else 0
	style.border_width_top = top
	style.border_width_bottom = bottom
	style.border_width_left = left
	style.border_width_right = right
	style.corner_radius_top_left = radius
	style.corner_radius_top_right = radius
	style.corner_radius_bottom_left = radius
	style.corner_radius_bottom_right = radius
	style.content_margin_left = maxf(pad + inset, 0.0)
	style.content_margin_right = maxf(pad + inset, 0.0)
	style.content_margin_top = maxf(pad, 0.0)
	style.content_margin_bottom = maxf(pad, 0.0)
	return style


static func _flat(fill: Color, border: Color) -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = fill
	style.border_color = border
	style.set_border_width_all(1)
	return style


## Seleção da linha em papel: fundo de aba inativa + tarja CARIMBO no `_draw`.
static func _apply_row_style(row: PaperRow, selected_index: int) -> void:
	row.selected = row.index == selected_index
	row.add_theme_stylebox_override("panel", _flat(
			UiKit.ABA_INATIVA if row.selected else Color(0, 0, 0, 0),
			Color(0, 0, 0, 0)))
	row.queue_redraw()


static func _emit_close(source: Node) -> void:
	var root := _find_root(source)
	if root != null:
		root.close_requested.emit()


static func _emit_choice(source: Node, id: String) -> void:
	var root := _find_root(source)
	if root != null:
		root.choice_selected.emit(id)


static func _find_root(source: Node) -> TemplateRoot:
	var node := source
	while node != null:
		if node is TemplateRoot:
			return node as TemplateRoot
		node = node.get_parent()
	return null
