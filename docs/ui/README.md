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

## Frente 6 — registro no template T2

A tela de Economia deixou o painel modal escuro e passou a desenhar a própria
janela de papel da spec `data/ui/templates/registro.json` (T2): busca, filtros,
contador, tabela ordenável/paginada nas abas Nações e Cidades, e a ficha
resumida à direita. Duas abas de pasta (`folder_tabs`) e uma única tabela por
aba; a linha selecionada ganha a tarja CARIMBO e alimenta a ficha lateral.

| Conjunto | Caminho | Resoluções | Estados |
|---|---|---|---|
| Frente 6 | `captures/f6/t2/` | 1920×1080, 1600×900, 1366×768, 2560×1440 | 24 |

Resultado: **96 capturas**, **0** problemas de `offscreen`, `text_overflow`,
`text_elided`, `focus_hidden` ou `i18n_key`. O único achado do relatório é
`scroll_needed` (rolagem real e esperada) em telas alheias a esta frente.

Estados do T2 verificados por conteúdo no relatório, não por leitura visual:

- `01_economy_nations`: “City register” / “Registro de nações”, contador
  “0 of 807” e a tabela de nações;
- `02_economy_cities`: aba Cidades com chips “All · n”, “Deficit · n”,
  “In transit · n” e a ficha lateral da cidade selecionada;
- `03_economy_empty`: busca sem resultado mostra “No city matches the filter.”
  e mantém o botão de limpar filtros.

A captura roda no idioma persistido em `user://settings.cfg` (esta rodada saiu
em inglês); a troca de idioma reconstrói a spec e o relatório não aponta chave
vazada em nenhum dos 96 estados.

## Frente 7 — alertas no T2 compacto

A lista de Alertas deixou o painel modal escuro e virou uma janela T2 compacta
(640–880 × 460–620 px, `_spec()` em `core/ui/screens/alerts_screen.gd`) com o
mesmo `window_frame`, `data_table` paginada e estado vazio do registro. As
linhas continuam vindo de `core/ui/alerts.gd` (no máximo 6, priorizadas por
gravidade); nada é inventado quando não há alerta.

Colunas: **Alerta** (tipo, em carimbo quando o tom é negativo), **Cidade**,
**Nação** e **Situação** (o texto do alerta com os mesmos campos do modelo).
Clicar ou dar Enter numa linha empilha a ficha T3 na aba que explica o dado:
**mercado** para déficit e estoque baixo, **produção** para cidade sem
produção.

| Conjunto | Caminho | Resoluções | Estados |
|---|---|---|---|
| Frente 7 | `captures/f6/t3/` | 1920×1080, 1600×900, 1366×768, 2560×1440 | 24 |

Resultado: **96 capturas**, **0** problemas de `offscreen`, `text_overflow`,
`text_elided`, `focus_hidden` ou `i18n_key`; `13_alerts` mostra “6 active”,
“Deficit / Karachi / Pakistan / Deficit of 3,429 t of wheat.” e foco de teclado
nas quatro resoluções.

Navegação verificada por teste headless novo (`tools/alerts_nav_test.gd`):

```powershell
& $godot --headless --path . --script res://tools/alerts_nav_test.gd
```

O teste monta o HUD real, abre os alertas e aciona as três primeiras linhas:
PASS com `PAK-KAR (deficit)` abrindo a ficha na aba `market`.

## Frente 8 — histórico da ficha de cidade no dia 1

O bloco **Histórico** da ficha T3 (abas Visão geral e Mercado) desenhava uma
única barra no dia 1, porque a série tem exatamente a amostra do dia corrente.
Agora o gráfico e a nota dão lugar a um cartão de papel tracejado com o estado
vazio honesto — “Histórico ainda vazio / A série começa a ser desenhada no
segundo dia simulado.” (`city.history.empty.*`, pt-BR/en/es) — e voltam a
aparecer do segundo dia em diante.

```powershell
& $godot --path . res://tools/ui_capture.tscn -- --phase=f6 --run=t4 `
    --sizes=1920x1080,1600x900,1366x768,2560x1440
