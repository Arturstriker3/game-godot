# Design system — Open Grand Strategy (HUD)

Interface de comando sobre o mapa, na direção **Industrial Strategy — Premium
Dark**: grafite, carvão e verde-escuro com acentos discretos de latão e cobre.
Implementada em nós `Control` do Godot; tokens, tema e componentes em
`core/ui/ui_kit.gd`.

## Modo

Operate: o jogador lê o estado da economia e abre a janela certa. Densidade
alta, hierarquia tipográfica clara, nenhum número inventado. A interface é
estado puro — os componentes recebem texto e valores já formatados e nunca
executam lógica econômica.

## Tokens

Cores (`UiKit`):

| Token | Valor | Uso |
|---|---|---|
| `BG_DEEP` | `#0A1112` | fundo de controles ativos, poços |
| `PANEL` | `#101A18` | painéis e janelas |
| `PANEL_RAISED` | `#16221F` | botões e linhas em repouso |
| `PANEL_HOVER` | `#1E2E29` | hover de botões e linhas |
| `EDGE` | `#2C3B35` | borda de 1 px de painéis |
| `EDGE_SOFT` | `#22302B` | réguas e trilhos |
| `OVERLAY` | `rgba(5,8,10,0.62)` | véu atrás de janelas e menu |
| `TEXT` | `#E2DFD2` | texto principal |
| `TEXT_DIM` | `#9AA69B` | texto secundário |
| `TEXT_MUTED` | `#7F8C82` | legendas e notas |
| `TEXT_INVERSE` | `#0E1715` | texto sobre latão |
| `BRASS` | `#C9A24A` | estado ativo, valor-chave, frete |
| `BRASS_DEEP` | `#8E7134` | pressionado |
| `COPPER` | `#B0764A` | acento secundário |
| `STEEL` | `#6F8F8C` | mercado, trânsito, modo de transporte |
| `POSITIVE` | `#7FA86B` | sem déficit |
| `NEGATIVE` | `#D26A5F` | déficit |
| `WARNING` | `#D08A3E` | cobertura entre 2 e 7 dias, pausa |

Tipografia:

- Títulos e nome do jogo: **Cinzel** (peso 600 via `FontVariation`), 19–26 px.
- Corpo, dados e botões: **Barlow Condensed**, 11–15 px.
- Cabeçalho de coluna e legenda: Barlow Condensed SemiBold, 11 px, caixa alta.
- Escala fixa: 26 / 19 / 15 / 15 / 13 / 11 px (`FS_DISPLAY`…`FS_CAPTION`).
- Números com separador de milhar do idioma ativo (`UiKit.number`).
- Fontes self-hosted em `assets/fonts/` (licença OFL ao lado de cada arquivo).

Forma e ritmo:

- Borda sempre 1 px; raio 2 px em painéis e 6–12 px em botões.
- Sem sombras projetadas, sem gradientes, sem vidro, sem glow.
- Réguas de 1 px (`UiKit.rule`) separam cabeçalho, colunas e tabela.
- Espaçamento em escala fixa: 4 / 8 / 12 / 16 / 24 px (`SPACE_XS`…`SPACE_XL`).
- Controle padrão de 30 px de altura; doca de 108 px de largura por 52 px de
  altura de botão.
- Movimento só em hover/foco: 0,12 s (`DURATION_FAST`) e 0,20 s (`DURATION`).


## Mapa de satélite

O fundo é um mosaico de satélite real (`map/map_data/satellite.jpg`, 8192×4096,
equirretangular global; gerado por `tools/fetch_satellite_background.ps1` a
partir do Esri World Imagery) passado pelo shader
`map/map_shaders/satellite_map.gdshader`, que mantém as cores naturais:

- contraste e saturação levemente realçados para leitura em tela;
- nenhum recolorimento por paleta: mar, vegetação, deserto e gelo aparecem como
  na imagem original;
- plataforma costeira sutil (água rasa um pouco mais clara) e mar aberto escuro,
  para a água não competir com as camadas estratégicas;
- crédito obrigatório da imagem no rodapé do HUD.

## Camada geopolítica

