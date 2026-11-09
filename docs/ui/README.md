# Interface — capturas e validação

Evidência objetiva da reformulação de interface (Design System "Industrial
Strategy — Premium Dark"). As capturas são para revisão humana ou de um modelo
multimodal. **Nenhuma imagem foi inspecionada visualmente por este agente**:
`tools/vision_review.py` reporta `visual_inspection: false` e
`vision_status: "desativado"` (sem credenciais `VISION_*`).

## Onde estão as imagens

| Conjunto | Caminho | Resoluções | Estados |
|---|---|---|---|
| Antes | `captures/antes/head/` | 1920×1080, 1366×768 | 7 (sem busca e sem menu) |
| Depois | `captures/f4/final/` | 1920×1080, 1600×900, 1366×768, 2560×1440 | 9 |

Estados: `00_hud`, `01_economy_nations`, `02_economy_cities`,
`03_economy_empty`, `04_trade`, `05_production`, `06_politics`, `07_city`,
`08_menu`. O "antes" não tem `03` (a tela não tinha busca) nem `08` (o HUD não
tinha menu de pausa).

Cada rodada grava também `layout_report.json` com os retângulos dos `Control`,
o foco atual, o transbordo fora da viewport, texto cortado/elidido e rolagem
necessária.

## Fases e evidência

| Fase | O que foi feito | Evidência |
|---|---|---|
| 1. Auditoria | inventário das telas e problemas (ver `DESIGN.md`) | relatório de layout do "antes": sem foco, 1 texto elidido no Comércio |
| 2. Design System | tokens, tema e ~20 componentes em `ui_kit.gd` | `01`–`06` usam botões, abas, cards, pills, tabelas e estados |
| 3. HUD | barra de 56 px, doca com atalhos, pausa e tooltips | `00_hud` |
| 4. Navegação | janela responsiva, menu Esc, foco por teclado | `08_menu` + campo `focus` do relatório |
| 5. Painéis | quatro telas + detalhe da cidade no mesmo sistema | `01`–`07` |
| 6. Validação | 36 capturas, 0 problemas de layout, foco em 32/32 estados | `layout_report.json` e `comparison.md` |


## Como reproduzir

```powershell
$godot = "C:\caminho\Godot_v4.7.2-stable_win64.exe"
& $godot --path . res://tools/ui_capture.tscn -- `
    --phase=f4 --run=final `
    --sizes=1920x1080,1600x900,1366x768,2560x1440
```

Métricas de pixels e comparação antes/depois:

```powershell
python tools\vision_review.py `
  --images "docs/ui/captures/f4/final/1920x1080/*.png" `
  --out docs/ui/comparison.json --markdown docs/ui/comparison.md `
  --compare "docs/ui/captures/antes/head/1920x1080/00_hud.png" "docs/ui/captures/f4/final/1920x1080/00_hud.png" `
  --compare "docs/ui/captures/antes/head/1920x1080/01_economy_nations.png" "docs/ui/captures/f4/final/1920x1080/01_economy_nations.png" `
  --compare "docs/ui/captures/antes/head/1920x1080/04_trade.png" "docs/ui/captures/f4/final/1920x1080/04_trade.png" `
  --compare "docs/ui/captures/antes/head/1920x1080/07_city.png" "docs/ui/captures/f4/final/1920x1080/07_city.png"
```

## Resultado verificável

- 36 capturas geradas sem erro em quatro resoluções.
- `layout_report.json`: **0** problemas (nenhum `offscreen`, `text_overflow`,
  `text_elided` ou `focus_hidden`).
- Viewport lógica **1920×1080** nas quatro resoluções, com retângulos
  idênticos; a janela mede 1651×907 em (134, 86) em todas.
- Foco de teclado presente em **todos** os 32 estados com janela/menu abertos;
  o "antes" não tinha foco em nenhum estado.
- Conteúdo confirmado no relatório, não por leitura visual: estado vazio da
  economia mostra “Nenhuma cidade corresponde ao filtro.” e “0 de 5”; o menu
  mostra “WarEconomy”; produção mostra o estado vazio honesto.

## Antes × depois (1920×1080, métricas de pixel)

| Par | MAE | RMSE | SSIM | pixels alterados |
|---|---|---|---|---|
| HUD | 0.0029 | 0.0305 | 0.9689 | 1.4% |
| Economia | 0.0194 | 0.0481 | 0.8220 | 6.0% |
| Comércio | 0.0216 | 0.0576 | 0.8049 | 6.3% |
| Cidade | 0.0194 | 0.0485 | 0.8384 | 6.1% |

Os números medem mudança de pixels, não qualidade estética. A identidade
anterior já usava latão sobre carvão; esta fase consolidou componentes, estados,
foco, busca/filtro e dimensionamento responsivo. As mudanças se concentram na
área da janela (moldura, cabeçalho, tabelas e estados), como os 6% de pixels
alterados indicam.

## Ferramentas usadas

- Skills lidas: `game-ui-ux` (layout por anchors/containers, stretch com
  referência, foco, pilha de telas), `godot-ui-control` (anchors vs offsets,
  Containers, Theme, foco) e `godot-ui` (hierarquia de `Control`, Theme,
  padrões de menu/HUD).
- MCP `mcp_godot-editor-mcp` respondendo: `godot://project/info` confirmou
  Godot 4.7.2, autoloads `Economy`/`I18n`/`Music`; a árvore da cena aberta
  confirmou `MainGame → WorldEnvironment, DirectionalLight3D,
  Player/CameraSocket/Camera3D`. Esse servidor não expõe captura de tela.
- O `mcp_godot-mcp-toolkit` (que teria `runtime_screenshot`) depende de addon
  no projeto e não está disponível; nenhuma ferramenta duplicada foi instalada.
  A captura foi feita pela cena `tools/ui_capture.tscn` em GPU real.
- Cena de captura: `tools/ui_capture.tscn`; métricas: `tools/vision_review.py`.

## Limites desta validação

- Alinhamento, hierarquia e legibilidade foram checados por geometria e
  conteúdo de texto, não por julgamento visual.
- Contraste global baixo nas capturas com janela aberta é esperado: o painel é
  escuro e ocupa a maior parte da imagem; o texto é claro e responde pela
  leitura. As métricas de contraste p95−p5 não avaliam contraste local de texto.
- Avaliação estética final exige abrir as capturas ou submetê-las a um modelo
  com visão.
