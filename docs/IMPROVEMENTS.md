# Melhorias e pendências — roadmap

Lista consolidada do que foi conversado: o que já foi feito, o que está pendente e
o que fica como backlog. Serve para não perder decisão no histórico do chat.

Legenda: **FEITO** · **PENDENTE** (próximo) · **BACKLOG** (depois) ·
**DECISÃO** (regra já escolhida).

---

## 1. Economia — robustez do núcleo (C++)

> Regra: toda mudança aqui exige rebuild do GDExtension e testes de economia.

| # | Item | Status | Nota |
|---|---|---|---|
| 1.1 | **Contrato único** (legacy trigo × demo completo) | EM ANDAMENTO | `initialize_demo_world` + `demo_trade_wheat` (C++) rodam o mundo real no demo; falta trocar o jogo ao vivo (`Economy` autoload) e ligar a IA. |
| 1.2 | **Bens e receitas data-driven** (IDs string) | PENDENTE | Hoje bens são índices (trigo=0…) e receitas estão hardcoded/duplicadas. |
| 1.3 | **Preço/mercado para N bens** | PENDENTE | Demo cobre poucos bens; generalizar com piso de custo e banda. |
| 1.4 | **Capacidade de transporte** | PENDENTE | Hoje frete/tempo existem, mas não há limite de carga — comércio pode mover “infinito”. |
| 1.5 | **Mecânica de extração** (reserva → taxa) | BACKLOG | Reserva não esgota; influencia quanto se extrai. Capacidade da cidade + porto. |
| 1.6 | **Offshore acessível por porto** | BACKLOG | Campos offshore decorativos até existir a extração; porto será o gateway. |
| 1.7 | **Demografia** | BACKLOG | Sem nascimento/migração, cidades estagnam no long-run. |
| 1.8 | **Setor externo** (importação/exportação) | BACKLOG | Choques externos para o mundo não ficar fechado. |
| 1.9 | **Terreno/clima na produção** | BACKLOG | Relevo placeholder; clima já existe em `nature.json`. |
| 1.10 | **Falência/insolvência** | PENDENTE | Regra explícita para caixa negativo (dívida? fecha? transferência?). |
| 1.11 | **Dinheiro inteiro/fixed-point** | PENDENTE | Evitar drift de `double` em partidas longas. |
| 1.12 | **NaN/Inf/divisão por zero + clamps** | FEITO | `safe_*` no initialize/tick/outputs; fix da fase 9 (preço pós-choque `lower>upper`). |
| 1.13 | **Invariantes em runtime** (não só teste) | FEITO | `demo_check_invariants` + snapshot/revert no `tick_demo`; conservação de dinheiro/estoque/preço. |
| 1.14 | **Determinismo total** (FPS, ordem, dicionários) | FEITO | `stable_sort` nas ordenações de comércio. |
| 1.15 | **Tick fixo + catch-up** | FEITO | `_process` acumula dias em fila com teto `MAX_DAYS_PER_FRAME`. |
| 1.16 | **Save/load versionado + migrações** | FEITO | `kDemoSaveVersion`; `demo_from_dict` recusa versão futura. |
| 1.17 | **Performance/escala** (50–200 cidades) | PENDENTE | Benchmark sintético; hotspots: matching de comércio e transporte. |
| 1.18 | **Validação de schema dos dados** | PENDENTE | IDs duplicados, referência inexistente, unidades (kt/Mt), faixas — falhar no build. |
| 1.19 | **Golden runs de balanceamento no CI** | PENDENTE | Limites de inflação/desemprego/preço; falha o build se sair da faixa. |
| 1.20 | **Observabilidade** (série temporal + log de decisões) | PENDENTE | PIB, inflação, comércio, desemprego por dia; diff de estado. |
| 1.21 | **Isolamento de falha** | PENDENTE | Erro no tick congela a economia com aviso; estado anterior preservado. |
| 1.22 | **Property/fuzz tests** | BACKLOG | Entradas aleatórias não podem gerar NaN/caixa negativo/preço fora da banda. |
| 1.23 | **Fonte única de verdade** | DECISÃO | Só `EconomySim` muta economia; UI/AI leem. Nada de economia paralela. |

---

## 2. Mapa, recursos e interface (GDScript)

### Feito

- **Base cartográfica branca** (marfim + mar branco + contorno + relevo suave),
  substituindo o satélite; `paper_map.gdshader`, `paper_flat.png`.
- **Botões quadrados de mapa** acima do minimapa: político e recursos.
- **Mapa de recursos**: billboards nítidos, tamanho ∝ √reserva, trigo em losangos.
- **Filtro por recurso** na legenda: caixa de seleção, cor, nome e contagem;
  “Todos” religa; mapa reage na hora.
- **Ficha do depósito** na legenda: recurso, campo, reserva (Mt), qualidade,
  profundidade e estágio, com tooltips explicando cada campo.
- **Hover do depósito**: cursor “i” + **anel de latão com pulso** (sem badge duplicado).
- **Clique do depósito** abre a ficha; **clique da cidade** mostra os recursos
  das províncias dela; “Voltar” restaura os filtros.
