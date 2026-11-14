extends RefCounted
class_name ProductIcons

## Ícones de produto/recurso em SVG (silhueta branca), tingíveis via `modulate`.
## Matérias-primas (21) vêm do Icon Forge (arte própria): `assets/icons/resources/`.
## Os bens das cadeias de produção (F1) usam o mesmo diretório e são resolvidos
## pelo id — o `goods.json` é a lista de verdade, não há segunda lista manual.
## Manufaturados/herdados vêm do game-icons.net (CC BY 3.0): `assets/icons/products/`.

const DIR := "res://assets/icons/products/"
const RESOURCE_DIR := "res://assets/icons/resources/"
## As 21 matérias-primas geradas por tools/icon_forge (mesmo traço da casa).
## Mantida como contrato da legenda de recursos do HUD; qualquer outro id do
## Icon Forge é aceito pelo fallback de `texture()`.
const RESOURCES := [
	"coal", "copper", "gas", "iron", "oil",
	"bauxite", "lead", "gold", "phosphate", "tungsten",
	"rare_earths", "sulfur", "diamond", "uranium",
	"wheat", "rice", "maize", "potato", "soy", "cotton", "wood",
]
const FILES := {
	"factory": "factory.svg",
	"crate": "crate.svg",
	"coin": "coin.svg",
	"sword": "sword.svg",
	"rifle": "rifle.svg",
	"truck": "truck.svg",
	"ship": "ship.svg",
	"plane": "plane.svg",
}

static var _cache: Dictionary = {}


## Caminho do SVG do id (vazio se não existir).
static func icon_path(id: String) -> String:
	if RESOURCES.has(id):
		return RESOURCE_DIR + id + ".svg"
	var file: String = FILES.get(id, "")
	if not file.is_empty():
		return DIR + file
	# Bens das cadeias de produção: o Icon Forge gera um SVG por id de goods.json.
	var candidate := RESOURCE_DIR + id + ".svg"
	return candidate if ResourceLoader.exists(candidate) else ""


## Textura do ícone (ou null se o id não existir). Em cache.
static func texture(id: String) -> Texture2D:
	if _cache.has(id):
		return _cache[id]
	var path := icon_path(id)
	if path.is_empty():
		return null
	var tex: Texture2D = load(path)
	if tex != null:
		_cache[id] = tex
	return tex


## Cria um TextureRect pronto para a UI (32×32, tingível).
## `background`: quando informado, o tint é corrigido para manter contraste
## sobre aquele fundo (os SVGs são silhuetas brancas; um bem claro como o
## algodão sumiria no papel claro). Fundo transparente mantém a cor original.
static func make(id: String, color: Color, size: float = 32.0,
		background: Color = Color(0, 0, 0, 0)) -> TextureRect:
	var icon := TextureRect.new()
	icon.texture = texture(id)
	icon.modulate = readable(color, background) \
			if background.a > 0.0 else color
	icon.custom_minimum_size = Vector2(size, size)
	icon.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	icon.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	icon.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return icon


## Luminância relativa WCAG de uma cor (0..1), ignorando alfa.
static func luminance(color: Color) -> float:
	var channels: Array[float] = [color.r, color.g, color.b]
	var linear: Array[float] = []
	for channel in channels:
		linear.append(channel / 12.92 if channel <= 0.04045 \
				else pow((channel + 0.055) / 1.055, 2.4))
	return 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2]


## Razão de contraste WCAG entre duas cores (1..21).
static func contrast(left: Color, right: Color) -> float:
	var a := luminance(left)
	var b := luminance(right)
	return (maxf(a, b) + 0.05) / (minf(a, b) + 0.05)


## Cor do ícone com contraste mínimo sobre `background`. Sobre papel claro, o
## tint claro é puxado para a tinta escura (sem trocar a matiz do setor); sobre
## fundo escuro, a cor clara já funciona e é devolvida como está.
static func readable(tint: Color, background: Color,
		minimum: float = 3.0) -> Color:
	if tint.a <= 0.0:
		tint = Color("#2b2418")
	if contrast(tint, background) >= minimum:
		return tint
	var ink := Color("#2b2418") if luminance(background) > 0.5 \
			else Color("#f2ead6")
	var result := tint
	for _step in 16:
		if contrast(result, background) >= minimum:
			break
		result = result.lerp(ink, 0.12)
	return result
