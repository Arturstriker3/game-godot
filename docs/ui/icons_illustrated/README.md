# Ícones ilustrados integrados (v3) — evidência

Integração dos **173 ícones** aprovados pela pessoa dona do projeto (arte gerada
para o OpenGS com o backend OpenAI em `tools/icon_gen`, estilo v3
semi-realista). Branch `feat/icons-illustrated`, a partir de
`balance/global-resources`.

## O que entrou

| Conjunto | Caminho | Qtde | Ids |
|---|---|---|---|
| Bens | `assets/icons/illustrated/<id>.png` + `illustrated/64/<id>.png` | 113 | todos os ids de `data/definitions/economy/goods.json` |
| Construções, infraestrutura e finanças | `illustrated/places/<id>.png` + `places/64/` | 58 | ex.: `farm`, `mine`, `factory_*`, `warehouse`, `city`, `bank_*`, `power_*`, `railway`, `port`, `currency`, `credit` |
| Bens futuros | `illustrated/microchips.png`, `illustrated/old_rifles.png` (+ `64/`) | 2 | ainda sem id em `goods.json` |

Total: **173 ícones × 2 variantes = 346 PNGs**. Os brutos de geração
(`assets/icons/illustrated_source/gen_api/`) continuam fora do git.

Nomenclatura decidida:

- a arte de `advanced_chips` virou **`microchips`** (bem futuro da G1);
- `chips` (bem existente) é o ícone de **semicondutores**;
- **`old_rifles`** fica salvo para um bem futuro, sem nenhum consumidor hoje.

Créditos e licença em `assets/icons/CREDITS.md`.

## Antes → depois

- **Antes:** todo bem usava o SVG branco do Icon Forge tingido de `TINTA`
  dentro de um selo de 20 px com a cor do setor a 35%.
- **Agora:** `ProductIcons` carrega primeiro o PNG ilustrado e o desenha com
  `modulate` **branco** (o PNG já é colorido). Abaixo de 32 px usa a variante de
  64 px; acima, a de 128 px. O SVG do Icon Forge continua como **fallback**
  (mesmo tratamento antigo) e como conjunto dos 20 setores de serviço.
- Construções/finanças ganharam API própria: `ProductIcons.place_path`,
  `place_texture` e `place` (também com a regra 64/128).

## Decisão sobre o selo

**Selo discreto de papel, sem o tint do setor.** O PNG ilustrado já traz
contorno escuro, sombra de contato e margem própria; tingi-lo de `TINTA` (como
as silhuetas) destrói a arte, e o selo colorido do setor briga com as cores do
desenho. A escolha foi feita comparando três tratamentos em 16, 20, 24 e 32 px,
sobre papel e sobre carvão:

| Tratamento | Resultado |
|---|---|
| glifo de tinta no selo do setor (antigo) | perde toda a cor; vira mancha escura |
| sem selo | ótimo sobre papel; sobre carvão o contorno escuro some |
| **placa de papel discreta (`COLORED_PLATE` = 0,55)** | **escolhido**: quase invisível sobre papel e legível sobre carvão |

Implementação: `ProductIcons.Seal` desenha a placa de `PAPEL_CLARO` a 55%, borda
`TINTA` a 25% e raio 3, e a arte ocupa o quadro inteiro. A legenda do mapa
(`LegendResourceIcon`) mantém o quadrado na cor do **recurso** porque ali ele é o
swatch da cor no mapa, não o selo do setor.

Evidência: `icon_scale_16_24_32.png` (os 113 bens em 16/24/32 px com o
componente real, as 58 construções em 32 px e a tira sobre carvão),
reproduzível com `tools/icon_scale_preview.tscn`.

## Capturas de UI (ui_capture, janela — não headless)

`godot --path . res://tools/ui_capture.tscn -- --set=icons --phase=icons
--run=v3 --screens=on --sizes=1920x1080,1600x900,1366x768,2560x1440`

16 PNGs + `layout_report.json` em `docs/ui/captures/icons/v3/` (fora do git).
Anexados aqui:

| Arquivo | Tela |
|---|---|
| `captures/1920x1080/01_market_all.png` | Mercado, aba Bens (113 bens) |
| `captures/1920x1080/03_chain.png` | Produção, aba Cadeia (receitas e quem consome) |
| `captures/1920x1080/04_resource_legend.png` | Legenda de recursos do mapa |
| `captures/1600x900/01_market_all.png` | Mercado em 1600×900 |
| `captures/1366x768/01_market_all.png` | Mercado em 1366×768 |
| `captures/2560x1440/01_market_all.png` | Mercado em 2560×1440 |
| `icon_scale_16_24_32.png` | nitidez 16/24/32 px + construções |
| `layout_report.json` | relatório da rodada |

`layout_report.json`: **16 capturas, 16 auditorias, 0 `offscreen`, 0
`text_overflow`, 0 `text_elided`, 0 `focus_hidden`, 0 chave de i18n vazada**.
Nenhum estado de janela ficou sem foco (a legenda do mapa é estado de mapa, sem
janela).

Inspeção visual feita nesta entrega (modelo com visão): Mercado 1920, Cadeia
1920, Legenda 1920 e a tira 16/24/32. Os ícones ficam reconhecíveis em 16 px;
a 24 e 32 px os detalhes aparecem; não há corte, tarja ou sobreposição.

## Como reproduzir

```powershell
$godot = "C:\caminho\Godot_v4.7.2-stable_win64.exe"

# 1) nitidez 16/24/32 (janela; grava docs/ui/icons_illustrated/)
& $godot --path . res://tools/icon_scale_preview.tscn -- `
    --out=res://docs/ui/icons_illustrated/icon_scale_16_24_32.png

# 2) telas com produto (janela; grava docs/ui/captures/icons/v3/)
& $godot --path . res://tools/ui_capture.tscn -- --set=icons --phase=icons `
    --run=v3 --screens=on --sizes=1920x1080,1600x900,1366x768,2560x1440
```

Um Godot por vez: o lock é `%TEMP%\opengs_godot_test.lock` (o mesmo dos testes).

## Testes

```
tools/run_tests.ps1 -List market_template_test,production_template_test,services_template_test,goods_data_test -MinFreeDiskGB 6
```

| Teste | Resultado |
|---|---|
| `market_template_test` | PASS — 113 bens no conjunto ilustrado, nenhum no fallback SVG |
| `production_template_test` | PASS |
| `services_template_test` | PASS (serviços continuam no Icon Forge) |
| `goods_data_test` | PASS (manifesto/cores/SVG do forge seguem válidos) |

## Limites conhecidos

- As capturas usam o dia 1 com estoque/preço reais do núcleo; não há número
  inventado. Onde o dado é zero, a tabela mostra `0 t`.
- `icons_illustrated` é arte final, mas o mapa 3D ainda usa os marcadores
  antigos de `assets/icons/places/` para cidade/porto; a API `place_*` está
  pronta para as telas que quiserem migrar.
- As variantes de 64 px dependem do import do Godot; os `.import` gerados não
  foram versionados por decisão da tarefa (o projeto os regenera na abertura).
