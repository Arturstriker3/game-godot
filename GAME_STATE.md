# GAME_STATE.md — WarEconomy / Open Grand Strategy

> Fonte central de conhecimento do jogo. Consulte antes de qualquer mudança importante e atualize as seções afetadas ao concluir uma implementação.
>
> - **Última auditoria:** 2026-11-09 (auditoria econômica; núcleo demo alterado e laboratório de 40 execuções executado).
> - **Fonte última da verdade:** o código. Em divergência entre este documento e o código, o código vence.
> - **Nome:** o projeto vive em `G:\projetos\opengs-game`; o app se chama **Open Grand Strategy** (`project.godot`), mas o menu de pausa e documentos internos usam o nome **WarEconomy**. `README.md` descreve o mapa upstream "OpenGS" (províncias), que coexiste com o jogo de cidades como ferramenta de edição.

---

## 1. Visão geral

- **Gênero:** grand strategy 3D (estilo Paradox/Supreme Ruler), focado em **economia e logística**, não em guerra.
- **Conceito central:** simular o comércio de **trigo** entre cidades reais da Europa Ocidental (produção, demanda, estoque, frete e tempo de trânsito por caminhão, navio e avião).
- **Posicionamento:** mapa e economia são o mesmo objeto — cada cidade é fonte de produção e ponto de demanda; as fronteiras nascem da posição das cidades (Voronoi/Natural Earth), não de províncias pintadas à mão.
- **Escala:** 5 nações, 5 cidades (Londres, Paris, Berlim, Amsterdã, Bruxelas) no modo ao vivo; o cenário de validação usa 2 cidades fictícias (AGRIA, PORTUS).
- **Experiência:** sessão longa, pausa/velocidades, painéis densos, leitura do estado do mundo; o tempo avança em dias.
- **Diferenciais:** núcleo econômico em C++ (GDExtension) com conservação de dinheiro verificada por teste; interface "estado puro" que nunca executa lógica econômica; nenhum número inventado.
- **Estado do protótipo:** jogável com HUD completo, economia de trigo ao vivo e mapa de satélite 3D. O ciclo econômico "rico" (dinheiro, preços, salários, POPs) está implementado e testado apenas no cenário demo, ainda não ligado ao loop principal.

Fontes: `PRODUCT.md`, `DESIGN.md`, `README.md`.

---

## 2. Estado de desenvolvimento

| Sistema | Status | Funcionamento atual | Evidências |
|---|---|---|---|
| Núcleo C++ `EconomySim` | IMPLEMENTADO | 3 modos: legacy (trigo, 5 cidades), demo (cadeia completa), benchmark | `gdextension/src/economy_sim.{h,cpp}` |
| Economia ao vivo (jogo) | PARCIAL | Só trigo (produção/consumo/estoque/frete); sem dinheiro/preços/salários | `core/economy/economy.gd`, `game/main_game.gd` |
| Ciclo econômico completo | IMPLEMENTADO (demo) | dinheiro, preços, salários, PIB, investimento, save/load | `tick_demo()`, `tools/economy_*_test.gd`, `tools/economy_lab.gd` |
| IA econômica | IMPLEMENTADO (demo) | investidor privado, governo, prospecção | `core/economy/ai/*`, `economy_ai_test.gd` |
| Recursos minerais (extração/esgotamento) | PARCIAL | depósitos + prospecção; extração/esgotamento não estão no núcleo | `core/economy/geography_service.gd`, `data/definitions/natural/deposits.json` |
| População/POPs | PARCIAL | demo simula POPs, emprego, salários, poupança; migração/demografia ausentes | `economy_sim.cpp` (`DemoPop`) |
| Mapa principal (cidades) | IMPLEMENTADO | satélite + overlay político + modos de calor + LOD | `map/city_world.gd`, `map/map_shaders/*` |
| Mapa de províncias (editor) | IMPLEMENTADO | Voronoi + SDF/JFA, 5 modos; usado só pelo editor | `map/map.gd`, `map/map_texture_sdf.gd` |
| Relevo/altura real | NÃO IMPLEMENTADO | `height.png`/`normal.png` são placeholders planos | `generate_opengs_map.py`, `docs/map_visual/REPORT.md` |
| Cidades 3D + crescimento urbano | IMPLEMENTADO | módulos GLB + `core/urban/*` + LOD | `city_world.gd`, `core/urban/*` |
| HUD + telas | IMPLEMENTADO | barra, doca, janela, 6 telas, painel de cidade | `core/ui/hud.gd`, `core/ui/screens/*` |
| Design system (UiKit) | IMPLEMENTADO | tokens, tema, ~30 componentes | `core/ui/ui_kit.gd` |
| i18n (pt-BR/en/es) | IMPLEMENTADO | runtime; lacunas nos protótipos (hardcoded pt-BR) | `core/i18n/i18n.gd` |
| Save/load do jogo | NÃO IMPLEMENTADO | economia só em memória (demo); urbano em `user://` (protótipo) | `demo_to_dict/demo_from_dict`, `urban_persistence.gd` |
| Política/diplomacia | PARCIAL | telas existem; todas as nações "em paz", sem simulação | `core/ui/screens/politics_screen.gd` |
| Exército, guerra, tecnologia, fábricas, dinheiro por nação | PLANEJADO | declarados ausentes em `PRODUCT.md` | `PRODUCT.md` |
| Áudio (música + SFX) | IMPLEMENTADO | 7 faixas + 100 SFX (14 eventos ligados) | `core/audio/*` |
| Editor de províncias in-game | IMPLEMENTADO | cena separada `editor/editor.tscn` | `editor/*` |

