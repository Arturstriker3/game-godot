# GAME_STATE.md — WarEconomy / Open Grand Strategy

> Fonte central de conhecimento do jogo. Consulte antes de qualquer mudança importante e atualize as seções afetadas ao concluir uma implementação.
>
> - **Última auditoria:** 2026-11-09 (mapa político refeito sobre campos de distância vetoriais; minimapa estático funcional; correções de Holanda, halo branco e cópias laterais).
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
- **Plataforma/stack:** desktop; Godot 4.7 (Forward+), interface em nós `Control`; núcleo econômico em C++17 via GDExtension.
- **Usuários:** jogadores de grand strategy que já conhecem Paradox/Supreme Ruler; sessões longas, painéis densos, sem tutorial.
- **Princípios de produto:** (1) o dado vem antes do ornamento; (2) nada de número inventado; (3) densidade legível; (4) o mapa é a superfície principal; (5) trocar de idioma não quebra layout nem rótulos.
- **Compromissos de marca:** referência declarada Hearts of Iron III em versão modernizada (painéis escuros e latão); idioma padrão pt-BR, com en e es.
- **Acessibilidade:** texto principal ≥ 13 px, contraste alto sobre painéis escuros, alvos de clique ≥ 28 px de altura, foco visível.
- **Fora do escopo atual:** exército, guerra, tecnologia, política interna, construção de fábricas e dinheiro por nação.

Fontes: `DESIGN.md` (design system) e `docs/` (evidências). Este documento absorveu o antigo `PRODUCT.md`.

---

## 2. Estado de desenvolvimento

| Sistema | Status | Funcionamento atual | Evidências |
|---|---|---|---|
| Núcleo C++ `EconomySim` | IMPLEMENTADO | 3 modos: legacy (trigo, 5 cidades), demo (cadeia completa), benchmark | `gdextension/src/economy_sim.{h,cpp}` |
| Economia ao vivo (jogo) | PARCIAL | Só trigo (produção/consumo/estoque/frete); sem dinheiro/preços/salários | `core/economy/economy.gd`, `game/main_game.gd` |
| Ciclo econômico completo | IMPLEMENTADO (demo) | dinheiro, preços, salários, PIB, investimento, save/load | `tick_demo()`, `tools/economy_*_test.gd`, `tools/economy_lab.gd` |
| IA econômica | IMPLEMENTADO (demo) | investidor privado, governo (sem prospecção — tudo conhecido) | `core/economy/ai/*`, `economy_ai_test.gd` |
| Recursos minerais (extração/esgotamento) | IMPLEMENTADO | 14 minerais + 7 renováveis; mina consome reserva (`demo_set_deposit`); qualidade→eficiência, profundidade→custo | `core/economy/geography_service.gd`, `data/definitions/natural/deposits.json` |
| População/POPs | PARCIAL | demo simula POPs, emprego, salários, poupança; migração/demografia ausentes | `economy_sim.cpp` (`DemoPop`) |
| Mapa principal (cidades) | IMPLEMENTADO | modo atlas (SDF vetorial, províncias admin-1, território dinâmico por cidade, nomes de nação) + satélite + overlay político + modos de calor + LOD | `map/city_world.gd`, `map/map_shaders/*`, `tools/build_atlas_layers.py`, `docs/map_visual/atlas_final/` |
| Mapa de províncias (editor) | IMPLEMENTADO | Voronoi + SDF/JFA, 5 modos; usado só pelo editor | `map/map.gd`, `map/map_texture_sdf.gd` |
| Relevo/altura real | NÃO IMPLEMENTADO | `height.png`/`normal.png` são placeholders planos | `generate_opengs_map.py`, `docs/map_visual/REPORT.md` |
| Cidades 3D + crescimento urbano | IMPLEMENTADO | módulos GLB + `core/urban/*` + LOD | `city_world.gd`, `core/urban/*` |
| HUD + telas | IMPLEMENTADO | barra, doca, janela, 6 telas, painel de cidade | `core/ui/hud.gd`, `core/ui/screens/*` |
| Design system (UiKit) | IMPLEMENTADO | tokens, tema, ~30 componentes | `core/ui/ui_kit.gd` |
| i18n (pt-BR/en/es) | IMPLEMENTADO | runtime; lacunas nos protótipos (hardcoded pt-BR) | `core/i18n/i18n.gd` |
| Save/load do jogo | NÃO IMPLEMENTADO | economia só em memória (demo); urbano em `user://` (protótipo) | `demo_to_dict/demo_from_dict`, `urban_persistence.gd` |
| Política/diplomacia | PARCIAL | telas existem; todas as nações "em paz", sem simulação | `core/ui/screens/politics_screen.gd` |
| Exército, guerra, tecnologia, fábricas, dinheiro por nação | PLANEJADO | declarados fora do escopo em §1 | §1 |
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
  - `initialize()` / `tick()` / `get_status()` — **legacy**, 5 cidades, só trigo (referência/benchmark; não é o que o jogo roda).
  - `initialize_demo()` / `tick_demo()` / `get_demo_status()` + `demo_invest/build/spend_government/price_shock/to_dict/from_dict` — **demo**, 2 cidades, ciclo completo.
  - `initialize_demo_world(cities, ports, buildings)` / `demo_trade_wheat()` — **demo no mundo real, o contrato único do jogo ao vivo** (1.1); comércio genérico de trigo com subsídio de subsistência e alocação justa; testado por `tools/economy_world_test.gd`.
  - `initialize_benchmark()` / `run_benchmark()` — sintético, determinístico.
- **Sistema de ticks:** o autoload `Economy` acumula a **fração do dia** em `_process` (não usa `Timer`, para trocar de velocidade sem reiniciar o dia). `SPEEDS=[0.0, 60.0, 30.0, 15.0]` segundos reais por dia (0 = pausa; normal = 30 dias em 30 min). Cada `_run_day()` chama `sim.tick()` e emite `status_updated(summary)` e `trade_orders_ready(orders)`.
- **Eventos/sinais (fluxo unidirecional):** `Economy.status_updated` → HUD (`hud.gd::_on_status`) e `main_game.gd::_on_status_updated` (refresh dos modos). `Economy.trade_orders_ready` → `trade_convoys.gd::spawn_orders`. `HUD.city_focused`/`map_mode_requested` → `main_game.gd`. `I18n.locale_changed` → `hud.gd`. A UI nunca executa lógica econômica.
- **Persistência:** a economia demo serializa em memória (`demo_to_dict`/`demo_from_dict`, versão 1); não há gravação em disco da partida. O layout urbano salva em `user://` (`urban_persistence.gd`, versão 1).
- **Separação simulação × apresentação:** C++ calcula; GDScript orquestra (autoload, geografia, decisão de investimento, visualização). A UI recebe texto/valores já formatados.

---

## 4. Mapa e geografia