```

Resultado (`captures/f6/t4/`, 24 estados × 4 resoluções): **96 capturas**,
**0** problemas de layout. `07_city` mostra “History / No history yet / The
series starts on the second simulated day.” e `11_city_market` repete o mesmo
estado antes do bloco de preços não modelados.

## Frente 9 — comércio no T2 com abas de pasta

Comércio e Logística passou a usar a spec `data/ui/templates/comercio.json`
(T2, `window` 1651×907) com três abas de pasta:

- **Pedidos** — tabela do dia (dia, origem, destino, mercado, carga, trânsito,
  frete) e, ao lado, o cartão **Trechos do pedido** do registro selecionado
  (modo, trecho, distância, carga, viagens, trânsito, frete);
- **Rotas** — soma por modo de transporte (caminhão, navio, avião);
- **Mercado** — busca, chips `Todas`/`Com déficit`/`Com escassez`, contador
  “n de total”, cartão nacional (oferta, demanda, estoque, déficit, cobertura)
  e a tabela por cidade; clicar numa linha abre a ficha T3.

A ordenação, a página e a seleção continuam sendo estado de exibição da
`data_table`. O texto de preços não modelados continua no cartão nacional.

| Conjunto | Caminho | Resoluções | Estados |
|---|---|---|---|
| Frente 9 | `captures/f6/t6/` | 1920×1080, 1600×900, 1366×768, 2560×1440 | 24 |

Resultado: **96 capturas**, **0** problemas de `offscreen`, `text_overflow`,
`text_elided`, `focus_hidden` ou `i18n_key`. A primeira rodada (`f6/t5`) acusou
`offscreen` na tabela de mercado com 14 linhas por página; a página caiu para
7 linhas (o que cabe no cartão sem rolagem) e a rodada seguinte ficou limpa.

Teste headless (`tools/trade_template_test.gd`):

```powershell
& $godot --headless --path . --script res://tools/trade_template_test.gd
```

PASS com `pedidos=107`, `trechos=1`, `rotas=3` (caminhão/navio/avião),
`mercado=807` e o clique na primeira cidade empilhando a ficha T3.


## Frente 10 - barra lateral esquerda, lista de cidades e modo rotas (t24)

Três entregas visuais, cada uma com evidência própria em
`docs/ui/captures/layout_lateral/` (a pasta `docs/ui/captures/` é ignorada
pelo git, como as demais capturas):

| Arquivo | O que mostra |
|---|---|
| `hud_barra_esquerda.png` | HUD limpo em 1920×1080 (pt_BR) com a barra lateral de painéis à esquerda e a lista de cidades à direita |
| `tela_aberta.png` | Tela de Economia aberta: a folha começa à direita da barra e o botão ativo fica preenchido de latão |
| `rotas_legenda.png` | Modo rotas com a etiqueta "Rotas comerciais" (filtro por modal, escala de espessura e estado vazio honesto, porque a captura roda sem o mapa) |
| `layout_report.json` | Relatório da rodada de 27 estados × 1920×1080/1366×768 |
| `layout_report_es.json` | Rodada extra em `es` (27 estados em 1366×768), 0 achados |
| `layout_report_rotas.json` | O mesmo roteiro com `--mapmode=routes` |

Reproduzir (a captura precisa de janela; `--headless` trava em
`RenderingServer.frame_post_draw`):

```powershell
# janela normal (não minimizada: janela minimizada para de renderizar)
& $godot --path . res://tools/ui_capture.tscn -- --phase=lateral --run=rail `
    --sizes=1920x1080,1366x768 --locale=pt_BR `
    --out=res://docs/ui/captures/layout_lateral
& $godot --path . res://tools/ui_capture.tscn -- --phase=lateral --run=routes `
    --sizes=1920x1080,1366x768 --locale=pt_BR --mapmode=routes `
    --out=res://docs/ui/captures/layout_lateral
```