---

## 3. Arquitetura técnica

- **Engine:** Godot 4.7, renderizador **Forward+**, `config/features=PackedStringArray("4.7", "Forward Plus")`.
- **Main scene:** `res://game/main_game.tscn`.
- **Autoloads** (`project.godot`):
  - `Music` — `core/audio/background_music.gd` (7 faixas `.ogg`).
  - `UiSfx` — `core/audio/ui_sfx.gd` (efeitos de interface).
  - `I18n` — `core/i18n/i18n.gd` (pt-BR/en/es em runtime).
  - `Economy` — `core/economy/economy.gd` (ponte para o C++).
- **GDScript:** 81 arquivos de jogo (~12.363 linhas) + add-ons. Tipado, `class_name`, sinais/grupos.
- **C++ / GDExtension:** `gdextension/src/economy_sim.{h,cpp}` + `register_types.*`; compilado por `CMakeLists.txt`; binário `bin/economy_gdextension.windows.*.x86_64.dll`; `gdextension/economy.gdextension` registra o addon.
- **`EconomySim` (C++):** classe única `RefCounted` com **três modos** mutuamente exclusivos:
  - `initialize()` / `tick()` / `get_status()` — **legacy**, 5 cidades, só trigo. É o que o jogo ao vivo usa.
  - `initialize_demo()` / `tick_demo()` / `get_demo_status()` + `demo_invest/build/spend_government/price_shock/to_dict/from_dict` — **demo**, 2 cidades, ciclo completo.
  - `initialize_benchmark()` / `run_benchmark()` — sintético, determinístico.
- **Sistema de ticks:** o autoload `Economy` usa um `Timer`; `SPEEDS=[0.0, 1.5, 0.75, 0.3]` segundos reais por dia simulado (0 = pausa). Cada `_run_day()` chama `sim.tick()` e emite `status_updated(summary)` e `trade_orders_ready(orders)`.
- **Eventos/sinais (fluxo unidirecional):** `Economy.status_updated` → HUD (`hud.gd::_on_status`) e `main_game.gd::_on_status_updated` (refresh dos modos). `Economy.trade_orders_ready` → `trade_convoys.gd::spawn_orders`. `HUD.city_focused`/`map_mode_requested` → `main_game.gd`. `I18n.locale_changed` → `hud.gd`. A UI nunca executa lógica econômica.
- **Persistência:** a economia demo serializa em memória (`demo_to_dict`/`demo_from_dict`, versão 1); não há gravação em disco da partida. O layout urbano salva em `user://` (`urban_persistence.gd`, versão 1).
- **Separação simulação × apresentação:** C++ calcula; GDScript orquestra (autoload, geografia, decisão de investimento, visualização). A UI recebe texto/valores já formatados.

---

## 4. Mapa e geografia