- **Dois sistemas de mapa coexistem:**
  1. **Jogo principal — `map/city_world.gd`:** duas bases visuais alternáveis por modo:
     - **Modo "political" (atlas):** uma passada opaca em `atlas_political.gdshader` sobre **campos de distância com sinal** gerados de vetores Natural Earth 10m por `tools/build_atlas_layers.py` (`map/map_data/atlas/`): `region_sdf/region_color` (Europa lon −25..45, lat 33..72, 4096 px, ~1,9 km/texel) + `world_sdf` (mundo, 8192 px, ~4,9 km/texel) + `world_color` (4096 px, cor chapada por país). Canais SDF: R costa, G fronteira de país, B divisa admin-1 (1.862 províncias reais na região). Linhas em pixels de tela medidas ao longo do gradiente (espessura igual em qualquer orientação), anti-aliasing, salto do eixo medial descartado pelo gradiente. Terra = cor da nação × luminância do papel (`paper_flat.png`) × grão de papel; faixa escura junto às fronteiras; mar com faixa rasa e linhas de nível; vinheta polar fundindo com o fundo do ambiente. Nações não jogáveis em tons de pergaminho por `MAPCOLOR9`. Nomes das 5 nações gravados no chão (Cinzel, i18n, âncora no polo de inacessibilidade, `nations.json`). O overlay político fica oculto neste modo. Rótulos de cidade em tom claro com halo escuro e `no_depth_test`.
     - **Território dinâmico (mundo inteiro):** a cor e a fronteira de país NÃO são mais pré-desenhadas. Unidade de território = província admin-1 do Natural Earth com **id global** (4.596 no mundo): `region_province.png` (Europa, ~1,9 km/texel) e `world_province.png` (mundo, ~4,9 km/texel) = id por pixel; `provinces.json` = país, nação, cor estática, centroide e `area_global` (px 5000×2500). O shader roda o bloco dinâmico nas duas camadas (`dyn_block`) e escolhe pela região; divisas finas de província só na Europa. Os samplers de região e mundo têm o mesmo `repeat` (exigência do Godot para sampler como argumento de função). `map/territory_map.gd` (`TerritoryMap`) atribui cada província à **cidade mais próxima da mesma nação original**; o **dono = `city["nation"]` do status do Economy** (fonte única, atualizado em `CityWorld.refresh_modes`). Uma textura 64×72 (província → cor + índice do dono) é atualizada em runtime; o shader desenha fronteira grossa onde os donos das duas províncias vizinhas diferem (canal B do SDF), divisa fina onde são iguais, e o preenchimento segue o sinal sub-texel do campo. Hoje GBR, DEU, NED e BEL têm 1 cidade (todo o território); a FRA tem Paris e **Caiena**, que segura a Guiana Francesa e, por ser a cidade francesa mais próxima, também Martinica e Guadalupe. A câmera inicial centra na **mediana** das cidades (`CityWorld.framing_center_px`), para a cidade isolada não puxar o enquadramento para o Atlântico. Rótulo da nação: posição original se o território é o original; senão recalculado sobre as províncias possuídas; some se a nação perde tudo. Debug: `CityWorld.debug_set_city_nation(id, tag)` / `debug_clear_territory_overrides()`, `map_capture -- --territory=BEL-BRU:FRA` e `--focus=FRA-CAY` (evidência: `docs/map_visual/guiana_cayenne/` e `guiana_cayenne_ned/`). Nenhuma regra faz uma cidade mudar de dono (guerra/captura fora do escopo).
     - **Demais modos:** base cartográfica branca (terra marfim, mar branco, relevo suave e contorno dos continentes) via `paper_map.gdshader`, desenhada a partir dos SDFs do Natural Earth — sem imagem de satélite —, com overlay político sutil (`political_overlay.gdshader`, composição em alfa pré-multiplicado).
     - **Camadas do mapa:** botões quadrados acima do minimapa com **político** e **recursos** (os modos população/economia/indústria continuam em `map_modes.gdshader` + `map_mode_layer.gd`, sem botão por enquanto). O mapa de recursos desenha **billboards nítidos** a partir de `map/map_data/atlas/resource_points.json` (gerado por `tools/build_resource_map.py`): círculos coloridos por depósito (carvão, cobre, gás, ferro, petróleo) com **tamanho uniforme** (decisão de design: reserva/aptidão aparecem só na legenda) e losangos de trigo. **Nada de recurso terrestre no mar**: carvão/cobre/ferro só em terra; petróleo/gás podem ser offshore até ~320 km da costa (20 células do `nav_mask`), o que cobre o pré-sal brasileiro e o Mar do Norte sem pontos no meio do oceano — a extração futura será uma regra de jogo (só na costa). O `resource_points.json` leva também `province_id` e `offshore` por depósito; o acesso da cidade usa isso (não mais `GeographyService.deposits_near`). Campos reais que faltavam no extrato MRDS entram por `data/definitions/natural/deposits_supplement.json` + `tools/apply_deposit_supplement.py`: pré-sal e Campos (BRA), Guianas (Stabroek/Block 58) e margem equatorial brasileira, Mar do Norte (NOR/GBR), Oriente Médio, África, Rússia/Ásia Central, Ásia-Pacífico e Golfo do México/Orinoco. O supplement também **ajusta a reserva restante** de campos maduros (Brent, Forties, Groningen, Statfjord, Oseberg), para a Europa não pesar como se ainda tivesse a reserva original. Rode o supplement depois de regenerar o `deposits.json`. No modo recursos o acesso é por **território**: cada depósito **onshore** pertence à província onde está e à cidade dona dela (`TerritoryMap.city_for_province`), sem raio e respeitando a fronteira; **offshore** fica decorativo até a mecânica de extração. Clicar na cidade mostra na legenda os recursos das províncias dela (minérios + trigo do território). A legenda no canto inferior esquerdo é um **filtro**: cada recurso tem caixa de seleção, cor, nome e contagem; abre com todos ligados, permite isolar 1, 2... e o botão "Todos" religa. O mapa reage na hora (`resource_filter_changed` → `CityWorld.set_resource_filter`). **Ficha na legenda:** no modo recursos, passar o mouse num **depósito** mostra o cursor "i" e um **anel de hover com pulso** em volta do ponto; clicar troca a legenda pela ficha do depósito (recurso, campo, reserva estimada, qualidade/teor, profundidade e estágio — cada rótulo tem tooltip explicando); clicar numa **cidade** troca a legenda pelos recursos das **províncias dela** + trigo do território. "Voltar" restaura os filtros.
  2. **Editor — `map/map.gd` + `map/map_mode.gd`:** mapa raster de províncias (SubViewport/Sprite2D sobre plano 3D), com SDF/JFA de fronteiras; usado por `editor/editor.tscn` e `tools/map_capture.gd`, **não** pelo jogo.
- **Projeção/coordenadas:** equirretangular; conversão pixel↔lonlat em `build_natural_geography.py` (`lon=px/5000*360−180`, `lat=90−py/2500*180`). Mundo = plano de 1000×500 unidades (`PIXELS_PER_WORLD_UNIT=5`).
- **Camadas raster (`map/map_data/`):**
  - `provinces.png` 5000×2500 (cor-chave por província, fonte do lookup/SDF).
  - `ne2.png` 5000×2500 (terreno Blue Marble, mapa legado).
  - `height.png`/`normal.png` 5000×2500 — **placeholders planos** (sem relevo real).
  - `paper_flat.png` 512×256 — textura neutra de papel (terreno do modo político); a base branca dos demais modos é desenhada por shader.
  - `atlas/world_color.png`, `atlas/world_sdf.png`, `atlas/region_color.png`, `atlas/region_sdf.png`, `atlas/minimap.png`, `atlas/nations.json` — camadas do mapa atlas geradas por `tools/build_atlas_layers.py` (Natural Earth 10m).
  - `nav_mask.png` 2500×1250 (terra/água/costa, pathfinding A* dos comboios).
