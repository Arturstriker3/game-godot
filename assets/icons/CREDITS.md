# Créditos dos ícones (assets/icons)

## Ícones novos (HUD "Papelaria de guerra")

- `population.svg` — silhuetas de pessoas. **Arte própria do projeto.**
- `deficit.svg` — triângulo de aviso. **Arte própria do projeto.**

Ambos desenhados em branco (`#FFFFFF`) para tingir via `modulate` no Godot.
Licença: domínio próprio do OpenGS Game, sem restrição de reuso.

## Ícones de matéria-prima (resources/)

- `resources/*.svg` — 21 ícones de matéria-prima (coal, copper, gas, iron, oil,
  bauxite, lead, gold, phosphate, tungsten, rare_earths, sulfur, diamond, uranium,
  wheat, rice, maize, potato, soy, cotton, wood). **Arte própria do projeto,**
  gerada deterministicamente por `tools/icon_forge` (descrita em
  `tools/icon_forge/resource_icons.json`). Sem licença de terceiros.

## Ícones herdados

- `products/` — ícones de produto (wheat, truck, etc.): game-icons.net, CC BY 3.0.
  Ver `products/CREDITS.txt` para os autores individuais. Recoloridos em branco.
- `places/` — ícones de lugar (city, port, badge): Material Design Icons
  (Apache 2.0) + arte própria. Ver `places/CREDITS.txt`.

## Ícones ilustrados (illustrated/) — arte gerada para o projeto

Arte **gerada para o OpenGS Game** com o backend OpenAI em `tools/icon_gen`
(folha 4×4 por chamada, estilo v3 semi-realista). Não usa arte de terceiros e
não reproduz logotipos, cédulas ou marcas reais. Licença: domínio próprio do
OpenGS Game, sem restrição de reuso.

| Conjunto | Caminho | Qtde | Uso |
|---|---|---|---|
| Bens | `illustrated/<id>.png` + `illustrated/64/<id>.png` | 113 | os 113 ids de `data/definitions/economy/goods.json` |
| Bens futuros | `illustrated/microchips.png`, `illustrated/old_rifles.png` (+ `64/`) | 2 | ainda sem id em `goods.json`; ver notas abaixo |
| Construções, infraestrutura e finanças | `illustrated/places/<id>.png` + `places/64/` | 58 | telas/mapa via `ProductIcons.place_*` |

Notas de nomenclatura:

- `chips` (bem existente) é o ícone de **semicondutores**; a arte antiga de
  `advanced_chips` foi renomeada para `microchips` (bem futuro da G1, ainda sem
  id em `goods.json`).
- `old_rifles` (bem futuro, sem uso hoje) fica salvo em `illustrated/` para não
  se perder; nenhum screen o resolve enquanto não houver id.

Cada PNG existe em 128 px (exibição acima de 32 px) e 64 px (exibição até
32 px). Os arquivos foram aprovados pela pessoa dona do projeto em 2026-11 e
integrados em `feat/icons-illustrated`. Os brutos de geração
(`illustrated_source/gen_api/`) não são versionados.