- **Dois sistemas de mapa coexistem:**
  1. **Jogo principal — `map/city_world.gd`:** base geográfica = `satellite.jpg` (8192×4096, equirretangular global, Esri World Imagery) via `satellite_map.gdshader`; overlay político via `political_overlay.gdshader` (`country_borders.png` + `nation_fill.png`); modos de calor via `map_modes.gdshader` + `map_mode_layer.gd`.
  2. **Editor — `map/map.gd` + `map/map_mode.gd`:** mapa raster de províncias (SubViewport/Sprite2D sobre plano 3D), com SDF/JFA de fronteiras; usado por `editor/editor.tscn` e `tools/map_capture.gd`, **não** pelo jogo.
- **Projeção/coordenadas:** equirretangular; conversão pixel↔lonlat em `build_natural_geography.py` (`lon=px/5000*360−180`, `lat=90−py/2500*180`). Mundo = plano de 1000×500 unidades (`PIXELS_PER_WORLD_UNIT=5`).
- **Camadas raster (`map/map_data/`):**
  - `provinces.png` 5000×2500 (cor-chave por província, fonte do lookup/SDF).
  - `ne2.png` 5000×2500 (terreno Blue Marble, mapa legado).
  - `height.png`/`normal.png` 5000×2500 — **placeholders planos** (sem relevo real).
  - `satellite.jpg` 8192×4096 (base do jogo).
  - `country_borders.png` 5000×2500 e `nation_fill.png` 2500×1250 (Natural Earth 10m).
  - `nav_mask.png` 2500×1250 (terra/água/costa, pathfinding A* dos comboios).
- **Províncias/territórios/países:** `data/history/provinces/provinces.json` (14.402 províncias), `data/history/territories/territories.json` (737 territórios), `data/history/countries/` (193 JSON); `data/definitions/*.txt` restringidos a 4 países (NNN/GBR/FRA/DEU). O jogo ao vivo usa **5 nações** (GBR, FRA, DEU, NED, BEL).
- **Fronteiras:** legado = Voronoi + SDF via Jump Flooding Algorithm (`jfa.glsl`/`jfa_encode.glsl` + `map_texture_sdf.gd`); jogo = raster Natural Earth composto por shader.
- **Câmera/navegação:** `core/utils/camera_controller.gd` (WASD, zoom, rotação Q/E ou botão do meio); pitch −90° a −15°; inclinação inicial −56°, distância 52; wrap cilíndrico no eixo X.
- **Fontes de dados:** Natural Earth 10m, NASA GIBS Blue Marble, Esri World Imagery, USGS MRDS (depósitos).
- **Limitações:** sem relevo real (heightmap/normal planos); `generate_opengs_map.py` com parâmetros divergentes dos dados commitados (8192×, lat −60..84 vs 5000×2500 global); terreno de todas as províncias = PLAINS.

---

## 5. Cidades e crescimento urbano

- **Cadastro de cidades:** modo legacy define 5 cidades hardcoded no C++ (`define_cities()`): Londres (1300 t/d), Paris (6000), Berlim (2500), Amsterdã (300), Bruxelas (400).
- **Render 3D (`city_world.gd`):** níveis por distância — continental (`Label3D`), regional (marcador + `city.glb`), local (cidade modular). `MAX_DETAILED_CITIES=40`, `MAX_CITY_LABELS=24`; resto vira `MultiMesh` de discos.
- **LOD:** fade do modelo 30–92 u, marcador 26–34 u, cidade modular construída ≤30 u e liberada >44 u (histerese).
- **Seleção:** `pick_city()` (projeção, sem física), anel de latão, `select_city()`.
- **Crescimento procedural (`core/urban/`):** `UrbanGrowthManager` (estágio por população+industrialização) → `DistrictPlanner` (quarteirões ~75 m) → `UrbanRenderer` (um `MultiMeshInstance3D` por arquétipo + LOD de caixas). Lotes nunca removidos, só melhorados.
- **Arquitetura:** estilo único semi-realista europeu (pedra/concreto/tijolo), módulos GLB em `assets/models/city_modules/` (`library.json`). `CITY_BIAS` em `industry_profile.gd` redistribui setores por cidade — **não** há arquitetura regional por país.
- **Integração econômica:** população/industrialização decidem o estágio; renda puxa torres; empregos sustentam escritórios; industrialização libera a zona industrial. Indicadores vêm do `EconomySim`.
- **Persistência:** `urban_persistence.gd` (JSON em `user://`, hash de determinismo); usado pelo protótipo `game/city_prototype.tscn`.