- **Províncias/territórios/países:** `data/history/provinces/provinces.json` (14.402 províncias), `data/history/territories/territories.json` (737 territórios), `data/history/countries/` (193 JSON); `data/definitions/*.txt` restringidos a 4 países (NNN/GBR/FRA/DEU). O jogo ao vivo usa **5 nações** (GBR, FRA, DEU, NED, BEL).
- **Fronteiras:** legado = Voronoi + SDF via Jump Flooding Algorithm (`jfa.glsl`/`jfa_encode.glsl` + `map_texture_sdf.gd`); jogo = raster Natural Earth composto por shader.
- **Câmera/navegação (estilo HOI3):** `core/utils/camera_controller.gd` (WASD, zoom, edge scroll 16 px — inativo com o mouse sobre a interface ou fora da janela, arrastar com botão do meio = pan); rotação horizontal desabilitada (mapa fixo norte para cima); pitch limitado −65° a −45°; zoom máximo 500 u; inclinação inicial −56°, distância 52; wrap cilíndrico no eixo X. **Limite elástico em Z** (`main_game.gd::_apply_camera_limit`): centro entre 78°N e 60°S, folga de 24 u e mola de 0,18 s; destinos de `move_to` presos à folha; teste `tools/camera_limit_test.gd`. **Vinheta 2D** sutil (`core/ui/screen_vignette.gdshader`, CanvasLayer 40, abaixo do HUD).
- **Fontes de dados:** Natural Earth 10m, NASA GIBS Blue Marble, Esri World Imagery, USGS MRDS (depósitos), WorldClim 2.1 10-min (clima/altitude).
- **Limitações:** sem relevo real (heightmap/normal planos); `generate_opengs_map.py` com parâmetros divergentes dos dados commitados (8192×, lat −60..84 vs 5000×2500 global); terreno de todas as províncias = PLAINS.
- **Reprodução do dataset (gap conhecido):** a cadeia de geração dos dados naturais é `reconstruct_natural_inputs.py` (atlas `provinces.json` → `provinces.txt`, `data/history/provinces`, `data/history/territories`) → `build_natural_geography.py` (→ `nature.json`, `agriculture.json`, `deposits.json`) → `apply_deposit_supplement.py` → `mark_facilities.py` → `apply_real_climate.py` → `build_resource_map.py` (→ `resource_points.json`). `tools/reconstruct_natural_inputs.py` reconstrói `provinces.txt` + `data/history/` a partir do atlas commitado; `fetch_usgs_deposits.py` baixa o `tools/data/mrds_deposits.jsonl` (gitignored, USGS WFS via r.jina.ai). **Divergência conhecida:** os dados naturais commitados (`nature.json` 737 territórios, `deposits.json` 2.249 depósitos) vieram de um gerador **Voronoi legado** (~14.402 províncias em 8192×, lat −60..84) cujos parâmetros não estão no repo; a reconstrução atual usa as 4.529 províncias admin-1 do atlas e agrupa **um território por nação** (~250) — então uma regeneração produz uma base **diferente** (não byte-idêntica) da commitada. Detalhes em `docs/reproducibility_report.md`.

---

## 5. Cidades e crescimento urbano