Resultado: **54 auditorias por rodada, 0** `offscreen`, `text_overflow`,
`text_elided`, `focus_hidden` ou `i18n_key` nas duas; a rodada extra em `es`
(27 auditorias em 1366×768) também ficou limpa, então a barra lateral, a lista
de cidades e a legenda de rotas não quebram ao trocar de idioma. Na primeira
rodada a etiqueta do modo rotas herdava a altura da lista de recursos (625 px
de papel vazio): `HUD._fit_legend` passou a refazer o ajuste no frame seguinte,
porque o tamanho mínimo de um `Control` só existe depois do layout.

Testes headless:

```powershell
& $godot --headless --path . --script res://tools/nation_city_list_test.gd
& $godot --headless --path . --script res://tools/trade_flow_tooltip_test.gd
& $godot --headless --path . --script res://tools/map_pick_test.gd
& $godot --headless --path . --script res://tools/cursor_hover_test.gd
& $godot --headless --path . res://tools/menu_smoke_test.tscn
```

Todos PASS. `trade_flow_tooltip_test` passou a ignorar cidades sob um
`Control` do HUD (a barra lateral cobre a faixa esquerda do mapa, então o
hover ali é do painel).

Pendência registrada: para o modo rotas exibir as fronteiras discretas do
`PoliticalLayer` (como o mapa de recursos), falta uma linha em
`map/city_world.gd::set_mode` - `show_overlay := mode not in ["geographic",
"political"]` (hoje `routes` está na lista de exclusão). A base do modo já é
a carta branca.

## Bandeiras em alta resolução no Dossiê (2026-11)

**Antes:** o bloco `flag` do Dossiê (T1) pedia 160x96 e carregava o PNG w40 do
mapa (40 px de largura) num `TextureRect` com
`STRETCH_KEEP_ASPECT_COVERED`. Resultado: bandeira ampliada ~4x (pixelada) e
cortada quando a proporção do país não era 2:1; a barra superior e o tooltip
sofriam do mesmo upscale.

**Depois:** `assets/flags_hi/` traz as mesmas bandeiras em **640 px**
(flagcdn.com / Wikimedia Commons, domínio público, mipmaps no import) e a UI
usa `UiKit.flag_view`: proporção nativa do país, filtro linear + mipmaps e
moldura de tinta de 1 px. O mapa continua com o atlas w40 + MultiMesh.

| Nação | Proporção | Envelope exibido |
|---|---|---|
| Reino Unido (GBR) | 2:1 | 160x80 |
| Suíça (CHE) | 1:1 | 96x96 |
| Catar (QAT) | ~1:2,55 | 160x63 |

Capturas (1920x1080, janela real):

- `docs/ui/captures/fix_round/dossie_bandeira.png` (GBR)
- `docs/ui/captures/fix_round/dossie_bandeira_suica.png` (CHE)
- `docs/ui/captures/fix_round/dossie_bandeira_catar.png` (QAT)

Como reproduzir:

```powershell
& $godot --path . res://tools/flag_capture.tscn
& $godot --path . res://tools/ui_capture.tscn -- `
    --phase=f4 --run=bandeiras `
    --sizes=1920x1080,1600x900,1366x768,2560x1440
```

Resultado: `ui_capture` gerou 108 capturas (4 resoluções x 27 estados) com
**0** problemas no `layout_report.json` (nenhum `offscreen`,
`text_overflow`, `text_elided` ou `focus_hidden`); a mesma rodada em
espanhol (`--locale=es`) e com a legenda de cidade (`--legend=city`) também
fechou sem problemas. Testes: `politics_dossier_test` (agora verifica textura
≥ 500 px e envelope 160x80), `city_tooltip_test` e `menu_smoke_test.tscn`.

## Culturas nas telas — População, ficha e dossiê (2026-11)

**Antes:** nenhuma tela mostrava cultura. O dado existia em
`data/definitions/culture/` (208 culturas, 179 nações, 834 cidades) sem leitor.