---

## 6. Economia

> Há **dois contratos econômicos** no mesmo `EconomySim`: o **legacy** (trigo por cidade, usado no jogo) e o **demo** (cadeia completa com dinheiro, usado nos testes/protótipos/IA). Não é bug, mas é uma bifurcação a ter consciência.

### 6.1 Modo ao vivo (legacy — trigo)

- **Produção:** constante por cidade (`production`, t/dia), sem cadeia/insumos.
- **Demanda/consumo:** `need = population × 0.0003` t/dia; `consumed = min(stockpile, need)`; `deficit = need − consumed`.
- **Estoque:** `stockpile` acumula produção e descargas.
- **Comércio:** mercado em 3 camadas — regional (<500 km, mesmo país), nacional, internacional; `dispatch_trade` escolhe o vendedor mais próximo e `build_route` monta a rota; ponte aérea quando estoque < 2 dias.
- **Transporte/frete:** por trecho (`make_leg`), custo = carga × distância × `cost_per_ton_km`; tempos por modo (caminhão 65 km/h/1200 t/R$0,10; navio 35 km/h/12000 t/R$0,02; avião 750 km/h/400 t/R$0,80).
- **Indicadores derivados:** `jobs`, `income`, `industrialization` são calculados, não simulados.

### 6.2 Modo demo (ciclo completo — trigo→farinha→pão + ferro)

- **Cidades:** AGRIA (100 mil, 3 fazendas) e PORTUS (200 mil, 2 fazendas, moinho + 5 padarias), rota de trigo com frete.
- **Produtos:** trigo (0), farinha (1), pão (2), ferro (3); preços-base 40/70/80/60.
- **Cadeia:** trigo → farinha (1,2) → pão (0,83); ferro é extração primária.
- **Produção:** `desired_output = target × capacity_share × labor`, limitada por capacidade (× aptidão) e insumos. O alvo repõe metade da distância até um estoque de 3 dias de venda, em vez de produzir 10% acima das vendas para sempre (isso acumulava estoque sem limite).
- **Preços:** reagem a escassez com piso de custo, suavização 0,20, banda [0,5×, 3×]. O limite diário de ±5% é aplicado **uma vez**, no fim do tick, depois de importação e reação de mercado; antes os dois canais somavam e produziam saltos de 10%.
- **Consumo:** famílias compram pão → farinha → trigo por orçamento; demanda externa de ferro fixa 20 t/d. Serviços são pagos antes do alimento (45% da renda disponível); é uma simplificação conhecida, não uma regra de prioridade alimentar.
- **Dinheiro:** contas (famílias, empresas, serviços, governo, transporte, exterior); todo débito tem crédito; `money_total` conservado. Empresas e serviços não pagam folha/opex/importação além do caixa disponível; antes o caixa ficava negativo em cenários deficitários.
- **PIB:** valor adicionado (receita − insumos) + serviços; `gdp_per_capita = gdp/population`. Receita de construção/investimento pedida entre ticks entra no PIB do tick seguinte (`construction_revenue_pending`), em vez de ser zerada pelo reset de fluxos.
- **Emprego/salários:** força de trabalho = 55%; o salário de serviços busca o equilíbrio do trabalho residual, com piso no custo de subsistência e 4% de desemprego friccional. O antigo salário fixo de 0,15/dia + teto de 30% de serviços produzia 77–90% de desemprego por construção. Construções mantêm salário nominal fixo (2,0/1,5), criando um mercado de trabalho dual.
- **Lucro/capital:** lucro = receita − custos; 100% do lucro positivo acima do capital de giro vai às famílias (renda de capital); `demo_invest` expande a R$2000/t-dia.
- **Impostos:** 15% sobre (salário + capital); transferências = 100% (orçamento equilibrado).

Arquivos: `gdextension/src/economy_sim.cpp` (regras), `core/economy/economy.gd` (autoload), `core/economy/geography_service.gd` (aptidão/depósitos), `core/economy/trade_convoys.gd` (animação).

### 6.3 Laboratório de balanceamento

