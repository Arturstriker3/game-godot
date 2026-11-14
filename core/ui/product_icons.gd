extends RefCounted
class_name ProductIcons

## Ícones de bem do jogo. Existe UM conjunto: os SVGs do Icon Forge
## (`assets/icons/resources/`, arte própria, 24×24, silhueta branca), um por id
## de `goods.json`, gerados por `tools/icon_forge` a partir de
## `resource_icons.json`.
##
## Tratamento único em todas as telas: glifo de TINTA dentro de um selo
## quadrado de 20 px preenchido com a cor do SETOR a 35% e borda de tinta de
## 1 px — a mesma geometria da legenda de recursos do mapa. A cor não é mais a
## do bem (o arco-íris de uma matiz por bem); o setor agrupa o bem no filtro do
## Mercado e no rótulo da tabela.
##
## `assets/icons/products/` guarda só glifos de NAVEGAÇÃO que não são bens
## (caixa, moeda, fábrica, espada, navio, avião). Um id de bem nunca resolve
## para lá, mesmo quando o nome coincide (ex.: `truck`).

const GOODS_DIR := "res://assets/icons/resources/"
const UI_DIR := "res://assets/icons/products/"

## As 21 matérias-primas do Icon Forge. Contrato da legenda de recursos do HUD;
## os demais bens também vêm do Icon Forge pelo id de `goods.json`.
const RESOURCES := [
	"coal", "copper", "gas", "iron", "oil",
	"bauxite", "lead", "gold", "phosphate", "tungsten",
	"rare_earths", "sulfur", "diamond", "uranium",
	"wheat", "rice", "maize", "potato", "soy", "cotton", "wood",
]
## Glifos de interface (navegação), nunca de bens.
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

## Tinta do glifo (mesma de `UiKit.TINTA`) e do contorno do selo.
const INK := Color("#2b2418")
## Selo padrão: 20 px de lado com glifo de 16 px (o tamanho da legenda).
const SEAL_SIZE := 20.0
const SEAL_GLYPH := 16.0
const SEAL_FILL := 0.35

## Tinta de cada setor de `goods.json`. Matizes dessaturados da papelaria: o
## selo sobre papel fica em ~#d5ccb4.., não vira etiqueta colorida.
const SECTOR_TINTS := {
	"food": Color("#a8792f"),
	"agro": Color("#6d7a3a"),
	"mining": Color("#6b6257"),
	"energy": Color("#a34f2a"),
	"metal": Color("#55616b"),
	"chemical": Color("#6b5478"),
	"machinery": Color("#7a5638"),
	"construction": Color("#8a7a58"),
	"wood": Color("#6f5230"),
	"packaging": Color("#a08a58"),
	"textile": Color("#8a5757"),
	"transport": Color("#47646a"),
	"electronics": Color("#45618a"),
	"military": Color("#5d5a34"),
	"luxury": Color("#7d5a6a"),
}
## Setor desconhecido/ausente: tinta suave, sem inventar cor.
const SECTOR_FALLBACK := Color("#5a4a2c")

static var _cache: Dictionary = {}


## Caminho do SVG do id (vazio se não existir). Bens primeiro: o Icon Forge é o
## único conjunto de bens; `products/` só responde a glifos de interface.
static func icon_path(id: String) -> String:
	var good := GOODS_DIR + id + ".svg"
	if ResourceLoader.exists(good):
		return good
	var file: String = FILES.get(id, "")
	if not file.is_empty():
		return UI_DIR + file
	return ""


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


## Cor de tinta do setor. Sem setor conhecido, devolve a tinta suave — nunca a
## cor do bem (é isso que criava o arco-íris).
static func sector_tint(sector: String) -> Color:
	return SECTOR_TINTS.get(sector, SECTOR_FALLBACK)


## Fundo do selo sobre `paper`: a tinta do setor a 35%. Serve ao teste de
## contraste (o glifo é sempre TINTA sobre este fundo).
static func seal_background(sector: String, paper: Color) -> Color:
	return paper.lerp(sector_tint(sector), SEAL_FILL)


## Selo padrão de um bem: fundo do setor, borda de tinta e glifo de tinta.
## É o componente usado em toda célula de tabela/ficha com ícone de bem.
static func seal(id: String, sector: String,
		size: float = SEAL_SIZE) -> Control:
	var node := Seal.new(texture(id), sector_tint(sector), size)
	return node


## Glifo solto, sem selo: silhueta de tinta para linhas que já têm o swatch de
## cor ao lado (legenda de recursos do HUD).
static func make(id: String, size: float = 15.0) -> TextureRect:
	var icon := TextureRect.new()
	icon.texture = texture(id)
	icon.modulate = INK
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


## Selo desenhado em `_draw`: quadrado arredondado na cor do setor + glifo de
## tinta centralizado. Sem textura, desenha só o miolo de tinta (fallback).
class Seal extends Control:
	var _tex: Texture2D
	var _glyph := ProductIcons.SEAL_GLYPH
	var _style: StyleBoxFlat

	func _init(tex: Texture2D, tint: Color, seal_size: float,
			glyph: float = -1.0) -> void:
		_tex = tex
		_glyph = glyph if glyph > 0.0 else seal_size * 0.8
		_style = StyleBoxFlat.new()
		_style.bg_color = Color(tint, ProductIcons.SEAL_FILL)
		_style.border_color = Color(ProductIcons.INK, 0.5)
		_style.set_border_width_all(1)
		_style.set_corner_radius_all(3)
		custom_minimum_size = Vector2(seal_size, seal_size)
		size_flags_vertical = Control.SIZE_SHRINK_CENTER
		mouse_filter = Control.MOUSE_FILTER_IGNORE

	func _draw() -> void:
		draw_style_box(_style, Rect2(Vector2.ZERO, size))
		if _tex == null:
			var inset := size.x * 0.25
			draw_rect(Rect2(Vector2(inset, inset), size - Vector2(inset, inset) * 2.0),
					Color(ProductIcons.INK, 0.55), true)
			return
		var glyph := clampf(_glyph, 1.0, minf(size.x, size.y))
		var at := (size - Vector2(glyph, glyph)) * 0.5
		draw_texture_rect(_tex, Rect2(at, Vector2(glyph, glyph)), false,
				ProductIcons.INK)