As fronteiras vêm das fronteiras reais do Natural Earth 10m, rasterizadas na
mesma projeção do terreno por `tools/build_political_layer.py`:

- `map/map_data/country_borders.png` (L8, 5000×2500): linha de 1 px onde o país
  do vizinho muda, com mipmaps para não tremer no zoom distante;
- `map/map_data/nation_fill.png` (RGBA, 2500×1250): preenchimento apenas das
  cinco nações jogáveis, com as cores do HUD;
- `map/map_shaders/political_overlay.gdshader` compõe fronteira (tinta escura a
  45%) e preenchimento (11%) em um único quad transparente — nenhuma geometria
  é reconstruída para trocar de camada.

Sobre o mapa ficam o marcador de cidade em três discos (halo escuro, disco na
cor da nação, ponto central claro) e o nome em Barlow Condensed caixa alta. O
modelo 3D da cidade mede 2,8 unidades e os veículos de comércio 1,8 unidades no
maior eixo; cada trecho de comércio mostra um único veículo (caminhão, navio ou
avião) seguindo do ponto de origem ao destino.

## Crescimento urbano modular

A cidade cresce em lotes de 0,5 unidade (~25 m) a partir de uma biblioteca de
12 módulos GLB semi-realistas gerada no Blender (`tools/blender/city_kit.py` →
`build_city_library_v2.py` → `assets/models/city_modules/`, com `library.json`).

- **Direção artística:** semi-realista e sóbria. Pedra, concreto, tijolo,
  reboco, ardósia, telha cerâmica e vidro escuro; nenhum branco puro, azul-claro
  ou cor vibrante. Cada módulo tem plinto, bandas de pavimento, cornija, janelas
  rebaixadas, telhado inclinado ou platibanda e equipamento de cobertura.
- **Biblioteca:** 3 residenciais baixos (sobrado geminado, casa isolada, quadra
  de 4 pavimentos), 3 médios (7, 10 e 11 pavimentos), 2 comerciais (misto com
  loja, escritório de 9 pavimentos), 2 industriais (galpão com docas, fábrica
  com chaminés), 1 torre de escritórios de 22 pavimentos e 1 marco cívico com
  cúpula.
- **Camadas:** `core/urban/` separa indicadores (`UrbanGrowthManager`), plano de
  bairros (`DistrictPlanner`), estado persistente (`CityLayout`), renderização
  (`UrbanRenderer`) e save/load (`UrbanPersistence`).
- **Tecido urbano:** estágios de raio 2, 4 e 6 lotes. Núcleo comercial e denso,
  anéis residenciais que perdem densidade para fora e indústria afastada no setor
  leste. Torres só no núcleo — nada de torre isolada no subúrbio.
- **Quarteirões:** ruas a cada quatro células (quarteirões de ~75 m com vias de
  ~25 m); lotes preenchem quase toda a frente da quadra, com jitter de 2 cm,
  rotação em quartos de volta e inclinação de ±2° para evitar alinhamento rígido.
- **Crescimento:** horizontal (novos lotes), adensamento e verticalização
  (casa → prédio → torre). Lotes existentes nunca são removidos, preservando a
  identidade dos bairros.
- **Determinismo:** seed por cidade + `RandomNumberGenerator` por célula e tint
  por instância entre 0,94 e 1,04.
- **Dados:** população e industrialização decidem o estágio; renda puxa torres,
  empregos sustentam escritórios e a industrialização libera a zona industrial
  (campos `jobs`, `income` e `industrialization` vêm do núcleo C++).
- **Espaço construível:** `buildable_filter` recebe a posição do lote e devolve
  se pode construir — no mapa principal consultará a máscara de água.
- **Render:** um `MultiMeshInstance3D` por arquétipo (+1 para ruas processuais)
  e um LOD de caixas por categoria entre 140 e 900 unidades.
- **Persistência:** `user://` JSON com os lotes e um hash do layout.
- **Protótipo isolado:** `game/city_prototype.tscn` (não altera o mapa
  principal), com a mesma inclinação de câmera do mapa (−56°) e a iluminação de
  `main_game.tscn`, sliders de população/industrialização, atalhos de estágio,
  salvar/carregar e leitura de FPS/draw calls.
