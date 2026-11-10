extends SceneTree

## Gera assets/ui/paper.png (512×512): papel em tom #e6dbbd com grão de
## FastNoiseLite, manchas sutis e vinheta escura nas bordas, para o fundo de
## ficha da variante C "Papelaria de guerra".
##   godot --headless --path . --script res://tools/generate_paper_texture.gd

const SIZE := 512
const BASE := Color("#e6dbbd")


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var image := Image.create(SIZE, SIZE, false, Image.FORMAT_RGBA8)

	# Grão fino do papel: ruído fractal de baixa frequência.
	var grain := FastNoiseLite.new()
	grain.seed = 20261110
	grain.frequency = 0.05
	grain.fractal_octaves = 4
	grain.fractal_type = FastNoiseLite.FRACTAL_FBM

	# Manchas maiores e mais raras (umidade/envelhecimento).
	var stains := FastNoiseLite.new()
	stains.seed = 771
	stains.frequency = 0.018
	stains.fractal_octaves = 3
	stains.fractal_type = FastNoiseLite.FRACTAL_FBM

	for y in SIZE:
		for x in SIZE:
			var n := grain.get_noise_2d(x, y)          # -1..1
			var s := stains.get_noise_2d(x, y)        # -1..1
			var v := 1.0 + n * 0.05                    # variação fina de brilho
			v -= maxf(s, 0.0) * 0.10                   # manchas só escurecem
			# Vinheta escura nas bordas (quadrática, sutil no centro).
			var dx := (float(x) / SIZE - 0.5) * 2.0
			var dy := (float(y) / SIZE - 0.5) * 2.0
			var dist := sqrt(dx * dx + dy * dy)
			var vig := clampf((dist - 0.62) / 0.55, 0.0, 1.0)
			v -= vig * vig * 0.18
			image.set_pixel(x, y, Color(BASE.r * v, BASE.g * v, BASE.b * v, 1.0))

	var err := image.save_png("res://assets/ui/paper.png")
	if err != OK:
		push_error("generate_paper_texture: falhou ao salvar (erro %d)" % err)
	else:
		print("assets/ui/paper.png OK")
	quit(err)
