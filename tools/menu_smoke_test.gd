extends Node

func _ready() -> void:
	var spec: Dictionary = UiTemplateBuilder.load_spec("menu")
	print("spec blocks: ", spec.get("blocks", []).size())
	var root: Control = UiTemplateBuilder.build(spec, {
		"title": "Open Grand Strategy",
		"day": "2",
		"last_save": "—",
		"version": "pré-alpha",
	})
	add_child(root)
	await get_tree().process_frame
	print("root: ", root.name, " children: ", root.get_child_count())
	get_tree().quit(0)