- **Referência visual:** `tools/blender/render_reference_city.py` monta uma
  cidade média europeia com os 12 módulos e renderiza
  `docs/urban_art/reference_city.png`.

## Cidades industriais dinâmicas

O crescimento industrial é independente do tecido urbano: a cidade pode ser uma
metrópole comercial com pouco parque produtivo ou uma cidade pequena dominada
por siderurgia e manufatura.

- **Leitura da economia:** `core/urban/industry_profile.gd` deriva cinco setores
  (aço, manufatura, refino, energia e armazenagem) dos indicadores que já vêm do
  `EconomySim` — produção, industrialização, renda e estoque. Não há economia
  paralela: o viés por cidade só redistribui o peso relativo entre setores.
- **Módulos:** `tools/blender/city_kit.py` gera complexos de siderurgia (alto-forno
  e chaminés), manufatura (galpões serrilhados), refino (colunas e tanques),
  energia (torres de arrefecimento e turbinas), armazenagem (galpão e silos) e
  estruturas auxiliares (chaminés, tanques, galpões e pátios).
- **Distrito:** `core/urban/industrial_planner.gd` reserva uma grade fixa a leste
  da cidade, com uma faixa por setor (3 complexos principais + 3 auxiliares). As
  posições dependem só do setor e do índice, então o crescimento nunca desloca o
  que já foi construído.
- **Expansão e modernização:** o número de complexos cresce com a intensidade
  industrial; estruturas auxiliares aparecem nos estágios seguintes e os módulos
  antigos dão lugar às variantes modernas (galpão → armazém, fábrica → parque de
  manufatura).
- **Persistência:** `CityLayout` guarda `industry_stage` e `industrial_lots` com
  chave própria, incluídos no hash de determinismo e no save/load.
- **Protótipo:** `game/city_prototype.tscn` mostra a mesma cidade em três
  composições econômicas — residencial, industrial e polo avançado — com o mesmo
  id, seed e tecido urbano. Capturas em `docs/industry_prototype/`.

## População e economia (cenário de validação)

O núcleo C++ ganhou um cenário mínimo de duas cidades para validar o ciclo
econômico antes de generalizar o modo europeu. Ele vive no mesmo `EconomySim`
(`initialize_demo`, `tick_demo`, `get_demo_status`, `demo_price_shock`), sem
substituir as cinco cidades atuais.

- **POPs agregados:** uma população por cidade, com força de trabalho, empregados,
  desempregados, dependentes, renda salarial, renda de capital, transferências,
  impostos, renda disponível, consumo, satisfação, poupança e patrimônio.
- **Setor de serviços agregado:** 30% da população empregada em serviços, com
  salário próprio; evita um desemprego artificial de 98% e mantém a renda
  circulando.
- **Cidades:** `AGRIA` (100 mil, cinco fazendas de trigo) e `PORTUS` (200 mil,
  moinho e quatro padarias), ligadas por uma rota de trigo com frete pago.
- **Cadeia:** trigo → farinha → pão, com coeficientes de 1,2 e 0,83.
- **Dinheiro:** contas de famílias, empresas, serviços, governo, transporte e
  exterior; toda transferência debita uma conta e credita outra. O total é
  conservado e verificado a cada dia.
- **Preços:** reagem a estoque, oferta, demanda e custo de produção (piso de
  custo), com suavização e limites para não explodir nem ficar negativo.
- **PIB:** valor adicionado das construções (receita − insumos) + serviços;
  PIB per capita é resultado, não entrada.
- **Investimento:** 60% do lucro positivo é distribuído às famílias como renda
  de capital; o restante fica no caixa da empresa. A decisão de investir ainda
  não existe neste cenário.
- **Painel:** `game/population_prototype.tscn` mostra população, força de
  trabalho, emprego, salário médio, renda, PIB, PIB per capita, consumo,
  satisfação, poupança e lucro privado, além dos preços e estoques.