**Depois:** `core/culture/culture_db.gd` lê os três arquivos por nome de coluna
(sem presumir ids) e a interface mostra a composição em três lugares:

| Tela (template) | O que passou a aparecer |
|---|---|
| População T2 — aba Nações | coluna **Cultura principal** (chip de cor + nome) e, na ficha lateral, o bloco **Culturas** com todas as culturas do país, % em barra fina na cor da cultura, `★` principal, `✓` aceitas, `outras` no fim, legenda e fonte |
| População T2 — aba Cidades | coluna **Cultura** (maior participação) e o bloco **Culturas** na ficha da cidade, com a fonte (`Censo …`) ou a nota **estimativa** quando a fonte é `national_fallback`/`estimate_post2022` |
| Ficha T3 — aba População | cartão **Culturas** com a mesma lista da cidade e a fonte/estimativa |
| Dossiê T1 | linha **Cultura principal · aceitas** (ex.: `Inglês · Escocês, Galês, Norte-irlandês, Irlandês`) |

Sem dado, a lista mostra `pop.culture.empty` (pt-BR/en/es); nenhum percentual é
inventado, somado ou reescalado. A UI não altera a economia.

Capturas (1920×1080; a pasta `docs/ui/captures/` é ignorada pelo git):

| Arquivo | O que mostra |
|---|---|
| `populacao_nacoes.png` | aba Nações com o Brasil selecionado e o cartão Culturas (★/✓, fonte IBGE) |
| `populacao_cidades.png` | aba Cidades com a coluna Cultura e a ficha de Mumbai |
| `ficha_cidade_culturas.png` | ficha T3 de Londres na aba População com o cartão Culturas |
| `ficha_cidade_culturas_estimativa.png` | mesma ficha numa cidade cuja fonte é `national_fallback`, com a nota `estimativa` |
| `populacao_*_1366x768.png`, `ficha_*_1366x768.png` | as mesmas telas na janela de 1366×768 |
| `layout_report.json` | auditoria das 8 capturas |

Reproduzir (janela real; `--headless` trava em `RenderingServer.frame_post_draw`):

```powershell
& $godot --path . res://tools/ui_capture.tscn -- `
    --set=culturas --out=res://docs/ui/captures/culturas `
    --sizes=1920x1080,1366x768 --locale=pt_BR