- **Runner:** `tools/economy_lab.gd` usa o mesmo `EconomySim` demo; só o estado inicial é controlado. 8 cenários × 5 horizontes (30/90/365/1825/3650 dias).
- **Dados:** `data/definitions/economy/balance.json` (cenários, preços-base, horizontes, amostragem, limiares de alerta e o bloco `model` aplicado via `demo_set_balance`: desemprego friccional, ajuste de salário de serviços, estoque-alvo e recuperação).
- **Saídas:** `docs/economy_lab/<tag>/runs.csv`, `runs.json` e um CSV por cenário/horizonte; `tools/economy_report.py` gera gráficos em `docs/economy_lab/charts/` e `metrics.md`.
- **Invariantes duras:** conservação de dinheiro, caixas/estoques/poupança não negativos, preço finito e dentro de [0,5×, 3×], passo diário ≤5%, PIB = valor adicionado, produção ≤ capacidade, emprego + desemprego = força de trabalho, consumo ≤ necessidade.
- **Alertas econômicos (não falhas):** desemprego >25%, satisfação <0,8, estoque >365 dias de venda e conta externa negativa.
- **Resultado de referência (tag `final`):** 40 execuções, 0 violações duras; crises emergentes nos cenários 02/04/07; relatório em `docs/economy_lab/REPORT.md`.

---

## 7. Recursos naturais

- **Aptidão agrícola:** `data/definitions/natural/agriculture.json` (8 culturas: trigo, arroz, milho, batata, soja, cana, café, fruta), `data_quality:"test"`; só **trigo** é aplicado ao sim (`GeographyService.apply_to_sim`).
- **Depósitos minerais:** `deposits.json` (≈289 mil registros, fonte `tools/data/mrds_deposits.jsonl` com 304.632 registros — contagens divergentes); campos: `resource`, `px/py/lon/lat`, `quantity_kt`, `recoverable_kt`, `quality`, `depth_m`, `extraction_cost`, `max_rate_kt_day`, etc.
- **Prospecção:** só na IA (`ResourceKnowledge.prospect`), custo 2000/sítio.
- **Extração/esgotamento:** **não implementados** no núcleo — a mina de ferro produz como fazenda, sem consumir reserva.
- **Conhecimento do jogador vs interno:** fog-of-war de depósitos na IA (`ResourceKnowledge`); o C++ não distingue.
- `nature.json`: geofísica por território (clima, solo, relevo, fertilidade, água), `data_quality:"test"`, não entra na regra econômica.

---

## 8. Indústria

- **Produtos:** trigo, farinha, pão, ferro (demo); só trigo no jogo ao vivo.
- **Receitas produtivas:** `input_per_output` (1,2 trigo→farinha; 0,83 farinha→pão) hardcoded no C++ e duplicados em `core/economy/ai/industry_templates.gd`.
- **Edifícios industriais:** `DemoBuilding` (`owner` private/state, `capacity`, `jobs`, `wage`, `opex`, `utilization`, `output_scale`, `loss_days`).
- **Capacidade/produtividade/custos:** `capital_cost_per_capacity`, `opex`, `input_value`, `profit` calculados por construção.
- **Expansão industrial:** `demo_invest`/`demo_build` (C++) + `PrivateInvestor`/`GovernmentAI` (GDScript).
- **Distrito industrial visual:** `core/urban/industrial_planner.gd` + `industry_profile.gd` (5 setores: aço, manufatura, refino, energia, armazenagem).

---

## 9. População

- **Legacy:** população fixa por cidade; `jobs`, `income`, `industrialization` são derivados.
- **Demo (`DemoPop`):** POPs agregados — `labor_force`, `employed`, `unemployed`, `dependents`, `wage_income`, `capital_income`, `transfers`, `taxes`, `disposable`, `food_spend`, `consumed`, `need`, `satisfaction`, `savings`, consumo por categoria.
- **Padrão de vida:** `living_standard = disposable_per_capita / custo_da_cesta`.
- **Crescimento populacional:** **ausente no núcleo** (só o laboratório injeta crescimento externo).
- **Migração:** **ausente** (declarada como não simulada na i18n e em `population_screen.gd`).
- **Divergência de documentação:** a i18n e `DESIGN.md` ainda dizem que emprego/salários/poupança "não são simulados", mas o demo **simula** os três (só a migração realmente não existe).

---

## 10. Governo e IA econômica

