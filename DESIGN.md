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


## Mapa base branco

O fundo dos modos geográfico/densidade/recurso é uma **carta branca** desenhada
por `map/map_shaders/paper_map.gdshader` a partir dos mesmos campos de distância
do Natural Earth (`map/map_data/atlas/region_sdf.png` e `world_sdf.png`):

- terra em marfim (`#F2EDE0`) e mar branco;
- relevo suave (hillshade da normal); hoje o `normal.png` é plano, então o
  efeito é discreto;
- apenas o **contorno dos continentes** em tinta `#3C372F`, 1,25 px — sem
  fronteiras de país ou província (estas ficam no modo político e no overlay
  sutil dos modos de calor);
- grão de papel fino e vinheta polar fundindo com o fundo do ambiente;
- a base existe para receber os futuros **mapas de densidade populacional e de
  recursos naturais** por cima.

O satélite `satellite.jpg` e o `tools/fetch_satellite_background.ps1` foram
removidos.

## Mapa político (atlas)

O modo político é uma carta desenhada por `map/map_shaders/atlas_political.gdshader`
em uma passada opaca, a partir de campos de distância com sinal gerados dos
vetores do Natural Earth 10m por `tools/build_atlas_layers.py`
(`map/map_data/atlas/`):

- **Fronteiras de país:** tinta `#17191B`, mínimo 1,8 px e ~2 km de largura no
  mundo (engrossam ao aproximar); faixa 30% mais escura junto à fronteira, por
  dentro de cada país.
- **Províncias:** divisas admin-1 reais, 0,85 px a 40%; somem ao afastar.
- **Costa:** linha de 1,15 px; mar com faixa rasa clara e duas linhas de nível
  paralelas à costa (somem ao afastar).
- **Terra:** cor da nação × tom do papel (`paper_flat.png`) × grão de papel procedural. Nações jogáveis em tons sóbrios
  (GBR `#A04844`, FRA `#506C9C`, DEU `#70757C`, NED `#C07840`, BEL `#B69C4A`);
  as demais em pergaminho com leve variação por `MAPCOLOR9`.
- **Nomes de nação:** Cinzel 600 em caixa alta, tinta a 68%, gravados no chão,
  no máximo 85% da largura do país; aparecem a partir de 34 u (Benelux só a
  partir de 95 u, quando os nomes de cidade saem).
- **Borda do mundo:** a vinheta polar termina exatamente na cor de fundo do
  ambiente, então o horizonte não mostra a quina da prancha.
- **Território dinâmico:** no mundo inteiro (Europa em alta resolução, resto na
  camada mundial) a cor e a fronteira de país vêm do
  dono atual de cada província (nação da cidade que a controla, lida do status
  do Economy). Fronteira grossa onde os donos de províncias vizinhas diferem,
  divisa fina onde são iguais; a faixa escura junto à fronteira tem 0,3 u (cabe
  dentro de uma província). O nome da nação se reposiciona sobre o território
  possuído e some se a nação perde tudo.
- **Regenerar:** `python tools/build_atlas_layers.py` (pyshp + scipy).

## Camada geopolítica (modos sobre o satélite)

Nos modos de calor, as fronteiras vêm das fronteiras reais do Natural Earth 10m,
rasterizadas na mesma projeção do terreno por `tools/build_political_layer.py`:

- `map/map_data/country_borders.png` (L8, 5000×2500): linha de 1 px onde o país
  do vizinho muda, com mipmaps para não tremer no zoom distante;
- `map/map_data/nation_fill.png` (RGBA, 2500×1250): preenchimento apenas das
  cinco nações jogáveis, com as cores do HUD;
- `map/map_shaders/political_overlay.gdshader` compõe fronteira (tinta escura a
  45%) e preenchimento (11%) em um único quad transparente — nenhuma geometria
  é reconstruída para trocar de camada.

Sobre o mapa ficam o marcador de cidade em três discos (halo escuro, disco na
cor da nação, ponto central claro), o nome em Barlow Condensed caixa alta e o
ícone de lugar (cidade/porto) logo acima do nome, com altura proporcional ao
texto e a mesma tinta do rótulo; ícone e nome somem juntos no zoom distante.
Portos ficam no ponto de costa, não no centro da cidade, e um porto que divide
o nome com a cidade dispensa rótulo próprio (o ícone/modelo marca o cais). O
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

## Câmera e navegação (estilo HOI3)

- **Inclinação:** inicial de −56° e distância 52, enquadrando o centro das cinco
  cidades na origem. Pitch limitado a −65° a −45° (range de 20°).
- **Rotação:** desabilitada por padrão. O mapa fica fixo com norte para cima,
  como em Hearts of Iron III. Jogadores de grand strategy esperam orientação
  estável; rotação livre desorienta.
- **Edge scroll:** habilitado com margem de 16 px. Move a câmera quando o mouse
  encosta nas bordas da tela; inativo com o mouse sobre qualquer controle da
  interface (barra, doca, janela, minimapa) ou fora da janela.
- **Drag pan:** arrastar com o botão do meio move a câmera (em vez de rotacionar).
  Threshold de 4 px antes de ativar para não conflitar com clique.