```

Resultado: **8 capturas, 0** `offscreen`, `text_overflow`, `text_elided` ou
`focus_hidden`; o roteiro completo (`--set` omitido) fecha 54 auditorias em
1920×1080 e 1366×768 com **0** problemas (`docs/ui/captures/culturas/regressao/`).

Testes headless:

```powershell
& $godot --headless --path . --script res://tools/culture_db_test.gd
& $godot --headless --path . --script res://tools/population_template_test.gd
& $godot --headless --path . --script res://tools/politics_dossier_test.gd
```

`culture_db_test` confere catálogo, composição (soma 100%, ordem, `outras` no
fim), papéis, fontes/estimativas e nomes nos três idiomas; os outros dois abrem
o HUD real e verificam a coluna, o cartão da nação (Brasil), a ficha da cidade e
a linha do dossiê.

## Frente F6 — Mercado, Logística e consumo (2026-11-14)

**Antes:** o jogo tinha seis telas de papel (Economia, Comércio, Produção,
População, Política, Estatísticas). Não havia tela de Mercado nem de Logística;
a aba Cadeia da Produção só listava receitas (insumos → produto → subprodutos →
“quem consome” em texto); População não tinha consumo; a ficha da cidade não
mostrava necessidades nem fluxo de comércio; o detalhe do pedido em Comércio não
dizia o bem, o preço entregue nem a moeda. Nas tabelas, o `page_size` fixo da
spec (6–8 linhas) deixava metade da área em branco, e vários ícones claros
(algodão, arroz ensacado) sumiam no papel: o SVG é silhueta branca e o
`icon_color` do bem chegava quase na cor do fundo.

**Depois:**

| Onde | O que passou a existir |
|---|---|
| **Mercado** (T2, `mercado.json` + `market_screen.gd`) | abas Bens/Ofertas; cidades à esquerda com busca; 114 bens com ícone, preço na moeda local, estoque, oferta, demanda, importação e exportação; filtro por setor; ficha do bem com preço local, preço de importação, estoque, demanda, moeda e âncora; ofertas do dia **por tonelada na moeda do comprador** — origem + tarifa = **CIF**, frete separado (serviço doméstico do comprador, fora do CIF) e **custo total/t** = CIF + frete, com coluna **Camada** (regional/nacional/internacional); estado vazio honesto quando o dia não publicou pedido |
| **Logística** (T2, `logistica.json` + `logistics_screen.gd`) | abas Frota, Câmbio, Tarifas e Portos; 179 nações com caminhões/navios/aviões, combustível e fila; moedas com taxa, regime, base e receita de tarifas; editor de tarifa com aplicação real (`demo_set_tariff`); portos com capacidade, uso, fila e expansão (`demo_expand_port`) |
| **Produção — aba Cadeia** | nova coluna **Origem** (`Jazida`/`Fazenda`/`Fábrica` + cidades produtoras) ao lado de **Quem consome**, que agora traz os **ícones dos bens consumidores**; a tabela de cidades produtoras continua abaixo |
| **População — aba Consumo** | tabela/cartão por cidade com satisfação básico/vida/luxo, duráveis e satisfação geral (`pop.*`), só leitura do status |
| **Ficha da cidade (T3)** | blocos novos na aba População (necessidades por nível + duráveis) e no Mercado (moeda, câmbio e receita de tarifas), além de “Trabalho e renda” e “Fluxo de comércio” na visão geral |
| **Comércio — detalhe do pedido** | linhas de **bem**, **preço entregue** e **moeda** antes da tabela de trechos |
| **Docas** | selos **MERCADO (2)** e **LOGÍSTICA (8)**, com preload das telas no HUD; clicar num porto do mapa abre a Logística na aba Portos |
| **Gerador** | `select_field` (PaperSelect) e célula de tabela com ícone/chip de cor reaproveitados pelas novas telas |

**Tabelas que preenchem a altura.** `DataTableView` ganhou `fill_height`: a
tabela mede a altura de uma linha e calcula `floor(altura disponível / altura da
linha)` a cada `NOTIFICATION_RESIZED`, então o número de linhas por página muda
com a janela (Mercado passou de 6 para 10 bens por página em 1920×1080;
Logística de 8 para 12 nações; Produção de 5 para 6 cidades). O `page_size` da
spec fica só como medida antes do primeiro layout. Numa página cheia as linhas
dividem a sobra menor que uma linha, para o corpo da tabela não terminar numa
faixa vazia; a última página parcial continua alinhada ao topo.

**Ícones legíveis.** O glifo de cada bem vai em `TINTA` sobre o fundo do selo
do setor (`ProductIcons.seal_background`), com contraste ≥ 3:1 sobre
`PAPEL_CLARO`; a cor do bem deixou de ser usada como tint. `market_template_test`
verifica todos os bens publicados: textura não nula, caminho dentro do Icon
Forge, setor com cor e contraste ≥ 3:1. `production_template_test` verifica a
origem e os ícones dos consumidores; `20` traduções de bens que estavam em
português no inglês (e as equivalentes em espanhol) foram corrigidas no
`i18n.csv`/`.json`.

**Capturas (janela real; `--headless` trava em `RenderingServer.frame_post_draw`):**

```powershell
& $godot --path . res://tools/ui_capture.tscn -- `
    --phase=f6f --run=final `
    --sizes=1920x1080,1600x900,1366x768,2560x1440
& $godot --path . res://tools/ui_capture.tscn -- `
    --phase=f6f --run=final_en --sizes=1366x768 --locale=en
& $godot --path . res://tools/ui_capture.tscn -- `
    --phase=f6f --run=final_es --sizes=1366x768 --locale=es