- **Cadastro de cidades/portos (data-driven):** cidades vêm de `data/definitions/scenario/cities.csv` → `cities.json` (`id, nation, home_nation, name, population, x, y, landmass, capital`); **portos** vêm de `ports.csv` → `ports.json` (`id, name, nation, x, y, landmass`). `EconomySim.initialize(cities, ports)` recebe os dois; produção = soma das construções de `buildings.csv`. **Território vinculado à cidade**: cada província vai para a cidade mais próxima da mesma `home_nation` (vínculo estável) e o dono = `nation` (atual) da cidade — mudar `nation` muda o dono do território, sem `territory.csv`. Portos são entidades 3D (`port.glb`) com dono próprio (`nation`), separado do território e estratégicos (sem porto, sem mar). As coordenadas em `ports.csv` são **pontos de costa** (mesma borda do SDF usada por Dover/Calais), não o centro da cidade; porto que divide o nome com uma cidade não duplica o rótulo no mapa. Na Guiana, o porto é **Mana** (costa a noroeste), separado de Caiena; o id `FRA-CAY` é mantido para não quebrar rotas/save. Para criar cenário, edite os CSVs e rode `tools/build_scenario_data.py` — sem recompilar.
- **Render 3D (`city_world.gd`):** uma única representação por cidade: a **cidade modular** (`assets/models/city_modules/`, 43 peças) — os modelos compactos antigos (`city/airport/farm/industry/mine/road.glb`) foram removidos. Restam só cidade modular, **veículos** (`truck/ship/plane.glb`) e o **porto novo** (`port.glb`, cais + 2 guindastes de pórtico + armazém + containers + farol). `MAX_DETAILED_CITIES=40`, `MAX_CITY_LABELS=24`; resto vira `MultiMesh` de discos.
- **LOD (sem sobreposição):** próximo (<34 u) módulos 3D detalhados; intermediário (34–100 u) a mesma cidade em volumes simplificados com fade de `visibility_range` (`urban_renderer.gd`); distante (>100 u) marcador discreto com fade 100–112 u. A cidade é construída **uma vez** (≤96 u) e só alterna a visibilidade a 100 u — sem reconstruir no zoom. A biblioteca de módulos é cacheada entre cidades (`BuildingLibrary._shared`), o que evita recarregar os GLBs a cada cidade.
- **Seleção:** `pick_city()` e `pick_port()` consideram a **área do corpo** (footprint na tela, mínimo de 26/20 px) **e o rótulo** (nome/bandeira/selo, pela largura real do texto em tela) — clicar no nome seleciona cidade ou construção. Anel fino **deitado no chão**, dimensionado por `UrbanGrowthManager.extent()`, + ponto central (`select_city()`/`select_port()`); hover (`set_hover()`). O `TorusMesh` já nasce plano — sem o antigo giro de 90°.
- **Marcador de lugar (cidade/porto):** composição horizontal `[bandeira] [selo] [nome]`. O selo é um disco escuro (`#0A1212`, alpha 0.8) com aro de latão (`#C9A24A`) e símbolo em marfim (`#E8E2D0`): **prédios** (`city.png`, MDI `city-variant`) para cidade e **âncora** (`port.png`, MDI `anchor`) para porto. Ícone em 1.0 em do rótulo e selo 1.45×. Os desenhos detalhados antigos (`city.svg`, `port.svg`) e o anel vazado (`ring.png`) foram removidos porque viravam mancha em ~14 px; o selo escuro garante contraste sobre qualquer terreno. Créditos em `assets/icons/places/CREDITS.txt`; evidência em `docs/map_visual/markers_after/`.
- **Cursor:** `core/ui/game_cursor.gd` instala cursores próprios (32×32, latão #C9A24A com contorno escuro, estilo gravado): `cursor_default.png` (seta de mapa, shape ARROW), `cursor_loupe.png` (lupa, shape HELP), `cursor_info.png` (**"i"**, shape HELP — usado no hover de cidade nos mapas de dados), `cursor_pointer.png` (mão apontando o dedo — ícone Phosphor `hand-pointing-fill` recolorido no latão — shape POINTING_HAND) e `cursor_drag.png` (arrastar). O tipo de alvo distingue a ação: **cidade no mapa político → lupa**; **cidade ou depósito nos mapas de dados → "i"**; **porto/construção → mão** (pegar/selecionar); arrasto tem prioridade. Geradores em `tools/build_cursor_icons.py`, `generate_info_cursor.py` e `generate_loupe_cursor.py`; créditos em `assets/ui/CREDITS.txt`. O hover roda por frame em `main_game._update_city_hover_pass()`, passa `"city"`/`"port"`/`""` para `GameCursor.set_state()` e ignora a UI (`gui_get_hovered_control`).
- **Escala:** cidade em `URBAN_SCALE=0.165` (as cidades ficam em posições geográficas reais; Bruxelas–Amsterdã dista ~4,5 u, então o diâmetro precisa ficar bem abaixo disso); veículos de comércio com 0,22 u de comprimento (`trade_convoys.gd`).
- **Tempo:** 1 dia = 60 s no ritmo normal (30 dias em 30 min); 2x = 30 s; 3x = 15 s (`core/economy/economy.gd`). A duração visual dos trechos acompanha o tempo de trânsito simulado (`transit_hours`), então os veículos não "teleportam". O teto fixo de 45 s vale para trechos curtos; travessias longas usam `MAX_VISUAL_SPEED_BY_MODE` (caminhão 1,2, navio 1,0, avião 3,0 u/s) para o navio não cruzar o Atlântico em 45 s.
- **Crescimento procedural (`core/urban/`):** `UrbanGrowthManager` (estágio por população+industrialização) → `DistrictPlanner` (aglomerado orgânico estilo Victoria 3: espiral de ângulo áureo, densa no centro, contorno em lóbulos, alongamento por cidade, vazios na borda, **sem ruas nem grade**; 26/72/150 lotes por estágio) → `UrbanRenderer`. Solo urbano com borda orgânica (shader no `UrbanGrowthManager`). A posição de cada lote depende só de (seed, índice): crescer nunca move o que já existe. **Filtro de espaço construível** (`city_world._make_urban_filter`): cada lote só é aceito se estiver em **terra firme** (SDF da costa, `TerritoryMap.land_at_uv`), **na província da própria nação** (`owner_at_uv` == `nation`) e **fora do raio de porto** (`URBAN_PORT_KEEPOUT=0.4` u). O planejador gera **3× candidatos** (`CANDIDATE_FACTOR`) e o manager aceita os primeiros N até o alvo do estágio, então lotes barrados (água, porto, fora da nação) **não encolhem a cidade**: o crescimento se redistribui para as direções livres.
- **Distrito industrial:** aglomerado orgânico na borda da cidade, em direção fixa por cidade (`industrial_planner.gd`), com tons mais escuros.
- **Arquitetura regional:** `DistrictPlanner.CITY_STYLES` dá a cada cidade matiz, tendência de verticalização e alongamento próprios (Londres tijolo, Paris calcário e prédios médios, Berlim pedra cinza, Amsterdã casas baixas, Bruxelas tijolo). Mesmos módulos GLB; a variação é de cor e mix. `CITY_BIAS` em `industry_profile.gd` redistribui setores.
- **Evidência visual:** `docs/map_visual/city_organic_v4/` (4 zooms).
- **Integração econômica:** população/industrialização decidem o estágio; renda puxa torres; empregos sustentam escritórios; industrialização libera a zona industrial. Indicadores vêm do `EconomySim`.
- **Persistência:** `urban_persistence.gd` (JSON em `user://`, hash de determinismo); usado pelo protótipo `game/city_prototype.tscn`.

---

## 6. Economia

> **Contrato único (1.1, feito):** o jogo ao vivo usa o modelo **demo** (`initialize_demo_world` + `tick_demo` + `get_demo_status`), com comércio genérico de trigo e a camada de IA ligada. O **legacy** (`initialize`/`tick`/`get_status`, trigo por cidade) continua no núcleo apenas como referência de benchmark — não é mais o que o jogo roda. O demo emite `ports`, `nations` e `trade_orders` (com leg em linha reta) para a camada visual, além dos campos de compatibilidade no dict de cidade (`deficit`, `consumed`, `in_transit`, `production`, `jobs`, `income`, `industrialization`, `stockpile`, `need`).
>
> **Mecânicas adicionadas sobre o demo (nesta onda):**
> - **Segurança alimentar**: subsídio de subsistência (governo garante a cesta mínima) + financiamento público de importação, para cidade sem produção não passar fome.
> - **Fiscal nacional (1.8)**: equalização per capita do orçamento público dentro de cada nação (Paris sustenta Cayenne).
> - **Falência (1.10)**: construção fecha após 60 dias de prejuízo (`kDemoLossDaysBeforeClose`).
> - **Capacidade de transporte (1.4)**: teto de carga diário por cidade; frete marítimo (0,02/t-km) entre massas de terra.
> - **Bens data-driven (1.2)**: preços-base via `data/definitions/economy/goods.json` + `demo_set_goods`.
> - **Invariantes em runtime (1.13)**: conservação de dinheiro/estoque/preço com snapshot/revert; também cobre o drift de `double` (1.11 fica deferido).

### 6.1 Modo ao vivo (legacy — trigo)

- **Produção:** constante por cidade (`production`, t/dia), sem cadeia/insumos.
- **Demanda/consumo:** `need = population × 0.0003` t/dia; `consumed = min(stockpile, need)`; `deficit = need − consumed`.
- **Estoque:** `stockpile` acumula produção e descargas.
- **Comércio:** mercado em 3 camadas — regional (<500 km, mesmo país), nacional, internacional; `dispatch_trade` escolhe o vendedor mais próximo e `build_route` monta a rota; ponte aérea quando estoque < 2 dias. **Travessia marítima** quando vendedor e comprador estão em massas de terra diferentes (`landmass`): caminhão até o **porto da nação do vendedor** (`nearest_port`) → navio → caminhão do porto da nação do comprador. Sem porto da nação, não há travessia (porto estratégico). Avião e caminhão saem direto da cidade. **Os veículos acompanham o relógio**: cada tween nasce com `set_speed_scale` do ritmo atual, e `speed_changed` acelera/desacelera todos os tweens em andamento; a pausa (índice 0) congela os veículos até o play (`TradeConvoys.set_time_scale`).
- **Cobertura por cidade:** cada cidade tenta manter `kCoverDays` (10 dias) de cobertura — estoque + em trânsito — em vez de pedir só o déficit diário. **Corrigido:** antes Caiena (a ~9 dias de navio) pedia só o déficit diário e recebia ~1/9 da necessidade, ficando em déficit crônico; agora mantém o pipeline de abastecimento e cobre a necessidade (validado em headless: déficit 0 após o aquecimento).
- **Transporte/frete:** por trecho (`make_leg`), custo = carga × distância × `cost_per_ton_km`; tempo = distância/velocidade + **turnaround** (carga/descarga) por modo. Modos: caminhão 65 km/h/1200 t/R$0,10/+6 h; navio 35 km/h/12000 t/R$0,02/+24 h; avião 750 km/h/400 t/R$0,80/+24 h. O turnaround faz a ponte aérea transatlântica levar ~2 dias (antes chegava no dia seguinte).
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

- **Aptidão agrícola:** `data/definitions/natural/agriculture.json` (9 culturas: trigo, arroz, milho, batata, soja, cana, café, fruta, **algodão**), `data_quality:"real_climate"`; **6 culturas (trigo, arroz, milho, batata, soja, algodão) + madeira** aplicadas ao sim (`GeographyService.apply_to_sim`); madeira vem do `forest_cover` (`nature.json`), não da aptidão agrícola. Aptidão recalculada com clima real (WorldClim 2.1 10-min) por `tools/apply_real_climate.py`, usando **temperatura de estação de cultivo** (`bio10` trimestre mais quente para soja/milho/algodão/arroz/cana/café/fruta; `bio11` trimestre mais frio para trigo/batata quando o clima anual passa de 22 °C — cultivo de inverno em trópicos/subtrópicos), não a média anual. Penalidade de solo suavizada (`0,6 + 0,4·s`) para refletir agricultura gerida (calagem/adubação); precipitação anual com faixa alargada nas tropicais (soja/milho/algodão/arroz).
- **Depósitos minerais:** `deposits.json` (≈5.727 registros após reclassificação + suplementos + re-derivação MRDS global de ouro/chumbo/bauxita/fosfato/tungstênio/terras raras/enxofre/diamante/urânio via `tools/add_mrds_minerals.py`; fonte `tools/data/mrds_deposits.jsonl` com 304.632 registros); **14 minerais**: carvão, ferro, cobre, petróleo, gás, bauxita, chumbo, ouro, fosfato, tungstênio, terras raras, enxofre, diamante, urânio. Campos: `resource`, `px/py/lon/lat`, `quantity_kt`, `recoverable_kt`, `quality`, `depth_m`, `extraction_cost`, `max_rate_kt_day`, etc. Registros `"facility"` (fundições/refinarias) são ignorados.
- **Sem prospecção:** todo depósito é conhecido desde a inicialização (`apply_to_sim` declara aptidão + reserva de cada cidade via `demo_set_deposit`); `ResourceKnowledge` foi removido.
- **Extração/esgotamento:** implementados no núcleo — a mina consome a reserva (`demo_set_deposit`, reserva compartilhada pelas minas da cidade) e a eficiência cai com a qualidade (`0,5 + 0,5·q`); profundidade encarece o opex. Nível de esgotamento (Intacta → Abundante → Moderada → Escassa → Residual) exibido na ficha.
- **Mapa de recursos agregado:** 1 marcador por **(território × recurso)** — minerais/renováveis iguais se fundem (a mina rende por todas), recursos diferentes ficam separados por um deslocamento (`MINERAL_OFFSET`/`RENEWABLE_OFFSET`); clicar abre a ficha de **um recurso** (reserva/qualidade/profundidade/situação para mineral; aptidão/fertilidade/cultivável para renovável). ~2.838 marcadores minerais + ~1.550 renováveis.
- `nature.json`: geofísica por território (clima, solo, relevo, fertilidade, água, floresta), `data_quality:"real_climate"`; fertilidade/cultivável entram na ficha agrícola.

---

## 8. Indústria

- **Produtos:** 23 bens no núcleo (14 minerais + 7 renováveis + farinha/pão); a cadeia viva hoje só modela trigo→farinha→pão + ferro — os demais minerais/renováveis são **matérias-primas** mineráveis/trocáveis, sem refino (aço/munição/fertilizante ficam para depois).
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
  - `GovernmentAI` — estatal de segurança alimentar, mina de ferro se o território tem ferro (limite 25% do orçamento; sem prospecção).
  - `IndustryTemplates` — parâmetros de construção (duplicam o C++).
- **Políticas econômicas:** limitadas a regras fixas (impostos/transferências); não há interface de política ativa.

---

## 11. Interface e UX

- **Design system:** `core/ui/ui_kit.gd` (`UiKit`) — tokens (BG_DEEP `#0A1112`, PANEL, latão `#C9A24A`, etc.), tipografia (Cinzel para títulos, Barlow Condensed para corpo), escala 26/19/15/15/13/11 px, tema global, ~30 construtores (`metric`, `status_pill`, `table_row`, `empty_state`, `toast`, etc.).
- **HUD (`core/ui/hud.gd`):** barra superior de 48 px (estilo HOI3: mais compacto); doca direita (5 botões, atalhos 1–5); **minimapa** em folha de papel "Papelaria de guerra" no canto inferior direito (`core/ui/minimap.gd`), com abas verticais de tipo de mapa (político/recursos) coladas à esquerda: **dinâmico** — renderiza em runtime a cor do dono por província (`TerritoryMap.minimap_image()`, 512×256, amostrando `world_province.png` + `world_sdf.png`; fallback = `minimap.png` estático), atualizado em `main_game._on_territory_changed`; retângulo `CARIMBO` com a área vista (atualizado por `main_game._update_minimap`), clique/arraste leva a câmera ao ponto (`HUD.map_point_requested`); janela modal responsiva (86%×84%, 960×560 a 1800×1000); menu de pausa (Esc); **espaço** pausa/retoma; **F11** alterna tela cheia. Tooltips com borda latão e sombra (estilo HOI3).
- **Telas (`core/ui/screens/`):** Economia, Comércio (Pedidos/Rotas/Mercado), Produção, População, Política (Nações/Diplomacia/Dossiê), Alertas.
- **Painel de cidade:** `core/ui/city_panel.gd` (5 abas). `core/ui/city_detail.gd` é **legado**; `core/ui/city_economy_panel.gd` serve ao cenário demo.
- **Navegação:** pilha de contexto com breadcrumb; foco por teclado (anel de latão); Esc fecha menu → janela → abre menu.
- **Estados vazios:** princípio "estado vazio honesto" — onde não há dado (produção, política, preços, desemprego), a tela declara a ausência.
- **i18n:** `core/i18n/i18n.gd` — ~330 chaves × {pt-BR, en, es}; troca em runtime; lacunas nos protótipos (texto fixo em pt-BR em `city_economy_panel.gd`, `game/*_prototype.gd`).
- **Áudio:** `Music` (7 faixas) + `UiSfx`. Tema ativo **`command_post`**: 42 WAV sintetizados (14 eventos × 3 variações) em `assets/audio/interface_command_post/`, secos/mecânicos (clique, interruptor, papel, carimbo, rádio, typewriter), gerados por `tools/generate_command_post_sfx.py`. O pacote Kenney CC0 original fica em `assets/audio/interface/` como fallback — trocar `THEME` em `core/audio/ui_sfx.gd` entre `"command_post"` e `"kenney"`. Rolagem e arraste usam `play_throttled` (90 ms / 55 ms por controle).
- **Ícones de produto:** `core/ui/product_icons.gd` + `assets/icons/products/*.svg` — 13 SVGs monocromáticos de gravura (trigo, farinha, pão, ferro, fábrica, caixa, moeda, óleo, espada, rifle/AK-47, caminhão, navio, avião) do **game-icons.net** (CC BY 3.0), baixados e recoloridos para branco por `tools/fetch_game_icons.py` (API Iconify); atribuição em `assets/icons/products/CREDITS.txt`. Ainda não ligados às telas; `ProductIcons.make(id, cor)` cria o `TextureRect` tingível (silhueta branca × `modulate`). Para trocar/renovar um ícone, edite o mapa `ICONS` em `tools/fetch_game_icons.py` e rode o script.

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
| D-004 | UI é "estado puro": recebe texto/valores formatados, nunca calcula | §11 / `DESIGN.md` |
| D-005 | Fronteiras do jogo = Natural Earth raster; SDF/JFA fica no editor | Duas representações de mapa coexistindo |
| D-006 | Estados vazios honestos (não inventar números) | princípio de produto 2 (§1) |
| D-007 | i18n pt-BR/en/es em runtime, default pt-BR | compromisso de marca (§1) |
| D-008 | Orçamento público equilibrado (imposto 15% = transferências) | Demo; simplificação declarada |
| D-009 | IA econômica determinística (sem LLM), a cada 7 dias | Previsibilidade/testabilidade |
| D-010 | Crescimento urbano determinístico (seed por cidade) e lotes nunca removidos | Identidade dos bairros |
| D-011 | Clique em cidade sem física (projeção de viewport) | Simplicidade |
| D-012 | Modo político usa base atlas (chapado); demais modos usam satélite | Leitura clara de fronteiras no modo principal; satélite preservado para contexto físico |
| D-013 | Câmera estilo HOI3: rotação desabilitada, edge scroll, drag pan, pitch limitado | Mapa fixo norte para cima; navegação intuitiva para jogadores de grand strategy |
| D-014 | Minimapa no canto inferior direito; mundo flat com vinheta de latitude | Orientação global sem custo de globo esférico; vinheta esconde bordas do mundo |
| D-015 | Rótulos padronizados; capital (coluna `capital` do CSV, lida direto do `cities.json` porque o status do núcleo não a carrega) fica um pouco maior e usa estrela no selo; fade por distância | Capitais mais visíveis sem variar o tamanho pelo tipo de cidade |
| D-018 | Território = províncias admin-1 controladas por cidades; dono = nação da cidade no status do Economy | Fronteiras mudam sozinhas quando uma cidade muda de nação, sem estado paralelo. Fronteira só corre por divisas de província reais. Evidência: `tools/territory_test.gd`, `docs/map_visual/territory_static/` e `territory_bel_fra/` |
| D-017 | Sem diorama/mesa 3D, props, luminária, sombras reais nem globo; o mapa atlas atual é a apresentação | Escopo de projeto pequeno (decisão do usuário). Mantidos só o limite elástico da câmera em Z e uma vinheta 2D de tela; o wrap em X continua |
| D-016 | Mapa político a partir de campos de distância com sinal (vetores Natural Earth), não de máscaras raster | Máscaras de 8–16 km/px ampliadas viravam escadinha; o SDF dá linhas nítidas em qualquer zoom numa passada opaca. Regenerar com `python tools/build_atlas_layers.py` (requer pyshp, scipy) |

---

## 15. Bugs e dívida técnica

- **BOM `U+FEFF`** em `core/ui/city_economy_panel.gd` (linha 1) — quebra `tools/economy_panel_test.gd` (único arquivo com BOM).
- **Pick de depósito (mapa recursos) — investigado:** medição `tools/deposit_pick_test.gd` (3 janelas × 2 câmeras) confirma `get_mouse_position()` e `unproject_position()` no mesmo espaço (base) e `pick_deposit` auto-coerente — Ferdinand (1232) e Chaillac (1618) respondem; o raio de 26px está correto. A causa do "sem resposta" era o billboard sem `billboard_keep_scale` (desenhava todo marcador a 1.0 u, escondendo o anel sob o ponto em depósitos pequenos).
- **Duas faces econômicas** (legacy × demo) no mesmo `EconomySim`, consumidas por UIs diferentes; risco de divergência.
- **Três implementações paralelas** de produção/consumo/preços/contabilidade no C++ (tick_demo × benchmark × legacy).
- **Parâmetros duplicados:** `IndustryTemplates` (GDScript) repete `jobs/wage/opex/input_ratio` do C++ sem vínculo; preços-base em `balance.json` × `demo_base_price()`.
- **Mapa legado de províncias/editor removido:** restam dois modos intencionais (político/atlas × geográfico/satélite).
- **Três painéis de cidade:** `city_panel.gd` (ativo), `city_detail.gd` (legado), `city_economy_panel.gd` (demo).
- **Cores de nação divergentes:** `ui_kit.gd`/`build_atlas_layers.py` × `city_world.gd`.
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
- **Corrigido (atlas):** a Holanda nunca era pintada (`NED` no jogo × `NLD` no Natural Earth) — mapeado em `build_atlas_layers.py`; halo branco nas nações vinha da composição não pré-multiplicada em `political_overlay.gdshader`; as cópias laterais do mundo não trocavam de modo (agora `set_mode` age em todas).
- **Corrigido (overlay dos modos de calor):** `political_overlay.gdshader` agora lê os mesmos SDFs/cores do atlas (`region_sdf`/`world_sdf` + `region_color`/`world_color`) em vez dos rasters antigos `country_borders.png`/`nation_fill.png`/`nav_mask.png` — fronteiras e costa nítidas em qualquer zoom, sem a escadinha das máscaras ampliadas. A amostragem/decodificação/linhas comuns foram extraídas para `map/map_shaders/atlas_common.gdshaderinc` (usado por `atlas_political.gdshader` e `political_overlay.gdshader`); os PNGs antigos `country_borders.png`/`nation_fill.png` e o gerador `tools/build_political_layer.py` foram **removidos** (substituídos pelas camadas `atlas/*`); `nav_mask.png` segue em uso por `core/economy/trade_convoys.gd`.
- **Memória das camadas do atlas (corrigida):** `region_color.png`/`world_color.png` agora em compressão VRAM (S3TC, `compress/mode=2`); `region_sdf.png`/`world_sdf.png` seguem lossless (`mode=0`) pela precisão das linhas. Medido por `map_capture --bench` (texture_mem_mb): 353,3 MB antes → 274,6 MB depois (−78,7 MB) com o overlay novo. O "antes" já usa o overlay sobre SDFs (sem `country_borders`/`nation_fill`/`nav_mask` em VRAM), então o delta é só da compressão das cores; o baseline histórico do atlas_final media 389,8 MB por ainda carregar os rasters antigos. Sem perda visível: a comparação político `heat_after` × `atlas_final` mostrou 99,6% dos pixels com Δ≤5/255 (quantização imperceptível); os ~0,03% com Δ grande são deriva de rótulos entre execuções, presente também com cores sem compressão.
- **Território dinâmico — limites conhecidos:** fora da Europa a fronteira dinâmica usa a camada mundial (~4,9 km/texel após o bump para 8192 px) — mais nítida, mas ainda menos precisa que a Europa (~1,9 km/texel); pequenos pontos podem aparecer em junções triplas no zoom máximo. (O minimapa agora é dinâmico, e o marcador da cidade acompanha a cor do dono ao mudar de nação.) O teste headless verifica a imagem CPU de donos (o renderizador dummy não reflete `ImageTexture.update()`); o efeito na GPU é verificado pelas capturas.
- **Nomes de cidade (corrigido):** o mapa usa `I18n.city_name(city_id, fallback)` e atualiza os rótulos em `I18n.locale_changed` (incluindo as cópias laterais).
- **Anéis de depósito pequenos invisíveis (corrigido):** o `StandardMaterial3D` do MultiMesh usava `billboard_keep_scale=false`, descartando a escala da instância e desenhando todo ponto com 1.0 u — nos depósitos de `size<0.5` (reserva < ~308 mil kt) o anel de hover (2×size) ficava menor/igual ao ponto e sumia sob ele.
- **Tamanho uniforme dos marcadores (decisão de design):** `billboard_keep_scale=true` e `_marker_size()` retorna a constante `MARKER_SIZE=1.0` para minerais E trigo — todos os depósitos têm o MESMO tamanho, e a reserva/aptidão seguem só na ficha da legenda. Hover = 1.8×MARKER_SIZE e seleção = 2.8×MARKER_SIZE (constantes, sempre maiores que o ponto) + piso em tela ≥22 px (hover)/30 px (seleção) aplicado por frame em `_process`; o raio de pick é uniforme = max(26px, raio projetado de MARKER_SIZE×0.5×1.35). Validado por `tools/deposit_ring_test.gd` (Chaillac 1618 e Ferdinand 1232: ponto, hover, seleção e pick com o mesmo diâmetro projetado ±1 px).
- **Ficha de recurso agrícola clicável (feito):** os pontos de trigo entram no pick com índice codificado (minerais `0..M-1`, trigo `M..M+W-1`) e ganham hover/seleção como os minerais; `deposit_info` devolve `kind:"wheat"` com **aptidão** (do ponto), **fertilidade** e **cultivável** (de `GeographyService.territory_attributes`, que resolve o território pelo centróide mais próximo) e o território. O card reusa o layout da ficha de mina (`hud._show_agriculture_info`) com o selo **RENOVÁVEL** e a dica "melhora com maquinário e construções na cidade"; sem reserva/qualidade/profundidade, porque a agricultura é fluxo anual, não estoque finito. Dados `data_quality:"test"`. Validado por `tools/wheat_info_test.gd` (180 pontos: aptidão/fertilidade/cultivável em 0..1, pick no centro, mineral não regrediu).
- **Auditoria do dataset — P1 (feito):** três correções nos dados do mapa de recursos. (1) **Fundições/refinarias não são depósitos**: `tools/mark_facilities.py` marca `"facility": true` em 48 registros do `deposits.json` (identificação pelo **nome** — `smelter`/`refinery`/`facility`/`concentrator`/`works`/`metallurgical`/`processing`/`foundry` + 7 nomes curados: Rönnskär, Avonmouth ISF, Budel, Crotone, Topionice, Iwami, Oskaloosa — porque o `status:"Plant"` do MRDS mistura fundições com minas reais como Batu Hijau/Boddington); o `build_resource_map.py` e o `GeographyService._index_deposits` pulam `facility` (mapa, legenda da cidade e prospecção da IA). Os 6 visíveis nas nações jogáveis (Hamburg, Duisberg, Luenen, Darley Dale, Olen, Le Palais) sumiram. (2) **Offshore via `nav_mask`** em vez do SDF (que tinha artefato de borda): 29 depósitos de terra deixaram de ser marcados offshore e 1 falso negativo (gás Rovuma) foi corrigido. (3) **Trigo não flutua mais no mar**: pontos com centróide em água encostam na terra mais próxima até 40 células do `nav_mask` (3 casos) ou são descartados (4 ilhas remotas). Resultado: `resource_points.json` 2186→2138 minerais e 180→176 trigo. Validado por `deposit_pick_test`, `deposit_ring_test`, `wheat_info_test` e `economy_world_test` (todos exit 0).
- **Auditoria do dataset — P2 (feito):** (1) **IDs de depósito únicos**: `apply_deposit_supplement.py` agora continua a numeração a partir do maior sufixo existente e renumeriza duplicados (`_normalize_ids`); corrigidos 16 IDs compartilhados por 49 registros (curated oil/gas). (2) **Caiena (`FRA-CAY`)** mantida como colônia-porto importadora (produção 0, sem construção — intencional). (3) **Reproducibilidade documentada** no GAME_STATE: pipeline completo e os inputs ausentes (`tools/data/mrds_deposits.jsonl`, `data/history/`, `data/definitions/provinces.txt`).
- **Auditoria do dataset — P3 (feito): clima real no lugar de latitude sintética.** `tools/apply_real_climate.py` amostra WorldClim 2.1 (10-min: `elev`/`bio1`/`bio12`) no centróide de cada território e re-deriva clima/solo/relevo/fertilidade/água/floresta/cultivável + aptidão das 8 culturas com as fórmulas re-alimentadas (reusando `build_natural_geography.py`). Efeito: Holanda −1..0 m, Bangladesh 12 m, Amazônia 119–689 m/26 °C (antes 2.793 m/7,9 °C), Himalaia 5.440 m/−3,9 °C; trigo Paris 0,95, Londres 0,65, Berlim 0,57, Amsterdam 0,70, Bruxelas 0,59 e **0,00 nas Guianas/Caracas** (tropical). 702 territórios passam a `data_quality:"real_climate"`; 35 com centróide em água mantêm valores sintéticos. `resource_points.json`: trigo 176→117 (recorte realista); minerais inalterados. Downloads via `tools/fetch_worldclim.py` (cache gitignored); precisa `tifffile`+`imagecodecs`. Validado por `economy_world_test`, `economy_ai_test`, `wheat_info_test`, `population_demo_test`, `deposit_pick_test` e `deposit_ring_test` (todos exit 0). Limitação conhecida: o rótulo `climate` continua por latitude (não Köppen), então alta-montanha tropical pode exibir `tropical_humid` — seria o P3b.
- **Ficha agrícola — Clima/Solo/Relevo (feito):** a aridez deixou de vir das caixas `ARIDITY` desenhadas à mão e passou para o **índice de De Martonne** (publicado, `I = P/(T+10)`, calculado sobre temp/precip reais) — novas funções `zone_for_latitude`/`aridity_from_climate`/`climate_label` em `build_natural_geography.py`, usadas por `apply_real_climate.py`. A ficha agrícola ganhou uma linha amigável **"Clima: temperado úmido · Solo: fértil · Relevo: planície"** (`hud._friendly_terrain`), composta de 25 chaves i18n (pt/en/es) para zona/aridez/solo/relevo com termos de exibição (ex.: `luvisol`→"fértil", `arenosol`→"arenoso"), sem expor Köppen/FAO. `resource_points.json`: trigo 117→116. Validado por `wheat_card_test.gd` (novo; monta a ficha em runtime) + `wheat_info_test`, `economy_world_test`, parse/game (exit 0).
- **Expansão de recursos (Fases 0–4, feito):** o set saiu de 5 minerais + trigo para **14 minerais + 7 renováveis** (23 bens no núcleo C++). **Fase 0** — classificador por código primário (`resource_for_codes`), reclassificação (~568 depósitos), mapa agregado (1 marcador/território), nomes regionais do Natural Earth (sem hífen), situação da mina (5 níveis de esgotamento), suplementos de carvão/diamante. **Fase 1** — arroz + milho. **Fase 2** — bauxita/chumbo/ouro/fosfato/tungstênio/terras raras/enxofre/diamante + extração com esgotamento de reserva (`demo_set_deposit`). **Fase 3** — batata/soja/algodão (FAO EcoCrop)/madeira (`forest_cover`). **Fase 4** — urânio (8 campos curados). Prospecção (`ResourceKnowledge`) removida: tudo conhecido desde a inicialização. Renováveis agregados por território (1 losango do dominante + ficha com mix). Testes: `economy_world/ai/integration`, `population_demo`, `wheat_info/card`, `deposit_ring` (exit 0). Produtos/subprodutos refinados (aço, munição, fertilizante, etc.) ficam para um plano futuro.

- **Balanceamento global de culturas + acesso por nação (feito):** a aptidão passou a usar **temperatura de estação** (WorldClim `bio10`/`bio11`) em vez de média anual, corrigindo distorções estruturais — EUA cinturão do milho (milho/soja 0,29/0,13 → 0,96), Brasil (soja 0,66 → 0,87, algodão 0,57 → 0,85), Argentina (soja 0,57 → 0,99), Índia trigo (0,00 → 0,78), China/Indonésia arroz. Minerais sub-representados re-derivados do MRDS globalmente (`tools/add_mrds_minerals.py`, +3.447 → 5.727 depósitos). **Acesso de recurso por nação** (`GeographyService.deposits_for_nation`/`crop_suitability_nation`/`forest_cover_nation`, mapeando tag → ISO-3 com `NED→NLD`): a capital agrega todos os territórios do país, não só o mais próximo — Venezuela volta a ter petróleo (Maracaibo), Holanda gás (Groningen), Alemanha carvão (Ruhr). Mapa agregado por **(território × recurso)**, iguais fundidos. Terras raras da China suplementadas (Bayan Obo etc.). Testes: `economy_world/ai/integration`, `population_demo`, `wheat_info/card`, `deposit_ring`, `camera_limit`, `territory` + parse/game (exit 0). O mundo inteiro fica povoado — pronto para nações futuras, sem povoar o mapa depois.

---

## 16. Roadmap

**Implementado:** núcleo econômico (legacy + demo), HUD + 6 telas, design system, i18n, mapa de satélite 3D, crescimento urbano/industrial, comboios, IA econômica, editor de províncias, áudio, testes; **14 minerais + 7 renováveis** com extração/esgotamento (sem prospecção) e mapa de recursos agregado.

**Em desenvolvimento / parcial:** integração do modo demo ao loop principal; save/load de partida; cadeias de refino (aço, munição, fertilizante, alumínio, eletrônica) — só matérias-primas hoje.

**Próximas prioridades (recomendadas pela auditoria):**
1. Corrigir o BOM de `core/ui/city_economy_panel.gd`.
2. Decidir a consolidação dos dois sistemas de mapa e dos três painéis de cidade.
3. Unificar os dois contratos econômicos (promover o demo ao jogo ao vivo) ou documentar a coexistência.
4. Sincronizar `IndustryTemplates` com o C++ (fonte única de parâmetros).
5. Implementar save/load de partida.
6. Substituir `nature.json`/`agriculture.json` por dados reais (WorldClim/FAO/SRTM) — já referenciado em `_sources.json`.
7. Gerar relevo real (heightmap/normal) para o mapa.

**Planejado (sem código):** exército, guerra, tecnologia, política interna, construção de fábricas, dinheiro por nação (declarados fora do escopo em §1).

---

## 17. Guia para agentes

### Arquivos essenciais
- `GAME_STATE.md` (este), `DESIGN.md`, `AGENTS.md` e `docs/` (evidências).
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
& $godot --headless --path . --script res://tools/economy_world_test.gd         # rápido (demo no mundo real)
& $godot --headless --path . --script res://tools/economy_long_run_test.gd      # lento
# Laboratório de balanceamento (8 cenários × 5 horizontes):
& $godot --headless --path . --script res://tools/economy_lab.gd -- --tag=final
& "C:\Users\artur\AppData\Local\Python\bin\python3.exe" tools/economy_report.py --tag final
# Capturas:
& $godot --path . res://tools/ui_capture.tscn -- --phase=f4 --run=final --sizes=1920x1080,1600x900,1366x768,2560x1440
& $godot --path . res://tools/map_capture.tscn -- --set=all --mode=political --tag=before --run=baseline_before
```

### Indexação do C++ (clangd)
```powershell
# Só reconfigura (não compila); gera gdextension\build-{debug,release}\compile_commands.json (~1091 entradas):
& "G:\tools\cpp-tools\Scripts\cmake.exe" -S gdextension -B gdextension\build-debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```
`-S gdextension` é obrigatório (o `CMakeLists.txt` fica em `gdextension/`). O clangd não procura o DB
em subpastas de build, por isso `gdextension/.clangd` aponta `CompilationDatabase: build-debug`.

### Ponte Claude Desktop → OpenCode (MCP local)

`tools/claude_bridge/` expõe o OpenCode como MCP local para o Claude Desktop:
o Claude decide e delega, o DeepSeek executa no repositório com agentes, testes
e MCPs de cena. Config do app fica no caminho virtualizado do MSIX
(`%LOCALAPPDATA%\Packages\Claude_pzs8sxrjxfjjc\LocalCache\Roaming\Claude\claude_desktop_config.json`).
Validação: `node tools/claude_bridge/selftest.mjs [--task]`. Detalhes em
`tools/claude_bridge/README.md`.

### Como atualizar este documento
1. Consulte o `GAME_STATE.md` antes de mudanças importantes.
2. Ao concluir uma implementação que altere sistema/mecânica/dados/arquitetura, atualize **apenas as seções afetadas** e o status na tabela §2.
3. Registre decisões novas em §14 e problemas em §15.
4. Não marque como IMPLEMENTADO sem evidência (teste/captura); use NÃO VERIFICADO quando não executou.
5. Mantenha o documento legível; detalhes extensos ficam em `docs/`.
