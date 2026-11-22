extends RefCounted
class_name ProductIcons

## Ícones de bem do jogo. Há dois conjuntos, nesta ordem de precedência:
##
## 1. `assets/icons/illustrated/` — a arte ilustrada aprovada (PNG colorido,
##    128 px, com a variante 64 px em `64/`). É o conjunto ativo. O PNG já tem
##    cor, contorno e sombra próprios, então NÃO se tinge com `INK`: desenha-se
##    com `modulate` branco. A variante de 64 px é a escolhida quando o ícone
##    aparece com até 32 px na tela (nitidez no downscale e menos memória).
##
## 2. `assets/icons/resources/` — os SVGs brancos do Icon Forge (24×24,
##    silhueta), fallback enquanto um id não tiver PNG ilustrado. Como são
##    silhuetas, continuam sendo tingidos com `INK`.
##
## Construções, infraestrutura e finanças ficam em `illustrated/places/`
## (mesmas variantes): use `place_path`, `place_texture` e `place` nas telas.
##
## Sobre o selo: o selo quadrado com a cor do SETOR foi desenhado para as
## silhuetas brancas (16 px de glifo de tinta sobre fundo tintado). O PNG
## ilustrado já traz o próprio contorno e sombra e ocupa o quadro inteiro, então
## o tratamento colorido é **sem selo tintado** — ver `COLORED_PLATE`.
##
## `assets/icons/products/` guarda só glifos de NAVEGAÇÃO que não são bens
## (caixa, moeda, fábrica, espada, navio, avião). Um id de bem nunca resolve
## para lá, mesmo quando o nome coincide (ex.: `truck`).
##
## `assets/icons/services/` é o terceiro conjunto: os 20 glifos dos setores de
## serviço (F8), gerados pelo MESMO Icon Forge a partir de
## `tools/icon_forge/service_icons.json`. Ids de serviço não colidem com ids de
## bem nem com os glifos de navegação, e continuam entrando no mesmo selo.

const GOODS_DIR := "res://assets/icons/resources/"
const UI_DIR := "res://assets/icons/products/"
const SERVICES_DIR := "res://assets/icons/services/"
const ILLUSTRATED_DIR := "res://assets/icons/illustrated/"
const ILLUSTRATED_SMALL_DIR := "res://assets/icons/illustrated/64/"
const PLACES_DIR := "res://assets/icons/illustrated/places/"
const PLACES_SMALL_DIR := "res://assets/icons/illustrated/places/64/"

## Até este tamanho na tela, o ícone ilustrado usa a variante de 64 px.
const SMALL_PX := 32.0

## Fundo do selo nos ícones ilustrados: placa de papel discreta (sem o tint do
## setor), só o suficiente para o PNG colorido continuar legível se aparecer
## sobre fundo escuro. Sobre papel ela é quase invisível. 0 = sem placa.
const COLORED_PLATE := 0.55

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
## Papel usado na placa opcional dos ícones ilustrados (mesmo de `UiKit.PAPEL_CLARO`).
const PLATE_PAPER := Color("#efe6cc")

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
	# F8: os 20 setores de serviço. Mesma paleta de papelaria; `construction`
	# já existia como setor de bem e vale para os dois (mesma família).
	"retail": Color("#a8792f"),
	"wholesale": Color("#8a6a3a"),
	"food_service": Color("#b5622e"),
	"hospitality_tourism": Color("#47707a"),
	"freight_logistics": Color("#5b6b73"),
	"passenger_transport": Color("#3f6b7d"),
	"finance": Color("#6b6257"),
	"insurance_pension": Color("#5f6b52"),
	"health": Color("#8a4a52"),
	"education": Color("#5a5f7a"),
	"telecom": Color("#4f6b6b"),
	"it": Color("#4a5a86"),
	"media_advertising": Color("#8a6a4f"),
	"professional_services": Color("#4f5a52"),
	"real_estate": Color("#6f5a3f"),
	"maintenance_repair": Color("#5d5a4a"),
	"personal_services": Color("#7a5470"),
	"leisure_culture": Color("#7d5a6a"),
	"cleaning_security_waste": Color("#5a6b5f"),
}
## Setor desconhecido/ausente: tinta suave, sem inventar cor.
const SECTOR_FALLBACK := Color("#5a4a2c")

## Cache por caminho (a mesma textura serve a qualquer id, e a variante 128/64
## tem caminhos distintos).
static var _cache: Dictionary = {}


## Caminho do PNG ilustrado do id para um tamanho na tela. Com `size` até
## `SMALL_PX` prefere a variante de 64 px; sem `size` (ou acima do limite) usa a
## de 128 px. Cai para a outra variante se só uma existir. Vazio se não houver.
static func illustrated_path(id: String, size: float = 0.0) -> String:
	var small := ILLUSTRATED_SMALL_DIR + id + ".png"
	var large := ILLUSTRATED_DIR + id + ".png"
	var order: Array[String] = []
	if size > 0.0 and size <= SMALL_PX:
		order = [small, large]
	else:
		order = [large, small]
	for path in order:
		if ResourceLoader.exists(path):
			return path
	return ""


## Caminho do PNG ilustrado de uma construção/finança (mesma regra 64/128).
static func place_path(id: String, size: float = 0.0) -> String:
	var small := PLACES_SMALL_DIR + id + ".png"
	var large := PLACES_DIR + id + ".png"
	var order: Array[String] = []
	if size > 0.0 and size <= SMALL_PX:
		order = [small, large]
	else:
		order = [large, small]
	for path in order:
		if ResourceLoader.exists(path):
			return path
	return ""


