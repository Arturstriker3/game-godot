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

## Mapa tático

O fundo é o relevo Natural Earth (`map/map_data/ne2.png`) passado pelo shader
`map/map_shaders/tactical_map.gdshader`, que o converte em carta militar:

- mar em três tons de petróleo, com plataforma continental calculada pela
  vizinhança de terra;
- terra em oliva → cáqui, graduada pela luminância do relevo original;
- gelo em pálido dessaturado;
- graticule de 15° em latão a 7% de opacidade;
- sem granulado animado (o grão por `sin` gerava artefatos pretos em alguns
  drivers; a versão atual usa só ruído estável).

Sobre o mapa ficam as células de Voronoi das nações (preenchimento a 10%,
borda a 72%, cores dessaturadas), o marcador de cidade em três discos (halo
escuro, disco na cor da nação, ponto central claro) e o nome em Barlow
Condensed caixa alta.

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