- **Zoom:** mínimo 10 u, máximo 500 u (cobre o mundo inteiro).
- **Wrap:** a posição da câmera dá a volta no eixo X a cada 1000 unidades,
  reaparecendo no lado oposto do mapa — o mundo continua porque há cópias
  laterais da carta.
- **Limite da folha (Z):** o centro da câmera fica entre 78°N e 60°S. Além
  disso há uma folga elástica de 24 u: empurrar (tecla, borda, arrasto) encontra
  resistência e, ao soltar, uma mola de 0,18 s devolve a câmera. Destinos de
  `move_to` (cidade, minimapa) já saem presos à folha. Implementado em
  `game/main_game.gd` (`_apply_camera_limit`, roda depois de todos os
  movimentos); teste em `tools/camera_limit_test.gd`.
- **Vinheta de tela:** `core/ui/screen_vignette.gdshader` num `CanvasLayer` 40
  (abaixo do HUD, que é 50): escurece só cantos e bordas em ~30% na cor de
  fundo do ambiente. Desligável por `VIGNETTE_ENABLED` em `main_game.gd`.
- **Vinheta de latitude:** `atlas_political` e `satellite_map` escurecem as
  latitudes extremas para esconder a borda do mundo.

## Composição

- Barra superior fixa de **48 px** (estilo HOI3, mais compacto): nação (nome +
  "Sua nação"), data e progresso do dia, quatro indicadores (população, estoque,
  déficit, trânsito), velocidade, idioma e menu. Os indicadores têm tooltip
  explicando a origem do número; ao pausar, o rótulo do dia troca para "Pausado"
  em `WARNING`. Tooltips têm borda latão e sombra de 4 px (estilo HOI3).
- **Minimapa** no canto inferior direito, 208×104 px (`core/ui/minimap.gd`):
  mapa político global estático (`map/map_data/atlas/minimap.png`, mesmo
  pipeline do atlas), borda `EDGE` de 1 px, retângulo latão vazado com a área
  vista pela câmera; clicar ou arrastar leva a câmera ao ponto.
- Doca à direita, 108 px de largura, cinco botões de 52 px (Economia,
  Comércio, Produção, População, Política) com tooltip “1 · Economia” etc. O
  botão ativo usa fundo `BG_DEEP` e borda latão.
- Janela modal responsiva: 86% × 84% da tela, limitada a 960×560 e 1800×1000;
  em 1920×1080 resulta em 1651×907. Cabeçalho com voltar (quando há contexto
  empilhado), breadcrumb, título, subtítulo, dica contextual e fechar; rodapé
  com atalhos e selo `ESC`.
- O painel de cidade (`city_panel.gd`) é empilhado sobre a tela de origem e
  organizado em cinco abas (Visão geral, População, Produção, Mercado,
  Recursos); o breadcrumb mostra a trilha e o voltar retorna ao contexto
  anterior sem abrir outra janela.
- Barra de modos de mapa no canto inferior esquerdo (político, geográfico,
  população, econômico, industrial, recursos); trocar de modo só altera
  uniformes da camada, sem reconstruir a geometria.
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

- `1`–`5` abrem os cinco painéis; `Esc` fecha o menu, depois a janela, depois
  abre o menu.
- Pilha de contexto (`_stack`): um painel é a raiz; abrir uma cidade ou uma
  cidade produtora empilha um nível e o breadcrumb (`Economia › Londres`)
  mostra onde o jogador está. O mesmo vale para os atalhos dos alertas e das
  tabelas de mercado, que abrem a cidade já na aba correspondente.
- O botão `!N` na barra superior abre a lista de alertas econômicos
  priorizados; cada linha leva à cidade responsável.
- Todo controle interativo tem `focus_mode = FOCUS_ALL`; ao abrir uma tela o
  HUD foca o primeiro controle útil (`UiKit.focus_first`) e, se a tela não tem
  controle acionável (por exemplo, lista de alertas vazia), foca o fechar.
- Abas, filtros, cabeçalhos ordenáveis e linhas de tabela também recebem foco;
  o anel de foco é latão de 1 px, sem glow.

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
  direita, reticências quando falta largura), `sort_header` (cabeçalho
  ordenável com seta e coluna ativa em latão), `row_button` (hover + seleção +
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
  HUD, para a janela de Comércio não ficar vazia nos dias sem pedido novo. A
  aba de mercado usa apenas os pedidos do dia atual; o histórico serve às abas
  de pedidos e rotas.
- Onde a simulação não tem dado (produção/construção, diplomacia, preços,
  desemprego, salários, poupança, migração), a tela declara a ausência em vez
  de inventar números.

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
- `core/ui/hud.gd` — barra, doca, janela, pilha de contexto, menu e histórico
  de pedidos.
- `core/ui/city_panel.gd` — painel de cidade com cinco abas.
- `core/ui/alerts.gd` — derivação dos alertas econômicos priorizados.
- `core/ui/screens/*.gd` — Economia, Comércio (pedidos, rotas, mercado),
  Produção, População, Política e Alertas.
- `core/ui/city_detail.gd` — painel anterior da cidade, mantido como legado e
  não usado pelo HUD.
- `core/i18n/i18n.gd` — textos em pt-BR, en e es.
- `tools/ui_capture.gd` / `.tscn` — captura e auditoria de layout.
- `docs/ui/` — capturas antes/depois e métricas objetivas.