## Caminho da imagem do id (vazio se não existir). Bens primeiro: o PNG
## ilustrado é o conjunto ativo e o SVG do Icon Forge é o fallback; `products/`
## só responde a glifos de interface; `services/` só aos 20 setores de serviço.
static func icon_path(id: String, size: float = 0.0) -> String:
	var illustrated := illustrated_path(id, size)
	if not illustrated.is_empty():
		return illustrated
	var good := GOODS_DIR + id + ".svg"
	if ResourceLoader.exists(good):
		return good
	var file: String = FILES.get(id, "")
	if not file.is_empty():
		return UI_DIR + file
	var service := SERVICES_DIR + id + ".svg"
	if ResourceLoader.exists(service):
		return service
	return ""


## Textura do ícone (ou null se o id não existir). Em cache por caminho.
static func texture(id: String, size: float = 0.0) -> Texture2D:
	var path := icon_path(id, size)
	if path.is_empty():
		return null
	if _cache.has(path):
		return _cache[path]
	var tex: Texture2D = load(path)
	if tex != null:
		_cache[path] = tex
	return tex


## Textura ilustrada de uma construção/finança (null se não houver PNG).
static func place_texture(id: String, size: float = 0.0) -> Texture2D:
	var path := place_path(id, size)
	if path.is_empty():
		return null
	if _cache.has(path):
		return _cache[path]
	var tex: Texture2D = load(path)
	if tex != null:
		_cache[path] = tex
	return tex


## O id tem PNG ilustrado (de bem)? Útil para as telas decidirem entre o
## tratamento colorido e o selo de tinta do SVG.
static func is_illustrated(id: String, size: float = 0.0) -> bool:
	return not illustrated_path(id, size).is_empty()


## A textura veio do conjunto ilustrado (bem ou construção)? Fonte única para o
## `modulate`: ilustrado = branco, silhueta = tinta.
static func is_colored_texture(tex: Texture2D) -> bool:
	return tex != null and String(tex.resource_path).begins_with(ILLUSTRATED_DIR)


## Cor de tinta do setor. Sem setor conhecido, devolve a tinta suave — nunca a
## cor do bem (é isso que criava o arco-íris).
static func sector_tint(sector: String) -> Color:
	return SECTOR_TINTS.get(sector, SECTOR_FALLBACK)


## Fundo do selo sobre `paper`: a tinta do setor a 35%. Serve ao teste de
## contraste (o glifo do fallback SVG é sempre TINTA sobre este fundo).
static func seal_background(sector: String, paper: Color) -> Color:
	return paper.lerp(sector_tint(sector), SEAL_FILL)


## Selo padrão de um bem: fundo do setor, borda de tinta e glifo de tinta.
## É o componente usado em toda célula de tabela/ficha com ícone de bem. Com o
## PNG ilustrado, desenha o colorido sem tingir (e sem selo tintado, ver
## `COLORED_PLATE`).
static func seal(id: String, sector: String,
		size: float = SEAL_SIZE) -> Control:
	var tex := texture(id, size)
	var node := Seal.new(tex, sector_tint(sector), size)
	return node


## Glifo solto, sem selo: textura tingida de tinta para as silhuetas do Icon
## Forge e textura em cores naturais para o PNG ilustrado.
static func make(id: String, size: float = 15.0) -> TextureRect:
	var icon := TextureRect.new()
	var tex := texture(id, size)
	icon.texture = tex
	icon.modulate = Color.WHITE if is_colored_texture(tex) else INK
	icon.custom_minimum_size = Vector2(size, size)
	icon.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	icon.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	icon.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return icon


## Ícone solto de construção/finança (PNG colorido, sem tingir), para telas e
## marcadores. Devolve um `TextureRect` já dimensionado.
static func place(id: String, size: float = 32.0) -> TextureRect:
	var icon := TextureRect.new()
	icon.texture = place_texture(id, size)
	icon.modulate = Color.WHITE
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


## Selo desenhado em `_draw`. No SVG de fallback: quadrado arredondado na cor
## do setor + glifo de tinta centralizado. No PNG ilustrado: a arte colorida
## ocupa o quadro (com a placa de papel opcional de `COLORED_PLATE`).
class Seal extends Control:
	var _tex: Texture2D
	var _glyph := ProductIcons.SEAL_GLYPH
	var _style: StyleBoxFlat
	var _colored := false

	func _init(tex: Texture2D, tint: Color, seal_size: float,
			glyph: float = -1.0) -> void:
		_tex = tex
		_colored = ProductIcons.is_colored_texture(tex)
		_glyph = glyph if glyph > 0.0 else seal_size * 0.8
		_style = StyleBoxFlat.new()
		if _colored:
			_style.bg_color = Color(ProductIcons.PLATE_PAPER,
					ProductIcons.COLORED_PLATE)
			_style.border_color = Color(ProductIcons.INK, 0.25)
			_style.set_border_width_all(1)
		else:
			_style.bg_color = Color(tint, ProductIcons.SEAL_FILL)
			_style.border_color = Color(ProductIcons.INK, 0.5)
			_style.set_border_width_all(1)
		_style.set_corner_radius_all(3)
		custom_minimum_size = Vector2(seal_size, seal_size)
		size_flags_vertical = Control.SIZE_SHRINK_CENTER
		mouse_filter = Control.MOUSE_FILTER_IGNORE

	func _draw() -> void:
		if _colored:
			if ProductIcons.COLORED_PLATE > 0.0:
				draw_style_box(_style, Rect2(Vector2.ZERO, size))
			if _tex != null:
				var px := minf(size.x, size.y)
				draw_texture_rect(_tex,
						Rect2((size - Vector2(px, px)) * 0.5, Vector2(px, px)),
						false, Color.WHITE)
			return
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
