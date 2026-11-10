extends SceneTree

## Teste headless do retângulo CARIMBO do minimapa (core/ui/minimap.gd):
## garante que a área vista fica SEMPRE dentro da textura do mapa, inclusive no
## zoom máximo afastado (câmera cobre mais que o mundo), no wrap horizontal e no
## zoom próximo (selo mínimo de 6×4 px preso ao mapa).
##   godot --headless --path . --script res://tools/minimap_view_test.gd
##
## Observação: o minimapa é carregado em runtime (load) para não referenciar a
## classe na assinatura — autoloads (UiSfx) só ficam disponíveis após o boot.

var _failures: Array[String] = []


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var minimap_script: GDScript = load("res://core/ui/minimap.gd")
	var minimap = minimap_script.new()
	root.add_child(minimap)
	await process_frame

	var map_rect := Rect2(minimap._image.position, minimap._image.size)
	print("[minimap] mapa: pos=%s size=%s" % [map_rect.position, map_rect.size])

	# 1) Zoom máximo afastado: câmera cobre mais que o mundo inteiro.
	minimap.set_view(Vector2(0.5, 0.5), Vector2(4.0, 2.0))
	_check_case("zoom_afastado", minimap, map_rect)

	# 2) Zoom próximo: selo mínimo, centrado.
	minimap.set_view(Vector2(0.5, 0.5), Vector2(0.01, 0.01))
	_check_case("zoom_proximo", minimap, map_rect)

	# 3) Wrap horizontal: centro perto da borda esquerda cruza a borda.
	minimap.set_view(Vector2(0.01, 0.5), Vector2(0.2, 0.2))
	_check_case("wrap", minimap, map_rect)

	# 4) Zoom próximo no canto: o selo mínimo é preso dentro do mapa.
	minimap.set_view(Vector2(0.995, 0.995), Vector2(0.005, 0.005))
	_check_case("zoom_proximo_canto", minimap, map_rect)

	minimap.queue_free()
	await process_frame

	if _failures.is_empty():
		print("[minimap] OK: retângulo sempre contido no mapa.")
		quit(0)
	else:
		for failure in _failures:
			push_error("[minimap] " + failure)
		quit(1)


func _check_case(case_name: String, minimap, map_rect: Rect2) -> void:
	var parts: Array[Rect2] = []
	if minimap._view.visible:
		parts.append(Rect2(minimap._view.position, minimap._view.size))
	if minimap._view_wrap.visible:
		parts.append(Rect2(minimap._view_wrap.position, minimap._view_wrap.size))
	if parts.is_empty():
		_failures.append("%s: nenhum retângulo desenhado" % case_name)
		return
	var desc := PackedStringArray()
	for r in parts:
		desc.append("(%s,%s %sx%s)" % [r.position.x, r.position.y, r.size.x, r.size.y])
		if not _contained(r, map_rect):
			_failures.append("%s: parte %s fora do mapa" % [case_name, desc[-1]])
	print("[minimap] %s: %d parte(s) %s" % [case_name, parts.size(), " ".join(desc)])


func _contained(r: Rect2, map_rect: Rect2) -> bool:
	var eps := 0.001
	return r.position.x >= map_rect.position.x - eps \
		and r.position.y >= map_rect.position.y - eps \
		and r.end.x <= map_rect.end.x + eps \
		and r.end.y <= map_rect.end.y + eps