- **Teste:** `tools/population_demo_test.gd` roda 30 dias, confere conservação de
  dinheiro/população/estoque, lucro privado positivo, satisfação e resposta a um
  choque de preço no pão (gasto alimentar sobe e poupança cai).

## Câmera e navegação

- Inclinação inicial de −56° e distância 52, enquadrando o centro das cinco
  cidades na origem.
- Rotação horizontal livre (360°), por Q/E ou arrastando com o botão do meio;
  a rotação vertical fica entre −90° e −15°.
- A posição da câmera dá a volta no eixo X a cada 1000 unidades, reaparecendo
  no lado oposto do mapa — o mundo continua porque há cópias laterais da carta.

## Composição

- Barra superior fixa de 56 px: nação (nome + “Sua nação”), data e progresso do
  dia, quatro indicadores (população, estoque, déficit, trânsito), velocidade,
  idioma e menu. Os indicadores têm tooltip explicando a origem do número;
  ao pausar, o rótulo do dia troca para “Pausado” em `WARNING`.
- Doca à direita, 108 px de largura, quatro botões de 52 px (Economia,
  Comércio, Produção, Política) com tooltip “1 · Economia” etc. O botão ativo
  usa fundo `BG_DEEP` e borda latão.
- Janela modal responsiva: 86% × 84% da tela, limitada a 960×560 e 1800×1000;
  em 1920×1080 resulta em 1651×907. Cabeçalho com voltar (só no detalhe da
  cidade), título, subtítulo, dica contextual e fechar; rodapé com atalhos e
  selo `ESC`.
- Detalhe da cidade é sub-tela dentro da mesma janela; o título do cabeçalho
  passa a ser o nome da cidade e o voltar reaparece.
- Mapa sempre visível atrás; véu `OVERLAY` apenas com janela ou menu aberto.
- Menu de pausa (Esc): cartão de 460 px com título Cinzel, retomar, créditos,
  sair e atalhos.

## Resolução e escala

- Projeto em 1920×1080 com `display/window/stretch/mode="canvas_items"` e
  `aspect="expand"`: o layout lógico é sempre 1920×1080 e escala para o
  tamanho da janela, sem cortes.
- Validado por captura automática em 1920×1080, 1600×900, 1366×768 e
  2560×1440 — os quatro resultam em viewport lógica 1920×1080 e retângulos
  idênticos.
- Layout feito só com `Control` + `Container` + anchors; nenhum elemento é
  posicionado por pixel fora de painéis ancorados.
- Tabelas longas e o detalhe da cidade têm rolagem vertical; o rodapé e o
  cabeçalho ficam fixos.

## Navegação

- `1`–`4` abrem os quatro painéis; `Esc` fecha o menu, depois a janela, depois
  abre o menu.
- Todo controle interativo tem `focus_mode = FOCUS_ALL`; ao abrir uma tela o
  HUD foca o primeiro controle útil (`UiKit.focus_first`); no detalhe da
  cidade o foco vai para “Voltar”.
- Abas, filtros e linhas de tabela também recebem foco; o anel de foco é latão
  de 1 px, sem glow.

## Componentes

Construtores em `core/ui/ui_kit.gd`, todos nativos de `Control`:

- Botões: `primary_button` (latão sólido, uma por tela), `secondary_button`,
  `ghost_button`, `icon_button` (glifo + tooltip obrigatório).
- Navegação: `tab_bar` / `tab_button` / `set_tab_active`, `filter_chip` /
  `set_chip_active`, `toolbar`, `search_field`.
- Conteúdo: `card`, `surface`, `status_pill` (neutral/accent/positive/
  negative/warning/steel), `metric`, `paragraph`, `label`, `caption`, `title`,
  `rule`, `spacer`.
- Dados: `table_header` / `table_row` (peso relativo por coluna, números à
  direita, reticências quando falta largura), `row_button` (hover + seleção +
  foco), `meter`.
- Estados: `empty_state`, `error_state`, `loading_state`, `toast`.
- Tema: `build_theme()` aplica a paleta a Button, LineEdit, OptionButton,
  PopupMenu, ProgressBar, HSlider, Label, CheckBox, Tooltip, separadores e
  barras de rolagem — herdado de um único nó raiz.