```

Resultado: **136** capturas (34 estados × 4 resoluções) em pt-BR + **34** em
inglês + **34** em espanhol, todas com **0** `offscreen`, `text_overflow`,
`text_elided`, `focus_hidden`, `i18n_key` ou `scroll_needed` no
`layout_report.json`. `25_market_offers` agora sai com oferta real: quando o dia
1 ainda não publicou pedidos, a captura avança até 3 dias de forma síncrona
(`_ensure_trade_orders`) e seleciona bem/cidade do primeiro pedido — nenhum
número é inventado. Evidência em `docs/ui/captures/f6f/final*/` (a pasta
`captures/` é ignorada pelo git).

Testes headless (todos PASS):

```powershell
& $godot --headless --path . --script res://tools/market_template_test.gd
& $godot --headless --path . --script res://tools/logistics_template_test.gd
& $godot --headless --path . --script res://tools/production_template_test.gd
& $godot --headless --path . --script res://tools/trade_template_test.gd
& $godot --headless --path . --script res://tools/population_template_test.gd
& $godot --headless --path . --script res://tools/city_tooltip_test.gd
& $godot --headless --path . --script res://tools/nation_city_list_test.gd
& $godot --headless --path . --script res://tools/politics_dossier_test.gd
& $godot --headless --path . --script res://tools/economy_integration_test.gd
```

## Ícones de bem unificados (2026-11-14)

**Antes:** dois conjuntos de arte conviviam — `assets/icons/products/` (SVGs do
game-icons herdado, 512 px) e `assets/icons/resources/` (SVGs 24×24 do Icon
Forge). `ProductIcons` resolvia `products/` primeiro para oito ids, e um deles
(`truck`) também é um bem: o Mercado mostrava o caminhão herdado no meio dos
glifos da casa. E `ProductIcons.readable` (removido) tingia cada glifo com a
cor do bem —
dezenas de matizes, com bens claros sumindo no papel.

**Depois:** um conjunto e um tratamento em todas as telas (Mercado, Cadeia,
ficha, Comércio, Produção, Consumo). Todo bem resolve para o Icon Forge; o
glifo vai em `TINTA` (16 px) dentro de um selo de 20 px com fundo na cor do
**setor** a 35% e borda de tinta de 1 px — a geometria do selo da legenda de
recursos. A legenda do mapa não mudou: ela é a referência. `products/` fica só
com os glifos de navegação (caixa, moeda, fábrica, espada, navio, avião).

**Pipeline:** `python tools/icon_forge/icon_forge.py check` → `0 problema(s)`;
`build --out <tmp>` reproduz os SVGs publicados byte a byte, então nenhum dos
ícones novos precisou ser regerado — eles já seguem o traço dos 21 primários.

**Capturas (janela real, sem `--headless`):**

```powershell
& $godot --path . res://tools/ui_capture.tscn -- `
    --phase=icons_unify --run=after --set=icons `
    --sizes=1920x1080,1600x900,1366x768,2560x1440
```

16 capturas por rodada (`01_market_all`, `02_market_chemical`, `03_chain`,
`04_resource_legend`) em `captures/icons_unify/{before,after}/`; a página
`captures/icons_unify/index.html` põe antes e depois lado a lado, com detalhe
1:1 do Mercado/Química e da coluna "Quem consome". Os dois
`layout_report.json` saem com **0** `offscreen`, `text_overflow`,
`text_elided` e `focus_hidden`.

**Teste:** `market_template_test` verifica, em cada linha, textura não nula,
caminho dentro de `assets/icons/resources/` (conjunto único), setor com cor e
contraste do glifo de tinta sobre o fundo do selo ≥ 3:1.

## Fechamento da UI de Serviços (F8, 2026-11)