- **Orçamento público:** demo — impostos 15%, transferências 100%, gasto via `demo_spend_government`.
- **Empresas privadas/públicas:** `owner` = private/state; lucro de estatal vai ao orçamento, privado às famílias.
- **IA econômica (GDScript, `core/economy/ai/`):** determinística, intervalo de 7 dias, **não conectada ao jogo ao vivo**.
  - `EconomicAI` — orquestrador.
  - `PrivateInvestor` — investe/expande se margem > 0,5/t e payback < 150 dias.
  - `GovernmentAI` — prospecção mineral, estatal de segurança alimentar, mina de ferro (limite 25% do orçamento).
  - `ResourceKnowledge` — fog-of-war de depósitos.
  - `IndustryTemplates` — parâmetros de construção (duplicam o C++).
- **Políticas econômicas:** limitadas a regras fixas (impostos/transferências); não há interface de política ativa.

---

## 11. Interface e UX

- **Design system:** `core/ui/ui_kit.gd` (`UiKit`) — tokens (BG_DEEP `#0A1112`, PANEL, latão `#C9A24A`, etc.), tipografia (Cinzel para títulos, Barlow Condensed para corpo), escala 26/19/15/15/13/11 px, tema global, ~30 construtores (`metric`, `status_pill`, `table_row`, `empty_state`, `toast`, etc.).
- **HUD (`core/ui/hud.gd`):** barra superior de 56 px (nação, data, progresso do dia, 4 indicadores com tooltip, velocidade, idioma, alertas, menu); doca direita (5 botões, atalhos 1–5); barra de modos de mapa (6 chips); janela modal responsiva (86%×84%, 960×560 a 1800×1000); menu de pausa (Esc).
- **Telas (`core/ui/screens/`):** Economia, Comércio (Pedidos/Rotas/Mercado), Produção, População, Política (Nações/Diplomacia/Dossiê), Alertas.
- **Painel de cidade:** `core/ui/city_panel.gd` (5 abas). `core/ui/city_detail.gd` é **legado**; `core/ui/city_economy_panel.gd` serve ao cenário demo.
- **Navegação:** pilha de contexto com breadcrumb; foco por teclado (anel de latão); Esc fecha menu → janela → abre menu.
- **Estados vazios:** princípio "estado vazio honesto" — onde não há dado (produção, política, preços, desemprego), a tela declara a ausência.
- **i18n:** `core/i18n/i18n.gd` — ~330 chaves × {pt-BR, en, es}; troca em runtime; lacunas nos protótipos (texto fixo em pt-BR em `city_economy_panel.gd`, `game/*_prototype.gd`).
- **Áudio:** `Music` (7 faixas) + `UiSfx` (Kenney Interface Sounds 1.0.0, CC0, 100 WAV em `assets/audio/interface/`). Eventos ligados: clique, seleção, alternância, voltar, abrir, fechar, confirmar, erro, rolagem, arraste de slider, troca de aba, pergunta, maximizar e minimizar janela. Rolagem e arraste usam `play_throttled` (90 ms / 55 ms por controle).

---

## 12. Salvamento

- **Economia (demo):** `demo_to_dict()`/`demo_from_dict()` — em memória, `version=1`, round-trip validado por teste.
- **Urbano:** `core/urban/urban_persistence.gd` — JSON em `user://`, `VERSION=1`, hash de determinismo (protótipo).
- **Partida completa / estado da UI:** **não implementado** — o menu não tem "Salvar jogo".
- **Compatibilidade:** serialização versionada no demo e no urbano; sem migração entre versões documentada.

---

## 13. Performance

- **Cidades:** `city_render_benchmark.gd` valida 5000 cidades com orçamento de 6000 nós (MultiMesh + `MAX_DETAILED_CITIES=40`).
- **Tick:** benchmark sintético mede tempo por fase (`run_benchmark`).
- **Memória/render:** Forward+, MSAA 2×, debanding, anisitrópico 3; LOD urbano por categoria 140–900 u.
- **Benchmarks documentados:** `docs/map_visual/REPORT.md` (mapa 3D 42% mais escuro que o 2D; nitidez cai no zoom; rótulos ilegíveis a d≥52; sem MSAA/TAA).
- **Limites conhecidos:** SDF de país rebaixado 4× e atualizado de forma incremental/bounded; relevo nulo; escala urbana estilizada (2,8 u ≈ 112 km) não documentada.

