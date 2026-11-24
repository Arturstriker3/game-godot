# Inventario de testes headless — OpenGS Game

Fonte: `G:/opengs_tmp/opengs_test_run_20261124_052239/summary.json` (bateria de 24/11/2026 05:22, 102 testes, todos OK). Duracao de parede medida pelo runner, um Godot por vez.

- Tempo total da bateria: **5928s (98.8 min)**; mediana 23.4s.
- Por tipo: economia+nucleo = 65, cena/UI = 32, sistema GDScript = 3, economia+mapa = 2.
- `modo` = como o runner executa: `script` (SceneTree/MainLoop) ou `scene` (cena `.tscn`).
- `modulos` = diretorios `res://` mais referenciados pelo teste (preloads/loads), nao prova de cobertura de cada linha.
- `sem Godot?` = `nao` para todos os testes atuais: mesmo os de economia pura sao GDScript e sobem o motor. A coluna so muda se o teste migrar para o CLI C++ (ver secao final).

## Top 15 mais lentos

| # | teste | tempo | tipo | o que cobre |
|---:|---|---:|---|---|
| 1 | `c2c_ab_365_test` | 781.3s | economia+nucleo | C2c (D-108) — A/B POR CANAL, 365 d com IA (threads=1), tres mundos em sequencia NO MESMO p |
| 2 | `economy_bistability_test` | 320.1s | economia+nucleo | Teste de regressão da biestabilidade de liquidez (GAME_STATE §15 "fome com alimento em est |
| 3 | `c2b2_fx_stress_test` | 275.3s | economia+nucleo | C2b2 (D-106) — ESTRESSE FORTE DO CÂMBIO: 365 d com reservas mínimas. |
| 4 | `construction_queue_test` | 271.7s | economia+nucleo | §15 unificado — MORADIA, PRÉDIO E INFRA NA MESMA FILA DA CIDADE. |
| 5 | `city_gov_test` | 214.2s | economia+nucleo | Teste do pacote W2 da §19 — prefeitura (docs/economy/s19_urban_spec.md). |
| 6 | `b6_property_test` | 209.8s | economia+nucleo | B6 — PATRIMÔNIO IMOBILIÁRIO DA REGIÃO (D-095): terra produtiva, moradia do interior e anel |
| 7 | `strata_test` | 204.2s | economia+nucleo | B2 — ESTRATOS DE RENDA (quintis) COM PATRIMÔNIO. |
| 8 | `b4_migration_test` | 186.9s | economia+nucleo | B4 — MIGRAÇÃO (êxodo rural → polo urbano e cidade → cidade na mesma nação). |
| 9 | `agriculture_seasonal_test` | 183.7s | economia+nucleo | G — teste headless da AGRICULTURA SAZONAL (D-096). |
| 10 | `agri_calendar_owner_test` | 153.9s | economia+nucleo | G — CALENDÁRIO DE SAFRA HERDADO DA REGIÃO (decisão do dono, D-098). |
| 11 | `c2_central_bank_test` | 148.5s | economia+nucleo | C2a (D-103) — BANCO CENTRAL POR MOEDA: estado + medição. |
| 12 | `b7_informality_test` | 146.6s | economia+nucleo | B7 — INFORMALIDADE, SERVIÇOS INFORMAIS, MORTALIDADE E INSATISFAÇÃO + save v24. |
| 13 | `turbo_equivalence_test` | 125.3s | economia+nucleo | Equivalência do modo TURBO (`core/economy/turbo_config.gd` + `core/economy/tick_scheduler. |
| 14 | `economy_deposit_ownership_test` | 110.1s | economia+mapa | Posse mineral (regra A1): cada jazida pertence a UMA cidade do cenário — a mais próxima da |
| 15 | `housing_market_test` | 102.7s | economia+nucleo | §19 W3 — mercado de moradia. Guarda de aceite do pacote: (1) falta de unidade -> aluguel S |

## Todos os testes

| teste | tempo | modo | tipo | modulos | o que cobre |
|---|---:|---|---|---|---|
| `c2c_ab_365_test` | 781.3s | script | economia+nucleo | data/definitions/economy, core/economy, data/definitions/scenario, data/definitions/population | C2c (D-108) — A/B POR CANAL, 365 d com IA (threads=1), tres mundos em sequencia NO MESMO processo (1 Godot por vez, nada em paralelo). |
| `economy_bistability_test` | 320.1s | script | economia+nucleo | data/definitions/economy, data/definitions/scenario, core/economy, data/definitions/population | Teste de regressão da biestabilidade de liquidez (GAME_STATE §15 "fome com alimento em estoque"). |
| `c2b2_fx_stress_test` | 275.3s | script | economia+nucleo | data/definitions/economy, core/economy, data/definitions/scenario, data/definitions/population | C2b2 (D-106) — ESTRESSE FORTE DO CÂMBIO: 365 d com reservas mínimas. |
| `construction_queue_test` | 271.7s | script | economia+nucleo | data/definitions/economy, data/definitions/scenario, core/economy | §15 unificado — MORADIA, PRÉDIO E INFRA NA MESMA FILA DA CIDADE. |
| `city_gov_test` | 214.2s | script | economia+nucleo | data/definitions/economy, data/definitions/scenario, core/economy | Teste do pacote W2 da §19 — prefeitura (docs/economy/s19_urban_spec.md). |
| `b6_property_test` | 209.8s | script | economia+nucleo | data/definitions/economy, core/economy, data/definitions/scenario, data/definitions/population | B6 — PATRIMÔNIO IMOBILIÁRIO DA REGIÃO (D-095): terra produtiva, moradia do interior e anel periurbano; propriedade por quintil; Gini de riqueza; sa... |
| `strata_test` | 204.2s | script | economia+nucleo | data/definitions/economy, data/definitions/scenario, core/economy, data/definitions/population | B2 — ESTRATOS DE RENDA (quintis) COM PATRIMÔNIO. |
| `b4_migration_test` | 186.9s | script | economia+nucleo | data/definitions/economy, data/definitions/scenario, core/economy, data/definitions/population | B4 — MIGRAÇÃO (êxodo rural → polo urbano e cidade → cidade na mesma nação). |
| `agriculture_seasonal_test` | 183.7s | script | economia+nucleo | data/definitions/economy, core/economy, data/definitions/scenario, data/definitions/agriculture | G — teste headless da AGRICULTURA SAZONAL (D-096). |
| `agri_calendar_owner_test` | 153.9s | script | economia+nucleo | data/definitions/economy, core/economy, data/definitions/scenario, data/definitions/population | G — CALENDÁRIO DE SAFRA HERDADO DA REGIÃO (decisão do dono, D-098). |
| `c2_central_bank_test` | 148.5s | script | economia+nucleo | data/definitions/economy, core/economy, data/definitions/scenario, data/definitions/population | C2a (D-103) — BANCO CENTRAL POR MOEDA: estado + medição. |
| `b7_informality_test` | 146.6s | script | economia+nucleo | data/definitions/economy, data/definitions/scenario, core/economy, data/definitions/population | B7 — INFORMALIDADE, SERVIÇOS INFORMAIS, MORTALIDADE E INSATISFAÇÃO + save v24. |
| `turbo_equivalence_test` | 125.3s | script | economia+nucleo | data/definitions/economy, core/economy, data/definitions/scenario | Equivalência do modo TURBO (`core/economy/turbo_config.gd` + `core/economy/tick_scheduler.gd`) contra o relógio normal do jogo. |
| `economy_deposit_ownership_test` | 110.1s | script | economia+mapa | data/definitions/scenario, core/economy, data/definitions/natural, data/definitions/economy | Posse mineral (regra A1): cada jazida pertence a UMA cidade do cenário — a mais próxima da MESMA nação e da MESMA massa de terra. Verifica: 1. |
| `housing_market_test` | 102.7s | script | economia+nucleo | data/definitions/economy, data/definitions/scenario | §19 W3 — mercado de moradia. Guarda de aceite do pacote: (1) falta de unidade -> aluguel SOBE (passo mensal limitado); (2) sobra de unidade -> alug... |
| `economy_city_catchment_test` | 96.5s | script | economia+mapa | data/definitions/scenario, core/economy, data/definitions/economy, map | Teste da bacia de captação por cidade em PROVÍNCIAS admin-1 (agricultura e madeira pertencem à cidade mais próxima da mesma nação e mesma massa de ... |
| `b3_region_test` | 94.2s | script | economia+nucleo | data/definitions/economy, data/definitions/scenario, core/economy, data/definitions/population | B3 — REGIÃO DA CIDADE (estados inteiros). Teste de dado + estado + save. |
| `population_age_test` | 72.6s | script | economia+nucleo | data/definitions/economy, data/definitions/scenario, core/economy, data/definitions/population | B1 — teste do padrão REAL da demografia por coortes no mundo real. |
| `b5_state_levers_test` | 67.5s | script | economia+nucleo | data/definitions/economy, data/definitions/scenario, core/economy, data/definitions/population | B5 — INFRAESTRUTURA DO ESTADO + CONCESSÕES (F9) + save v21. |
| `services_business_audit_test` | 61.7s | script | economia+nucleo | data/definitions/economy, data/definitions/scenario, core/economy | Auditoria dos NEGÓCIOS DE SERVIÇO (F8) AO LONGO DO TEMPO — não altera o núcleo. |
| `economy_world_test` | 59.6s | script | economia+nucleo | data/definitions/economy, core/economy, data/definitions/scenario, data/definitions/population | Teste do contrato único: o modelo demo (ciclo completo) inicializado a partir do cenário real (cities/ports/buildings.json) em vez do cenário AGRIA... |
| `convoy_lifecycle_test` | 58.7s | scene | cena/UI | map | Ciclo de vida dos comboios, headless: roda 20 dias da economia real e vigia, frame a frame, cada comboio ativo. |
| `economy_save_load_roundtrip_test` | 58.5s | script | economia+nucleo | data/definitions/economy, data/definitions/scenario, core/economy, data/definitions/population | Teste de determinismo do save/load do núcleo econômico C++ (revisão Q-008/S5). |
| `economy_save_load_v11_compat_test` | 48.5s | script | economia+nucleo | data/definitions/economy, data/definitions/scenario, core/economy | Guarda de retrocompatibilidade do save v11 (revisão Q-009, pré-S6 `93c5440`). |
| `c1_balance_test` | 48.4s | script | economia+nucleo | data/definitions/economy, core/economy, data/definitions/scenario, data/definitions/population | C1 (D-102) — CONTABILIDADE POR BALANÇO. |
| `urban_load_test` | 40.9s | script | economia+nucleo | data/definitions/economy, data/definitions/scenario, core/economy | Teste do pacote W0 da §19 (solo urbano, moradia e prefeitura). |
| `economy_live_path_test` | 34.6s | script | economia+nucleo | data/definitions/scenario, data/definitions/natural, core/economy, data/definitions/economy | Teste do caminho real do jogo ao vivo. |
| `population_template_test` | 31.0s | script | cena/UI | core/ui, core/culture | População no T2: abas Nações/Cidades, busca, ficha lateral e navegação para a ficha T3 na aba População. Também confere o atalho nação → cidades dela. |
| `status_query_test` | 29.7s | script | economia+nucleo | data/definitions/economy, data/definitions/scenario, core/economy | Guardo da A4 (status sob demanda): cada consulta do binding tem de devolver EXATAMENTE o campo correspondente do `get_status()` no mesmo dia. |
| `map_pick_test` | 29.6s | script | cena/UI | game | Verifica o raio ÚNICO de picking em tela e a desseleção no mapa. |
| `deposit_pick_test` | 29.5s | script | cena/UI | game | MEDIÇÃO do pick de depósitos no mapa "resources" (bug: Chaillac idx 1618 não responde a hover/clique; Ferdinand Mine idx 1232 responde). |
| `trade_flow_tooltip_test` | 29.0s | script | cena/UI | game | Tooltip das setas de comércio no modo rotas. |
| `economy_tick_async_test` | 28.4s | script | economia+nucleo | data/definitions/scenario, core/economy, data/definitions/economy | 60 dias no caminho SÍNCRONO (tick_day direto, na main thread) contra 60 dias no caminho ASSÍNCRONO do jogo (WorkerThreadPool via tick_scheduler.gd)... |
| `storage_g2_test` | 28.3s | script | economia+nucleo | data/definitions/economy, core/economy | G2 (D-101) — testes da ARMAZENAGEM (armazéns agregados, reserva nacional, pulmão região→cidade, IA privada de armazém, IPC e save v25). |
| `map_frame_test` | 28.2s | scene | cena/UI | data/definitions/scenario, map | Teste de fumaça POR FRAME do mapa com o cenário real (nações e cidades do cenário, portos do ports.csv). |
| `e6_energy_map_test` | 28.1s | script | cena/UI | data/definitions/energy, map, data/definitions/scenario | E6 — mapa de POTENCIAL renovável (solar / eólica / hídrica). |
| `camera_limit_test` | 28.0s | script | cena/UI | game | Limite elástico da câmera (game/main_game.gd): o centro nunca passa da folga além da folha e a mola o devolve para dentro; destinos de move_to já s... |
| `cursor_hover_test` | 27.2s | script | cena/UI | game, core/ui | Cursor de mão no hover do mapa (cidade, porto e jazida) e seta fora. |
| `price_formation_test` | 26.6s | script | economia+nucleo | core/economy, data/definitions/economy | Formação de preço (D-091 + D-092) — headless, exit != 0 em falha. |
| `wheat_info_test` | 26.5s | script | cena/UI | game | Verifica a ficha do recurso agrícola: para um índice de trigo, deposit_info deve retornar kind "wheat" + aptidão/fertilidade/cultivável do territór... |
| `territory_test` | 26.2s | script | cena/UI | game, core/economy | Território dinâmico: valida a atribuição província->cidade, o override de debug e a textura de dono. godot --headless --path . |
| `deposit_ring_test` | 25.9s | script | cena/UI | game | MEDIÇÃO do tamanho uniforme dos depósitos (mapa "resources"). |
| `d125_efficiency_test` | 25.6s | script | economia+nucleo | data/definitions/economy, data/definitions/production, core/economy | D-125 — eficiência de produção (estilo HOI4). |
| `wheat_card_test` | 25.3s | script | cena/UI | game | Verifica em runtime que a ficha agrícola monta a linha "Clima · Solo · Relevo" (composta de i18n amigável) sem erro, para um índice de trigo real. |
| `label_layout_test` | 24.8s | scene | cena/UI | data/definitions/scenario, map | Teste headless dos rótulos do mapa (cidade + porto), no padrão simples: (a) ZERO linhas-guia (sem Line2D / CanvasLayer de líderes); (b) nenhum nome... |
| `economy_ai_test` | 24.2s | script | economia+nucleo | core/economy | Testes da IA econômica — agora lida pelo LOG DE COMANDOS da A3, porque a IA decidiu mudar de casa: privado, governo e grupos rodam no núcleo C++, d... |
| `city_tooltip_test` | 24.1s | script | cena/UI | core/ui | Tooltip rico de cidade (item 3): atraso de ~250 ms, conteúdo do status, modo de recursos, esconde ao sair e nunca aparece com tela aberta. |
| `renewables_balance_test` | 23.6s | script | economia+nucleo | data/definitions/economy, data/definitions/population, data/definitions/energy | D-122 — BALANCEAMENTO DAS RENOVÁVEIS (decisão de design do dono). |
| `economy_light_signals_test` | 23.6s | script | economia+nucleo | data/definitions/economy, data/definitions/scenario | Teste dos sinais leves por cidade (S7-c): confere que os arrays planos de `status["light"]` são bit a bit idênticos aos campos do bloco pesado por ... |
| `market_template_test` | 23.5s | script | cena/UI | core/ui, data/definitions/economy | Mercado no T2: abas Bens/Ofertas, tabela dos bens com ícone e preço na moeda local, filtro por setor, busca e detalhe do preço entregue. |
| `minimap_territory_test` | 23.4s | script | cena/UI | data/definitions/scenario, map | Verifica o minimapa dinâmico (dono por província, mundo inteiro): (a) a imagem é RGB8 512x256 (mesma resolução do minimap.png); (b) cobre MUITO mai... |
| `production_template_test` | 23.4s | script | cena/UI | core/ui | Produção no T2: abas de pasta Cadeia/Cidades, tabelas com dados reais, busca, ficha lateral e navegação para a ficha T3 da cidade. |
| `services_template_test` | 23.2s | script | cena/UI | core/ui | Serviços (F8) no T2: abas Setores/Negócios, tabela dos 20 setores com ícone do Icon Forge e o selo do setor, lista de negócios com nome gerado pela... |
| `economy_long_run_test` | 23.2s | script | economia+nucleo | core/economy | Simulação longa (365 dias) do cenário de duas cidades. Registra preços, estoques, produção, consumo, emprego, salários, lucros, PIB e padrão de vid... |
| `mine_stage_test` | 23.1s | script | economia+nucleo | core/economy, data/definitions/economy, data/definitions/scenario, data/definitions/natural | D-086 — ESTÁGIO DE MINA: o recurso NUNCA acaba (decisão do dono, 2026-11-17). |
| `statistics_template_test` | 23.1s | script | cena/UI | core/ui | Estatísticas no T5 (Quadro comparativo): ranking por indicador, grupo Mundo/região, posição da nação do jogador e estado vazio da tendência. |
| `nation_city_list_test` | 23.0s | script | cena/UI | core/ui | Lista das cidades da nação do jogador junto ao minimapa (item 5): ordem por população, alerta de déficit, clique centraliza, duplo clique abre a fi... |
| `trade_template_test` | 23.0s | script | cena/UI | core/ui | Verifica a janela de Comércio no T2 (abas de pasta) com dados reais. |
| `business_name_test` | 23.0s | scene | cena/UI | core/culture | Teste headless da base de nomes de empresas (Fase F8 — serviços). |
| `alerts_nav_test` | 22.9s | script | cena/UI | core/ui | Verifica que a tela de Alertas (T2 compacto) leva à cidade na aba certa. |
| `politics_dossier_test` | 22.8s | script | cena/UI | core/ui | Política no T1 (Dossiê): a nação do jogador, a troca para outra nação pela lista, os dados reais do cenário/simulação e a declaração de diplomacia ... |
| `services_test` | 22.8s | scene | cena/UI | data/definitions/economy, core/culture, core/economy | F8 — teste headless dos SERVIÇOS (20 setores). |
| `economy_integration_test` | 22.7s | script | economia+nucleo | data/definitions/economy, core/economy | Teste de integração dos quatro sistemas econômicos: geografia natural, cidades/POPs, produção (trigo consumido diretamente), comércio, investimento... |
| `logistics_template_test` | 22.7s | script | cena/UI | core/ui | Logística no T2: Frota, Câmbio, Tarifas e Portos com dados reais, aplicação de tarifa (set_tariff) e expansão de porto (expand_port). |
| `c5_stock_exchange_test` | 22.3s | script | economia+nucleo | data/definitions/economy, core/economy, data/definitions/population | C5 (D-112) — BOLSA DE VALORES POR PAÍS, CONFIANÇA E EXPECTATIVAS. |
| `f0_fuels_test` | 22.2s | script | economia+nucleo | data/definitions/economy, data/definitions/energy, data/definitions/transport, core/economy | F0 — combustíveis por grau (gasoline / diesel / jet_fuel). |
| `nation_highlight_test` | 22.2s | script | cena/UI | map | Destaque de nação do mapa político (API do TerritoryMap): garante que o material NUNCA fica com o latão de um destaque antigo — set/clear reescreve... |
| `nav_strait_test` | 22.1s | scene | cena/UI | core/economy, map | Verifica os estreitos da máscara de navegação (`map/map_data/nav_mask.png`) com o `NavGrid` REAL usado pelos comboios. |
| `pop_needs_test` | 22.1s | script | economia+nucleo | data/definitions/economy | Teste headless da F3: necessidades das POPs com substituição, níveis por renda, duráveis com reposição anual e consumo institucional do governo. |
| `currency_trade_test` | 22.0s | script | economia+nucleo | data/definitions/economy | F5 — moedas, câmbio, tarifa e preço entregue (opção c: preço na ÂNCORA). |
| `c3_bank_credit_test` | 21.9s | script | economia+nucleo | data/definitions/economy, core/economy, data/definitions/population | C3 (D-107) — CRÉDITO DE EMPRESA PELO BANCO AGREGADO (aceites A2–A7). |
| `urban_land_test` | 21.9s | script | economia+nucleo | data/definitions/economy, core/economy | Teste do pacote W1 da §19 — solo urbano e zoneamento. |
| `c6_bank_crisis_test` | 21.8s | script | economia+nucleo | data/definitions/economy, core/economy, data/definitions/population | C6 (D-114) — CRISE BANCÁRIA, CORRIDA E RESOLUÇÃO. |
| `b5_initial_infra_test` | 21.7s | script | economia+nucleo | data/definitions/economy, core/economy, data/definitions/scenario | B5 (D-089) — INFRAESTRUTURA INICIAL REAL. Teste do dado + da ponte ao núcleo. |
| `economy_income_distribution_test` | 21.7s | script | economia+nucleo | data/definitions/economy, core/economy, data/definitions/scenario | Distribuição de renda por nação: Gini estimado (7 regiões) + quintis da B2. |
| `c2b1_monetary_test` | 21.7s | script | economia+nucleo | data/definitions/economy, data/definitions/population, core/economy | C2b1 (D-104) — EMISSÃO DE MOEDA E JUROS DO BANCO CENTRAL. |
| `c4_banks_test` | 21.7s | script | economia+nucleo | data/definitions/economy, core/economy, data/definitions/population | C4 (D-110) — BANCOS COM DONO (público, privado nacional e estrangeiro). |
| `construction_project_test` | 21.7s | script | economia+nucleo | data/definitions/economy | §15 — construção como projeto físico. Guarda de regressão do cronograma de obra: (1) sem material a obra ATRASA (não falha); (2) o capital é RESERV... |
| `labor_market_test` | 21.5s | script | economia+nucleo | data/definitions/economy | F7 — teste do mercado de trabalho, qualificação e renda. |
| `production_chain_test` | 21.4s | script | economia+nucleo | data/definitions/economy | Teste das cadeias de produção (F2) no núcleo demo: 1. moinho (trigo -> farinha + farelo) + padaria (farinha -> pão): receita de 1 insumo com SUBPRO... |
| `goods_data_test` | 21.3s | script | sistema GDScript | data/definitions/economy, data/definitions/scenario, data/definitions/i18n | Teste headless dos dados das cadeias de produção (Fase F1 do plano). |
| `fleet_fuel_test` | 21.3s | script | economia+nucleo | data/definitions/economy | F4 — frete exige frota e combustível. |
| `vehicle_spacing_test` | 21.3s | scene | cena/UI | map | Teste headless da separação de veículos (`map/convoy_spacing.gd`). |
| `d127_byproducts_test` | 21.3s | script | economia+nucleo | data/definitions/economy | Teste D-127 - subprodutos com balanco de massa real (só dado + builder). |
| `e5_interconnect_test` | 21.2s | script | economia+nucleo | data/definitions/economy, data/definitions/population, data/definitions/energy, core/economy | PARTE E / E5 (D-117) — COMÉRCIO DE ENERGIA ENTRE REDES NACIONAIS VIZINHAS. |
| `c2b2_fx_gold_test` | 21.2s | script | economia+nucleo | data/definitions/economy, data/definitions/population, core/economy | C2b2 (D-106) — REGIMES DE CÂMBIO, PADRÃO MONETÁRIO E OURO FÍSICO. |
| `e2e3_power_test` | 21.2s | script | economia+nucleo | data/definitions/economy, data/definitions/population, data/definitions/energy, core/economy | PARTE E / D-116 — ENERGIA ELÉTRICA: aceites da E2 (usinas/geração) e da E3 (rede, despacho, preço e consequências da falta). |
| `history_service_test` | 21.2s | script | sistema GDScript | core/stats | Histórico de 12 meses (item 9): acúmulo por dia, anel de 30 dias, um ponto por mês (12 meses) com downsampling para o último dia do mês, séries por... |
| `command_replay_test` | 21.1s | script | economia+nucleo | - | A3 - replay da fila de comandos (docs/economy/refactor/A3_commands.md). |
| `menu_smoke_test` | 21.1s | scene | cena/UI | - | - |
| `c2c_real_fx_test` | 21.1s | script | economia+nucleo | data/definitions/economy, data/definitions/population, core/economy | C2c (D-108) — REGIMES CAMBIAIS REAIS, ÂNCORA, CESTA E MOEDA DE UNIÃO. |
| `culture_db_test` | 21.1s | script | sistema GDScript | core/culture | Contrato de leitura das culturas: catálogo (nacional + sem estado), composição nacional, mistura por cidade, papéis (principal/aceita), "outras" no... |
| `investment_test` | 21.0s | script | economia+nucleo | data/definitions/economy, core/economy | F9 — teste de aceite de investimento e propriedade (docs §14). |
| `port_capacity_test` | 21.0s | script | economia+nucleo | - | Teste da mecânica de porto v1 (docs/economy/port_design.md), em mundo sintético e determinístico: 1. |
| `economy_city_status_test` | 21.0s | script | economia+nucleo | data/definitions/economy | Teste do `get_city_status(city_id)` (S7-c, fatia final): o bloco por cidade do `get_status()` agora é LEVE (sem markets/pop/services/buildings/ dep... |
| `c2c_p1_fx_passthrough_test` | 21.0s | script | economia+nucleo | data/definitions/economy | C2c-P1 (D-109) — REPASSE SIMÉTRICO DO CÂMBIO AO PREÇO IMPORTADO. |
| `population_demo_test` | 20.9s | script | economia+nucleo | - | Teste headless do cenário mínimo de duas cidades (POPs, trigo consumido diretamente). |
| `economy_external_income_test` | 20.8s | script | economia+nucleo | core/economy, data/definitions/scenario | Renda externa por PIB PPP per capita (balance_review §20). |
| `e7_tariff_test` | 20.8s | script | economia+nucleo | data/definitions/economy, data/definitions/population, data/definitions/energy | PARTE E / D-120 — E7: TARIFA DE VAREJO por rede e CONTRATO de importação. |
| `minimap_view_test` | 20.7s | script | cena/UI | core/ui | Teste headless do retângulo CARIMBO do minimapa (core/ui/minimap.gd): garante que a área vista fica SEMPRE dentro da textura do mapa, inclusive no ... |
| `c3_bank_seizure_conservation_test` | 20.6s | script | economia+nucleo | data/definitions/economy, core/economy, data/definitions/population | Regressao t153/F0 -- CONSERVACAO EXATA NA EXECUCAO DO CALOTE (C3/C4). |
| `c3_group_balance_test` | 20.6s | script | economia+nucleo | data/definitions/economy, data/definitions/population | C3 (D-107) — BALANÇO POR GRUPO (empresa com balanço). |

## Candidatos a rodar sem Godot (CLI C++)

O nucleo ja tem um executavel sem Godot: `gdextension/src/cli/main.cpp` -> `opengs_sim_cli`
(`--snapshot in.json --days N --out out.json [--threads T] [--ai 0|1]`). Ele NAO tem um harness de
assercoes: roda o mundo e grava o save. Hoje a unica prova CLI x Godot e `tools/world_equivalence.ps1`
(exporta snapshot com Godot e compara campo a campo). Testes GDScript listados abaixo sao de economia
pura (nao abrem cena, UI nem mapa) e sao os melhores candidatos a um porte futuro, por ordem de custo:

| teste | tempo | o que ja prova (cabecalho) |
|---|---:|---|
| `c2c_ab_365_test` | 781.3s | C2c (D-108) — A/B POR CANAL, 365 d com IA (threads=1), tres mundos em sequencia NO MESMO processo (1 |
| `economy_bistability_test` | 320.1s | Teste de regressão da biestabilidade de liquidez (GAME_STATE §15 "fome com alimento em estoque"). |
| `c2b2_fx_stress_test` | 275.3s | C2b2 (D-106) — ESTRESSE FORTE DO CÂMBIO: 365 d com reservas mínimas. |
| `construction_queue_test` | 271.7s | §15 unificado — MORADIA, PRÉDIO E INFRA NA MESMA FILA DA CIDADE. |
| `city_gov_test` | 214.2s | Teste do pacote W2 da §19 — prefeitura (docs/economy/s19_urban_spec.md). |
| `b6_property_test` | 209.8s | B6 — PATRIMÔNIO IMOBILIÁRIO DA REGIÃO (D-095): terra produtiva, moradia do interior e anel periurban |
| `strata_test` | 204.2s | B2 — ESTRATOS DE RENDA (quintis) COM PATRIMÔNIO. |
| `b4_migration_test` | 186.9s | B4 — MIGRAÇÃO (êxodo rural → polo urbano e cidade → cidade na mesma nação). |
| `agriculture_seasonal_test` | 183.7s | G — teste headless da AGRICULTURA SAZONAL (D-096). |
| `agri_calendar_owner_test` | 153.9s | G — CALENDÁRIO DE SAFRA HERDADO DA REGIÃO (decisão do dono, D-098). |
| `c2_central_bank_test` | 148.5s | C2a (D-103) — BANCO CENTRAL POR MOEDA: estado + medição. |
| `b7_informality_test` | 146.6s | B7 — INFORMALIDADE, SERVIÇOS INFORMAIS, MORTALIDADE E INSATISFAÇÃO + save v24. |

Observacao: nenhum snapshot de mundo esta versionado (os dumps reais tem dezenas de MB).
Para tirar esses testes do Godot e preciso (a) uma fixture de snapshot pequena e versionada e
(b) assercoes no CLI/binario ou num script externo. Enquanto isso, `-Parallel` do runner so
aceita checagens de CLI declaradas em `tools/test_impact_map.json`.