**Antes:** a tabela dos 20 setores tinha a coluna "Emprego" lendo
`demo_get_services_detail(city_id).sectors[].jobs`. Medido em `IND-MUM` (Mumbai),
esse campo devolvia **237.230,6 em 15 dos 20 setores**, com 0 nos quatro setores
sem negócio. Pelo próprio núcleo (`economy_sim.cpp`, passo 7 do dia de serviços)
o valor é o emprego de serviços **da cidade** rateado pela capacidade
(`business.jobs`) das empresas abertas, não o emprego do setor: como os negócios
semeados têm a mesma capacidade, o mesmo número se repete. O binário em uso
devolveu soma de 3.807.550,8 nos 20 setores contra `service_jobs` = 3.944.032,3.
O emprego real por negócio existe no payload (`businesses[].workers`, 40 em cada
um dos 16 negócios da amostra), mas somá-lo por setor daria 640 empregados onde
o cabeçalho publica 1.384.784: são universos diferentes (empresas abertas vs.
emprego agregado de serviços). O núcleo não foi alterado — outro agente está no
`gdextension/`.

**Depois:**

- A coluna "Emprego" por setor saiu da tabela. Sobram ícone + setor, demanda,
  oferta, preço local, negócios e exportação, e a coluna do setor ficou mais
  larga (peso 3,6) para os nomes longos em pt-BR/es.
- O cabeçalho da cidade, acima da tabela, publica o dado que o núcleo realmente
  abre: `Mumbai · 1,384,784 service workers · 17 businesses` na captura (que
  roda em `en`) e `Mumbai · 1.384.784 empregados em serviços · 17 negócios` em
  pt-BR — `service_workers` + `service_businesses` do resumo/detalhe; "sem
  serviços publicados neste dia" quando a cidade não publica setor no dia.
- A nota do rodapé diz onde o emprego aparece; os empregados de cada negócio
  continuam na aba Negócios, sem duplicar um total rateado.
- i18n: `services.col.jobs` saiu e entraram `services.sectors.city.workers`,
  `.city.businesses` e `.city.none` em pt-BR, en e es; a nota
  `services.sectors.note` foi reescrita nos três idiomas.

**Contrato esperado do núcleo (para reabrir a coluna):** `sectors[].jobs` só
volta a virar coluna quando representar o emprego **do setor**:

1. `jobs >= 0` por setor e `sum(sectors[].jobs) == city.service_jobs` no mesmo
   dia (tolerância 1e-6).
2. Setores com emprego real distinto publicam valores distintos — nada de
   ratear o total da cidade pela capacidade das empresas.
3. O mesmo número para o mesmo dia em `get_demo_status().cities[]` e em
   `demo_get_services_detail(city_id)`.
4. Se a UI for mostrar vagas e empregados, `sectors[].workers` (preenchidos)
   vem separado de `sectors[].jobs` (vagas), ambos por setor.

Enquanto o contrato não existir, a UI não mostra emprego por setor — nenhum
número é inventado e o total da cidade fica no cabeçalho.

**Captura (janela real, sem `--headless`; `captures/` é ignorado pelo git):**

```powershell
& $godot --path . res://tools/ui_capture.tscn -- `
    --phase=f8 --run=final --sizes=1920x1080,1600x900,1366x768,2560x1440
```

**156 capturas** (39 estados × 4 resoluções) sem erro, em
`captures/f8/final/`; o `layout_report.json` sai com **0** `offscreen`,
`text_overflow`, `text_elided`, `focus_hidden`, `scroll_needed` e `i18n_key` —
o cabeçalho novo não empurra a tabela de 8 linhas para fora da janela em
nenhuma das quatro resoluções (a viewport lógica segue 1920×1080 nas quatro).
Uma rodada extra em pt-BR (`--run=lang_pt --locale=pt_BR --sizes=1366x768`,
39 estados em `captures/f8/lang_pt/`) também fechou com **0** achados: o texto
mais longo (`Bombaim · … empregados em serviços · … negócios`) não elide.

**Teste:**

```powershell
& $godot --headless --path . --script res://tools/services_template_test.gd
```

PASS com 20 setores, ícones do conjunto de serviços, nomes por cultura e o
cabeçalho da cidade (`Mumbai · 1,384,784 service workers · 17 businesses`, sem
chave de i18n crua).
