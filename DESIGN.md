# Design system — Open Grand Strategy (HUD)

Interface de comando sobre o mapa, na linguagem de Hearts of Iron III
modernizada. Implementada em nós `Control` do Godot, com tema construído em
`core/ui/ui_kit.gd`.

## Modo

Operate: o jogador lê o estado da economia e abre a janela certa. Densidade
alta, hierarquia tipográfica clara, nenhum número inventado.

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
| `TEXT` | `#E2DFD2` | texto principal |
| `TEXT_DIM` | `#9AA69B` | texto secundário |
| `TEXT_MUTED` | `#7F8C82` | legendas e notas |
| `BRASS` | `#C9A24A` | estado ativo, valor-chave, frete |
| `STEEL` | `#6F8F8C` | mercado, modo de transporte |
| `POSITIVE` | `#7FA86B` | sem déficit |
| `NEGATIVE` | `#D26A5F` | déficit |
| `WARNING` | `#D08A3E` | cobertura entre 2 e 7 dias |

Tipografia:

- Títulos: **Cinzel** (serifa lapidar, peso 600), 18–26 px.
- Dados, corpo e botões: **Barlow Condensed** (sans condensada), 13–16 px.
- Cabeçalho de coluna e legenda: Barlow Condensed SemiBold, 12 px, caixa alta.
- Números com separador de milhar do idioma ativo (`UiKit.number`).
- Fontes self-hosted em `assets/fonts/` (licença OFL ao lado de cada arquivo).

Forma:

- Borda sempre 1 px; raio 2 px em painéis e 6–12 px em botões.
- Sem sombras projetadas, sem gradientes, sem vidro.
- Réguas de 1 px (`UiKit.rule`) separam cabeçalho, colunas e tabela.
- Espaçamento: 4/6/10/12/14/18 px; mais espaço acima de um título que abaixo.

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

## Câmera e navegação

- Inclinação inicial de −56° e distância 52, enquadrando o centro das cinco
  cidades na origem.
- Rotação horizontal livre (360°), por Q/E ou arrastando com o botão do meio;
  a rotação vertical fica entre −90° e −15°.
- A posição da câmera dá a volta no eixo X a cada 1000 unidades, reaparecendo
  no lado oposto do mapa — o mundo continua porque há cópias laterais da carta.

## Composição

- Barra superior fixa de 52 px: nação, data, dia, progresso do dia,
  velocidade, recursos e idioma.
- Doca à direita, 96 px, com quatro diretórios: Economia, Comércio,
  Produção, Política.
- Janela central de 1240×700 com cabeçalho (título + Fechar), réguas e abas.
- Detalhe da cidade é sub-tela dentro da janela, com botão Voltar.
- Mapa sempre visível atrás; fundo escurecido apenas enquanto uma janela está
  aberta.

## Componentes

- `UiKit.row_button()`: linha de tabela clicável com hover e estado selecionado
  (borda latão).
- `UiKit.table_header` / `table_row`: peso relativo por coluna, alinhamento à
  direita para números, reticências quando falta largura.
- `UiKit.tab_button` + `set_tab_active`: aba ativa com fundo `BG_DEEP`, borda e
  texto em latão.
- `UiKit.meter`: trilho `EDGE_SOFT` com preenchimento colorido por cobertura.
- Estado vazio: centro da janela, título serifado e uma frase que explica o que
  falta na simulação.

## Movimento

Sem animação de entrada. Apenas o progresso do dia se move continuamente na
barra superior; transições de hover são imediatas para não atrapalhar leitura.

## Conteúdo e i18n

- Todo texto vem de `core/i18n/i18n.gd` (pt-BR, en, es), trocável em tempo de
  execução; as telas se reconstroem ao trocar o idioma.
- Pedidos de comércio são guardados em um histórico rolante de 240 entradas no
  HUD, para a janela de Comércio não ficar vazia nos dias sem pedido novo.
- Onde a simulação não tem dado (produção/construção, diplomacia), a tela
  declara a ausência em vez de inventar números.

## Arquivos

- `core/ui/ui_kit.gd` — tokens, tema e widgets.
- `core/ui/hud.gd` — barra, doca, janelas e histórico de pedidos.
- `core/ui/city_detail.gd` — sub-tela de cidade.
- `core/ui/screens/*.gd` — Economia, Comércio, Produção, Política.
- `core/i18n/i18n.gd` — textos em pt-BR, en e es.
