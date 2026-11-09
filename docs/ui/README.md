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

## Frente 5 — UX econômica (ciclo de QA 1)

Roteiro ampliado para 21 estados: além dos nove anteriores, cobre as cinco abas
do painel de cidade, a lista de alertas, a aba de cidades da produção, o fluxo
cidade → cidade produtora → voltar, o mercado com e sem resultado e as duas
abas de População.

| Conjunto | Caminho | Resoluções | Estados |
|---|---|---|---|
| Frente 5 | `captures/frente5/flow3/` | 1920×1080, 1600×900, 1366×768, 2560×1440 | 21 |

Resultado do ciclo: **84 capturas**, **0** problemas de layout (`offscreen`,
`text_overflow`, `text_elided`, `focus_hidden`) e **0** estados com janela sem
foco. O relatório também registra a pilha de contexto de cada estado — por
exemplo, `screen:production > city:GBR-LDN > city:FRA-PAR` no clique numa cidade
produtora e `screen:production > city:GBR-LDN` depois de voltar.

Correções do ciclo verificadas pela captura e pela suíte headless:

- a aba de mercado passou a somar importações/exportações **apenas do dia
  atual** (antes usava o histórico rolante de 240 pedidos do HUD);
- cobertura de estoque sem demanda passou a exibir “—” em vez de `0,0 d`,
  alinhada ao painel de cidade e à tela de produção;
- os hints do painel de cidade eram renderizados como chave crua em 8 dos 84
  estados auditados (`city.income_index.note`, `city.history.note`); agora são
  traduzidos, e a auditoria ganhou a checagem `i18n_key`, que reprova qualquer
  rótulo começando por um prefixo de chave conhecido;
- cobertura, índice de renda e qualidade de depósito passaram a usar o
  separador decimal do idioma (`0,0 d` em vez de `0.0 d`).