---

## 14. Registro de decisões

| ID | Decisão | Justificativa / consequência |
|---|---|---|
| D-001 | Núcleo econômico em C++ (GDExtension), UI em GDScript | Performance determinística; UI não executa lógica |
| D-002 | Modo ao vivo usa só trigo (legacy); demo valida ciclo completo antes de generalizar | Reduz risco; mantém duas faces do mesmo núcleo |
| D-003 | Design "Industrial Strategy — Premium Dark" (HOI3 modernizado) | Referência declarada do usuário |
| D-004 | UI é "estado puro": recebe texto/valores formatados, nunca calcula | `PRODUCT.md`/`DESIGN.md` |
| D-005 | Fronteiras do jogo = Natural Earth raster; SDF/JFA fica no editor | Duas representações de mapa coexistindo |
| D-006 | Estados vazios honestos (não inventar números) | `PRODUCT.md` princípio 2 |
| D-007 | i18n pt-BR/en/es em runtime, default pt-BR | `PRODUCT.md` |
| D-008 | Orçamento público equilibrado (imposto 15% = transferências) | Demo; simplificação declarada |
| D-009 | IA econômica determinística (sem LLM), a cada 7 dias | Previsibilidade/testabilidade |
| D-010 | Crescimento urbano determinístico (seed por cidade) e lotes nunca removidos | Identidade dos bairros |
| D-011 | Clique em cidade sem física (projeção de viewport) | Simplicidade |

---

## 15. Bugs e dívida técnica

- **BOM `U+FEFF`** em `core/ui/city_economy_panel.gd` (linha 1) — quebra `tools/economy_panel_test.gd` (único arquivo com BOM).
- **Duas faces econômicas** (legacy × demo) no mesmo `EconomySim`, consumidas por UIs diferentes; risco de divergência.
- **Três implementações paralelas** de produção/consumo/preços/contabilidade no C++ (tick_demo × benchmark × legacy).
- **Parâmetros duplicados:** `IndustryTemplates` (GDScript) repete `jobs/wage/opex/input_ratio` do C++ sem vínculo; preços-base em `balance.json` × `demo_base_price()`.
- **Dois sistemas de mapa:** `city_world.gd` (jogo) × `map.gd` (editor) — dois domínios (cidades × províncias).
- **Três painéis de cidade:** `city_panel.gd` (ativo), `city_detail.gd` (legado), `city_economy_panel.gd` (demo).
- **Cores de nação divergentes:** `ui_kit.gd`/`build_political_layer.py` × `city_world.gd`.
- **Documentação defasada:** i18n/DESIGN dizem que emprego/salários/poupança "não são simulados", mas o demo simula.
- **Fome com alimento em estoque (02/07):** serviços são pagos antes do alimento e não há crédito, governo comprador nem mercado externo de alimentos; a cidade empobrece, não compra o estoque e a satisfação cai. É crise de demanda/liquidez, não violação contábil.
- **Mercado de trabalho dual:** serviços ajustam o salário para ~0,03/dia, enquanto fazendas/moinho/padarias ficam fixos em 2,0/1,5; o diferencial (~60×) é uma calibração pendente.
- **Conta externa sem orçamento:** a demanda externa de ferro (20 t/dia) é paga por `foreign_cash` negativo sem limite; o dinheiro doméstico é conservado, mas o exterior concede crédito infinito.
- **Construção one-off:** `demo_invest`/`demo_build` caem como receita de serviço/construção no tick seguinte; um investimento de 100 mil em cidade de 100 mil habitantes cria um pico de PIB de 1,0 no dia.
- **Demografia ausente:** crescimento populacional do cenário 05 é aplicado pelo laboratório; o núcleo não modela nascimentos, mortes ou migração.
- **Relevo placeholder:** `height.png`/`normal.png` planos; `height_scale=0`.
- **Arquivos órfãos:** `game/ui/province_selected.gd/.tscn` (não referenciado), `main_game.tscn*.tmp`, `assets/fonts/IMPRISHA.TTF`/`ROMAN.FON`, `assets/audio/Open_World_-_Happiness_Full.WAV`, `map/lookup_texture.gd.uid`/`province_border_texture.gd.uid` (UIDs sem script), `tactical_map.gdshader` sem referências.
- **Contagens divergentes:** `deposits.json` (289.575) × `mrds_deposits.jsonl` (304.632); `generate_opengs_map.py` (8192×, lat −60..84) × dados commitados (5000×2500 global).
- **Contagem de nações:** 4 países nas definições × 5 nações no jogo.