- **Acesso por território**: depósito onshore pertence à província → cidade dona
  (`TerritoryMap.city_for_province`), sem raio, respeitando fronteira.
  Offshore fica decorativo por enquanto.
- **Cursor por alvo**: seta (mapa), lupa (cidade no político), “i” (cidade/depósito
  nos mapas de dados), mão (porto/construção), quatro setas (arrasto).
- **Clique no rótulo** (nome/bandeira/selo) seleciona cidade e porto.
- **Capital pela coluna `capital` do CSV**: nome um pouco maior + estrela no selo.
- **Rótulos padronizados**: cidade/porto no mesmo tamanho; só capital maior.
- **Modelo de marcadores**: selo escuro + aro de latão; cidade (prédios), porto
  (âncora), capital (estrela).
- **Dataset de recursos enriquecido**: cobertura global, pré-sal, Guianas, margem
  equatorial, Venezuela (Orinoco Belt/Maracaibo em terra); offshore só até ~320 km.
- **Ferramentas de evidência**: `ui_capture` com `--mapmode`/`--legend`;
  `map_capture` com `--hide`; capturas commitadas em `docs/map_visual/`.

### Pendente

| # | Item | Nota |
|---|---|---|
| 2.1 | **Unidades na legenda da cidade** | Trocar “×N” por “×N · reserva somada em Mt” para minério; trigo fica “aptidão %”. Explicar a diferença (contagem de campos × aptidão de área). |
| 2.2 | **Destaque do território ao selecionar a cidade** | Hoje só o anel de seleção da cidade; o ideal é realçar as províncias donas (requer máscara por cidade no overlay). |
| 2.3 | **Relevo real (DEM)** | `height.png`/`normal.png` planos; gerar hillshade do Natural Earth para o mapa branco. |
| 2.4 | **Porto como gateway offshore** | Quando a extração entrar, offshore acessível pelo porto mais próximo dentro do raio. |
| 2.5 | **Nomes de cidades faltantes** | Venezuela/Suriname/Guiana não têm cidade no cenário (ex.: Caracas?) — sem cidade, território sem dono. |

### Backlog / ideias

- Exportar o mapa de recursos como imagem (relatório).
- Pulsar/realçar o território ao hover da cidade (não só ao clicar).
- Tooltip da mina no mapa (sem abrir a ficha).
- Salvar o estado dos filtros entre sessões.
- `docs/ideas/`: referência da tela de batalha do HOI4 (composição e o que reaproveitar).

---

## 3. Dados e cenário

| # | Item | Status |
|---|---|---|
| 3.1 | CSV→JSON de cidades/portos/construções/nações/i18n | FEITO |
| 3.2 | `capital` no `cities.csv` (fonte da estrela/tamanho) | FEITO |
| 3.3 | Depósitos: localização real + atributos estimados (`data_quality`) | FEITO |
| 3.4 | Supplement de campos reais (`deposits_supplement.json`) | FEITO |
| 3.5 | Offshore: petróleo/gás até ~320 km; terra para os demais | FEITO |
| 3.6 | Província + offshore por depósito em `resource_points.json` | FEITO |
| 3.7 | Validar CSV/JSON no build (schema/faixas/unidades) | PENDENTE |
| 3.8 | Adicionar cidades que faltam (ex.: Caracas/VEN) e bandeiras | PENDENTE |

---

## 4. Ferramentas e processo

| # | Item | Status |
|---|---|---|
| 4.1 | Testes headless de economia (integração, IA, long-run) | FEITO |
| 4.2 | `tools/economy_lab.gd` (laboratório de cenários) | FEITO |
| 4.3 | `tools/check_balance.py` + docs | FEITO |
| 4.4 | `compile_commands.json` para clangd | FEITO (`gdextension/build-*/compile_commands.json`, 1091 entradas) |
| 4.5 | Serena + clangd para o C++ | BACKLOG (só se o núcleo crescer) |
| 4.6 | `docs/IMPROVEMENTS.md` (este arquivo) | FEITO |
| 4.7 | `docs/ideas/` (referências criativas) | FEITO |

---

## 5. Ordem recomendada

1. **Determinismo + tick fixo/catch-up** (base de tudo).
2. **Save versionado + round-trip + migração**.
3. **Dinheiro inteiro + NaN/clamps + invariantes em runtime**.
4. **Contrato único + bens/receitas data-driven**.
5. **Capacidade de transporte + preço para N bens**.
6. **Extração (reserva→taxa) + offshore por porto**.
7. **Demografia, setor externo, terreno**.
8. **Validação de dados + golden runs + benchmarks no CI**.
9. UI: unidades/reserva somada na legenda; destaque de território; DEM.

### Critério de “não falhar”

Uma partida longa (30 h simuladas) precisa terminar com: sem NaN/Inf, caixa e
estoques não negativos, preços dentro da banda, save/load idêntico, e
determinismo sob FPS variável.