- Toda a API antiga (`flat`, `label`, `metric`, `table_*`) foi preservada para
  o painel econômico de validação (`game/economy_panel_demo.tscn`).

## Movimento

- Sem animação de entrada. O progresso do dia se move continuamente na barra
  superior e pausa junto com a simulação.
- Hover e foco trocam de `StyleBox` imediatamente na leitura; apenas `toast`
  usa fade (0,12 s de entrada, 0,20 s de saída).

## Validação

- `tools/ui_capture.tscn` instancia o HUD sobre o mapa, percorre HUD, os quatro
  painéis, busca sem resultado, detalhe de cidade e menu nas quatro
  resoluções, grava PNGs e `layout_report.json`.
- O relatório é objetivo: retângulos dos `Control`, foco atual, transbordo
  para fora da viewport, texto cortado/elidido e rolagem necessária. Não é
  inspeção visual e não substitui revisão humana.
- `tools/vision_review.py` mede brilho, contraste, saturação, nitidez e bordas
  das capturas, e compara antes/depois (MAE, RMSE, SSIM).
- Saídas em `docs/ui/` (`captures/antes`, `captures/f4/final`,
  `comparison.md`).


## Conteúdo e i18n

- Todo texto vem de `core/i18n/i18n.gd` (pt-BR, en, es), trocável em tempo de
  execução; as telas se reconstroem ao trocar o idioma.
- Pedidos de comércio são guardados em um histórico rolante de 240 entradas no
  HUD, para a janela de Comércio não ficar vazia nos dias sem pedido novo.
- Onde a simulação não tem dado (produção/construção, diplomacia), a tela
  declara a ausência em vez de inventar números.

## Auditoria (Fase 1) — o que foi corrigido

- **Barra superior**: 52 px sem tooltips, sem indicação de pausa e sem menu.
  Agora 56 px com quatro indicadores explicados por tooltip, selo de pausa e
  botão `≡`; a altura continua abaixo de 6% da tela.
- **Doca**: 96 px sem atalhos visíveis. Agora 108 px, botões de 52 px e tooltip
  com o número do atalho.
- **Janela**: tamanho fixo de 1240×700 (não acompanhava a resolução). Agora
  proporcional, com mínimo e máximo, e retângulos idênticos em 16:9.
- **Economia**: sem busca nem filtros; o resumo repetia o subtítulo da janela.
  Removido o resumo duplicado; adicionados busca, três filtros, contador de
  resultados e estado vazio com ação “Limpar filtros”.
- **Comércio**: na aba de rotas a coluna de detalhe ficava vazia, gastando 40%
  da largura. A coluna agora só aparece na aba de pedidos, onde tem conteúdo.
- **Produção**: o hint do rodapé repetia o texto do estado vazio e o do
  cabeçalho. O texto do cabeçalho foi reescrito e a repetição removida.
- **Política**: tabelas flutuavam sobre o painel; o dossiê usava fonte 12 e a
  matriz tamanhos fixos fora da escala. Agora tudo dentro de `card`, com
  escala tipográfica e `paragraph` para textos longos.
- **Detalhe da cidade**: título e botão “Voltar” duplicavam o cabeçalho da
  janela. O título passou a ser o nome da cidade no cabeçalho; a sub-tela mostra
  nação, população e pills de déficit/trânsito.
- **Global**: nenhum controle recebia foco de teclado, e os tamanhos de fonte
  e espaçamento eram ad hoc. Agora há foco inicial por tela, anel de foco e
  escala fixa de tipografia/espaçamento.

## Arquivos

- `core/ui/ui_kit.gd` — tokens, tema e biblioteca de componentes.
- `core/ui/hud.gd` — barra, doca, janela, menu e histórico de pedidos.
- `core/ui/city_detail.gd` — sub-tela de cidade.
- `core/ui/screens/*.gd` — Economia, Comércio, Produção, Política.
- `core/i18n/i18n.gd` — textos em pt-BR, en e es.
- `tools/ui_capture.gd` / `.tscn` — captura e auditoria de layout.
- `docs/ui/` — capturas antes/depois e métricas objetivas.