---

## 16. Roadmap

**Implementado:** núcleo econômico (legacy + demo), HUD + 6 telas, design system, i18n, mapa de satélite 3D, crescimento urbano/industrial, comboios, IA econômica, editor de províncias, áudio, testes.

**Em desenvolvimento / parcial:** integração do modo demo ao loop principal; extração/esgotamento mineral; save/load de partida.

**Próximas prioridades (recomendadas pela auditoria):**
1. Corrigir o BOM de `core/ui/city_economy_panel.gd`.
2. Decidir a consolidação dos dois sistemas de mapa e dos três painéis de cidade.
3. Unificar os dois contratos econômicos (promover o demo ao jogo ao vivo) ou documentar a coexistência.
4. Sincronizar `IndustryTemplates` com o C++ (fonte única de parâmetros).
5. Implementar save/load de partida.
6. Substituir `nature.json`/`agriculture.json` por dados reais (WorldClim/FAO/SRTM) — já referenciado em `_sources.json`.
7. Gerar relevo real (heightmap/normal) para o mapa.

**Planejado (sem código):** exército, guerra, tecnologia, política interna, construção de fábricas, dinheiro por nação (declarados ausentes em `PRODUCT.md`).

---

## 17. Guia para agentes

### Arquivos essenciais
- `GAME_STATE.md` (este), `PRODUCT.md`, `DESIGN.md`, `README.md`.
- `gdextension/src/economy_sim.{h,cpp}` — regras econômicas (fonte de verdade).
- `core/economy/economy.gd` — autoload/ponte C++.
- `core/ui/ui_kit.gd` + `core/ui/hud.gd` + `core/ui/screens/*` — interface.
- `map/city_world.gd` + `map/map_shaders/*` — mapa do jogo.
- `core/urban/*` — crescimento urbano/industrial.
- `core/i18n/i18n.gd` — textos.

### Convenções
- GDScript tipado; nomes de código em inglês; comentários/UI em pt-BR.
- UI em nós `Control`, via `UiKit`; não inventar números — usar estado vazio.
- Sinais para comunicar economia → UI; nunca lógica econômica na UI.
- Não alterar `project.godot` sem autorização; não editar o C++ sem build/teste.

### Como executar
```powershell
$godot = "C:\Users\artur\OneDrive\Área de Trabalho\Godot_v4.7.2-stable_win64.exe"
& $godot --path .   # abre o jogo
```

### Como testar (rode só o relevante; veja `AGENTS.md` §Testes)
```powershell
& $godot --headless --path . --script res://tools/economy_integration_test.gd   # rápido
& $godot --headless --path . --script res://tools/economy_ai_test.gd            # rápido
& $godot --headless --path . --script res://tools/population_demo_test.gd       # rápido
& $godot --headless --path . --script res://tools/economy_long_run_test.gd      # lento
# Laboratório de balanceamento (8 cenários × 5 horizontes):
& $godot --headless --path . --script res://tools/economy_lab.gd -- --tag=final
& "C:\Users\artur\AppData\Local\Python\bin\python3.exe" tools/economy_report.py --tag final
# Capturas:
& $godot --path . res://tools/ui_capture.tscn -- --phase=f4 --run=final --sizes=1920x1080,1600x900,1366x768,2560x1440
& $godot --path . res://tools/map_capture.tscn -- --set=all --mode=political --tag=before --run=baseline_before
```

### Como atualizar este documento
1. Consulte o `GAME_STATE.md` antes de mudanças importantes.
2. Ao concluir uma implementação que altere sistema/mecânica/dados/arquitetura, atualize **apenas as seções afetadas** e o status na tabela §2.
3. Registre decisões novas em §14 e problemas em §15.
4. Não marque como IMPLEMENTADO sem evidência (teste/captura); use NÃO VERIFICADO quando não executou.
5. Mantenha o documento legível; detalhes extensos ficam em `docs/`.
