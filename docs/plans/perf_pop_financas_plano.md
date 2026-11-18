# Plano — Performance, População e Sistema Financeiro

> Status: PROPOSTA (2026-11-17), aguardando aprovação do dono. Nada aqui está implementado.
> Ordem obrigatória: **Parte A (performance/arquitetura) → Parte B (população) → Parte E (energia) e
> Parte C (finanças)**. E e C são independentes entre si e podem correr em paralelo.
> Pré-requisitos: feature em andamento (§19 / obras) fechada e `chore/repo-hygiene` (4c50cc7) mesclada.
> Regra geral: em divergência entre este plano e o código, o código vence. Confirme antes de assumir.

## 0. Por que nesta ordem

- A Parte B multiplica o estado por cidade (faixas etárias × estratos) e a Parte C adiciona balanços
  e mercados financeiros. Sem a Parte A, cada camada nova piora o tick e os sweeps de calibração.
- A Parte C depende da B: hipoteca, poupança e bolsa precisam de riqueza por estrato e de ciclo de vida.
- Cada fase = 1 branch em worktree próprio, 1 versão de save (com migração), 1 parecer de revisão.

## 1. Onde estamos (medido/lido em 2026-11-17)

- 179 nações, 834 cidades, 21 produtos, 20 setores de serviço. Tick ~42–48 ms/dia, status ~25 ms,
  531 MB (medidos com editor aberto — número ruidoso).
- População: **1 POP por cidade** (`DemoPop`). 3 níveis de educação (`kDemoEducationLevels = 3`), que
  também servem de estrato de renda. Idade = 4 contagens estáticas (`working_age`, `retirees`,
  `students`, `recruit_reserve`); **sem nascimento, morte, envelhecimento ou migração**.
  Poupança = **um escalar por cidade** (`savings`); sem dívida, sem patrimônio.
- Empresas: `DemoGroup` tem caixa e lucro distribuível; **sem dívida, sem balanço, sem ações**.
- Crédito: não existe internamente; o exterior concede crédito infinito (`foreign_cash` negativo).
- Simulação cobre `sim_population` ~1,27 bi (aglomerações) de 8,20 bi exibidos.
- IA econômica em GDScript lendo `Dictionary`; status publicado inteiro todo dia em buffer duplo.

---

## PARTE A — Performance e arquitetura

Critério comum de A1–A5 e A7: **equivalência funcional** — JSON do mundo em 60 d idêntico campo a campo
ao pré-mudança, goldens S0 inalterados, bateria de economia verde. Só a A6 muda resultado.

### A0 — Benchmark canônico (antes de tudo)
- Release, headless, **editor fechado**, 834 cidades, 60 d, tempo por fase (`profiling.cpp`), 3 rodadas.
- Saída versionada em `docs/economy/perf/baseline_A0.json`. Todas as fases seguintes comparam com ele.
- Aceite: variação entre rodadas ≤ 3%; tabela por fase publicada.

### A1 — Núcleo como biblioteca pura + executor de linha de comando
- Separar `sim/` (C++ puro, sem Godot) do binding (`economy_sim.cpp`, hoje 6 mil linhas).
- `opengs_sim_cli`: carrega cenário, roda N dias, grava JSON. Sweeps 365 d × 5 em processos paralelos.
- Remover os modos `legacy` e `benchmark`; renomear `demo_*` (é o jogo real) numa fatia mecânica própria.
- Aceite: sweep 365 d × 5 pelo CLI com o mesmo resultado do caminho Godot; tempo total do sweep registrado.

### A2 — IDs inteiros e arrays contíguos
- Internar nação/setor/grupo/produto em índices no carregamento; tirar `std::string` e `std::map` do
  caminho quente (`investment.cpp`, `logistics.cpp`, `world_state.h`, `DemoGroup::profit_by_host`).
- Estado quente em arrays `[cidade × produto]` / `[cidade × setor]`.
- Aceite: equivalência + ganho medido por fase vs A0.

### A3 — Fila de comandos
- Ações do jogador e da IA viram comandos aplicados no início do dia (ordem determinística).
- Remove `demo_status_invalidate()` e o mutex recursivo como caminho normal.
- Aceite: mesmos comandos + mesma semente = mesmo mundo (teste de replay).

### A4 — Status sob demanda
- Trocar o `Dictionary` do mundo inteiro por consultas: coluna (`PackedFloat64Array`) para modo de
  mapa, ficha de uma cidade, ficha de uma nação, ranking. Migrar leitores (HUD, telas, testes).
- Aceite: fase `status` do tick ≈ 0; consulta de ficha < 1 ms; buffer duplo removido.

### A5 — IA econômica em C++ como fase do tick
- Portar `private_investor`, `group_investor`, `government_ai` (leem arrays, emitem comandos da A3).
- Paralela por nação, determinística; some o fatiamento por frame.
- Aceite: equivalência de decisões em 60 d (ou diferença explicada e aprovada); virada do dia sem pico.

### A6 — Cadência por sistema, escalonada (MUDA RESULTADO)
- Diário: produção, consumo, preços, comércio. Semanal: salários/emprego. Mensal: moradia, solo,
  prefeitura, investimento, crédito. Anual (em fatias mensais): demografia.
- Cidade *i* roda as fases mensais no dia `i mod 30`.
- Aceite: recalibração — sweep 365 d × 5 dentro das tolerâncias atuais; goldens regenerados com parecer.

### A7 — Comércio paralelo determinístico
- Ofertas calculadas em paralelo (só leitura); liquidação em ordem fixa. Sem `fast-math`, sem soma
  paralela em ordem variável.
- Aceite: resultado idêntico com 1 e N threads.

**Meta da Parte A (a confirmar após A0):** dia completo (tick + IA) ≤ 15 ms p95 em 834 cidades;
sweep 365 d × 5 em poucos minutos pelo CLI.

---

## PARTE B — População: faixas etárias, estratos e grupos

Objetivo: sair de "1 POP por cidade" para **coortes**, sem simular indivíduos.

### B1 — Demografia por faixa etária
- 5 faixas: 0–14, 15–24, 25–44, 45–64, 65+. Nascimento, morte e envelhecimento por taxa nacional.
- Dados: ONU World Population Prospects (estrutura etária, fecundidade, mortalidade por país).
- A idade passa a dirigir: força de trabalho, demanda de educação (jovens) e saúde (idosos),
  aposentadoria, reserva de recrutamento.
- Aceite: pirâmide de 2026 por nação bate o dado; projeção de 10 anos plausível (JPN envelhece, NGA cresce).

### B2 — Estratos de renda com patrimônio
- 5 estratos (quintis) por cidade, separados dos 3 níveis de educação (educação define o acesso a vagas;
  estrato define consumo e poupança).
- Cada estrato: população, renda (salário/capital/transferência), **poupança, dívida, patrimônio**,
  condição de moradia (proprietário / inquilino / informal — liga com §19).
- Taxa de poupança por ciclo de vida (faixa etária) e por estrato.
- Aceite: Gini **emerge** perto do dado sem âncora direta; fração de renda gasta em comida cai com a renda.

### B3 — Cidade = aglomeração + interior — DECIDIDO PELO DONO
- Toda a população nacional (8,20 bi) entra na simulação, sem criar cidades novas: cada cidade passa a
  ter **dois segmentos** — **urbano** (a aglomeração real) e **interior** (toda a população das províncias que a
  cidade já administra via `assign_provinces`: campo, vilas e cidades pequenas fora do roster;
  no código `urban` / `hinterland`). Substitui o `sim_population` de 1,27 bi.
- **Rateio:** população real por província (raster GPW/WorldPop, mesmo pipeline do WorldClim) somada
  para a cidade dona da província. NÃO ratear proporcional à população urbana.
- O interior usa as mesmas 5 faixas × 5 estratos, com regras próprias:
  - **Trabalho:** empregado rural (fazendas/minas/madeira existentes, salário); agricultor familiar
    (terra não ocupada por fazenda comercial, produtividade baixa, come primeiro e vende o excedente);
    rural não agrícola (informal). Proporção ancorada em `sector_employment.json` (ILOSTAT).
  - **Renda:** salário, venda de excedente, aposentadoria rural, transferências, remessas de migrantes.
  - **Moradia:** casa própria autoconstruída, sem aluguel e sem favela; índice de qualidade + cobertura
    de infraestrutura rural (luz, água, estrada). O §19 (solo, aluguel, prefeitura) vale só para o urbano.
  - **Consumo:** autoconsumo de comida fora do mercado e sem frete; o resto no mercado da cidade, com
    custo de acesso que cai com a estrada. Acesso menor a saúde/educação.
- **Cidade + Região (decisão final do dono, 2026-11-17; substitui "interior dividido" e "três segmentos"):**
  2 blocos por cidade. A **Região** é um bloco só (cidades pequenas + campo, toda a população fora da
  aglomeração; dado em data/definitions/population/hinterland_2026.json), SEM mercado de solo/moradia e
  SEM prefeitura própria — a simulação urbana completa existe só na cidade. A urbanização real da nação
  (~57% vs ~26% coberto pelas aglomerações) define apenas o MIX de empregos da Região (agro/mineração/
  madeira × indústria leve/serviços locais). Ressalvas de dado: regiões enormes (ex.: Patna 226 M);
  7 aglomerações maiores que a região ficam com Região 0.
- **Região = ESTADOS INTEIROS (decisão do dono, opção 1):** a região de cada cidade passa a ser um ou mais
  estados (admin-1) completos, seguindo a divisa real, em vez de "província → cidade mais próxima". A cidade
  é a principal do estado; estado sem cidade no roster se junta ao estado vizinho com cidade (mesma nação).
  Sem cidades novas, sem custo de perf. Regiões com nome real (São Paulo, Bihar, Texas). Opção 3 APROVADA
  junto da B3: capitais extras só para estados grandes sem cidade (Índia/China sobretudo; ex.: Kolkata 242 M,
  Guangzhou 238 M).
- **Papel de gameplay da Região:** é a camada de TERRITÓRIO da cidade — onde ficam terra, jazidas, usinas
  (Parte E), malha de transporte (Parte F), cobertura de luz/água (E7), base de recrutamento e, no futuro,
  o território ocupado na guerra. Alavancas do jogador por região: estrada/ferrovia, eletrificação e
  água, programas agrícolas, concessões de mineração, usinas. Modo de mapa por região.
- Rótulo do mapa continua mostrando a aglomeração; a ficha mostra também o total da região.
- **Recalibração grande** (~6,5× consumidores): fase própria, sweep dedicado, goldens refeitos.
- Aceite: população nacional simulada = dado real; fatia de emprego agrícola cai com a renda do país;
  renda rural < urbana; fome dentro das tolerâncias após recalibração.

### B4 — Migração
- **Êxodo rural** (interior → urbano na mesma cidade): migra quando salário urbano × chance de emprego −
  custo de moradia supera a renda rural; saem primeiro 15–44 anos (o campo envelhece). Quem fica tem
  mais terra por pessoa; a chegada pressiona aluguel/favela (§19). Mecanização reduz vagas rurais.
- Cidade → cidade e internacional em fatias seguintes.
- Aceite: taxa de urbanização por nação perto do dado; `housing_stress` responde à entrada.

Custo estimado de estado: 834 cidades × 2 segmentos × 5 faixas × 5 estratos = ~42 mil coortes leves — cabe com a Parte A.

---

### B5 — Gameplay da Região/Estado (aprovado pelo dono, 2026-11-17)
- **Logo após a B3:** (1) infraestrutura do estado — estrada, luz, água (custo de acesso, produtividade,
  êxodo); (2) concessões de mineração e terra pelo sistema de grupos F9 (privatizar/estatizar/estrangeiro).
- **Planos futuros aprovados:** (3) desigualdade regional + transferência do governo central a estados
  pobres; (4) êxodo para a capital com efeito em favela/aluguel; (5) eventos por estado com dado real
  (seca/quebra de safra, esgotamento de mina, boom de commodity); (6) instabilidade por estado (com
  política) e ocupação de estados na guerra.
- **Evitar:** prédio a prédio no estado, preço separado por estado, mercado imobiliário prédio a prédio no campo (o patrimônio agregado da região entra na B6).


### B6 — Patrimônio imobiliário da região (terra + moradia, extensão da cidade) — aprovado pelo dono, 2026-11-18
- **Objetivo:** dar patrimônio real às pops do interior (6,1 bi) e ligar riqueza a terra/moradia; base para a Parte C (garantia, hipoteca, crédito rural).
- **Modelo (agregado por região, cadência mensal):**
  - **Terra produtiva:** hectares × valor que segue a renda das fazendas/minas da região, a infra da B5 (estrada/luz/água) e o preço das commodities (boom sobe, seca/quebra derruba).
  - **Moradia:** estoque, preço e aluguel por região; **anel periurbano** ligado ao preço da cidade-polo, descontado por distância/estrada. Quando a moradia da cidade lota (limite da B4), o excedente vai para o anel (periferia/favela — absorve o item 4 da B5).
  - **Propriedade por quintil (B2):** concentração de terra (latifúndio × pequeno produtor) entra no patrimônio e no Gini.
- **Efeitos:** riqueza e desigualdade do interior; valorização por obra de infra (imposto sobre imóvel/terra como receita do Estado); migração B4 passa a pesar aluguel do anel × cidade; pronto para virar garantia na C1/C4.
- **Evitar:** prédio a prédio no campo, construtoras no campo, compra e venda individual. (A regra antiga "sem mercado imobiliário no campo" passa a ser "sem mercado prédio a prédio no campo".)
- **Ordem:** depois do marco M2, desenhada junto com C1 (balanço) para o patrimônio já nascer como ativo.
- **Requisitos:** conservação de dinheiro, save novo com migração, testes (valorização por infra, transbordo para o anel, conservação), goldens, CLI×Godot, 365 d, bench.

### B7 — Informalidade, serviços informais, mortalidade e insatisfação — aprovado pelo dono, 2026-11-18
- **Trabalho informal:** fração informal da força de trabalho por quintil e por cidade/região (dado real com fonte — ex.: OIT, emprego informal por país; sem dado → parâmetro declarado). Informal não paga imposto de renda nem contribuição, ganha menos, é o primeiro a perder renda na crise e não tem proteção. Formalização sobe com renda, educação, infra e fiscalização.
- **Serviços informais:** setor próprio onde esses trabalhadores atuam — comércio ambulante, transporte informal, construção/reforma informal, serviços pessoais e domésticos. Mais barato, sem imposto, produtividade menor, consumido principalmente pelos quintis de baixo (cidade, anel periurbano da B6 e região). Concorre com o serviço formal; cresce quando o formal está caro ou falta emprego formal. Agregado por cidade/região, sem firma individual.
- **Mortalidade ligada a comida e saúde:** a mortalidade das coortes (B1) deixa de ser só tabela fixa — sobe com falta de comida (food_avg) e de serviço de saúde, com peso maior em 0–14 e 65+; fome prolongada também empurra migração (B4). Tabela real continua sendo a base em situação normal.
- **Insatisfação por quintil e região:** índice medido a partir de desemprego (peso jovem 15–24), aluguel/ônus de moradia, fome, inflação, desigualdade de terra (B6) e informalidade. Só medição e status agora; vira entrada da parte política (protesto, instabilidade, eleição) quando ela existir.
- **Requisitos:** conservação de dinheiro e de pessoas, save novo com migração, testes, goldens, CLI×Godot, 365 d, bench.

### Futuro (depende de leis/política e diplomacia)
- **Previdência e aposentadoria** (coorte 65+ da B1, contribuição dos formais da B7, déficit fiscal, reforma como lei) — junto com a parte política/leis.
- **Remessas e migração internacional, inclusive ilegal** — junto com política/diplomacia e fronteiras.

---

## PARTE G — Agricultura sazonal (base econômica; pedido do dono, 2026-11-18)
- **Safra por época:** cada primário de comida (grãos, arroz, soja, cana, frutas, carne/leite com ciclo
  próprio) tem calendário de plantio e colheita por hemisfério/clima (dado real com fonte, ex. calendários
  de cultivo da FAO); a produção chega concentrada na colheita, não diária.
- **Rendimento por hectare** (dado real por país/cultura) × área plantada da região (B3/B6); clima/seca
  como variação (eventos por estado ficam para depois).
- **Armazenagem e estoque:** colheita enche o estoque, consumo esvazia ao longo do ano; perda por
  armazenagem ruim (infra B5); preço sobe na entressafra e cai na colheita.
- **Cadeia alimentar:** comprar comida básica (primário) → processar (moinho, frigorífico, laticínio,
  indústria de alimentos) → produto que rende mais/dura mais; importador decide comprar primário e
  processar ou comprar processado.
- **Insumos:** fertilizante (liga com gás/energia — Parte E), sementes, diesel/maquinário.
- **Efeito esperado:** resolve de forma realista a dívida de comida do interior (B3) e dá dinâmica de
  estoque, entressafra e especulação.
- **Requisitos:** conservação, save novo, testes (safra chega na época, estoque atravessa a entressafra,
  preço sazonal), goldens, CLI×Godot, 365 d (comida por país), bench.

## Ordem combinada com o dono (2026-11-18)
1. **Terminar a base econômica:** B6 (patrimônio da região) e B7 (informalidade/serviços informais/
   mortalidade/insatisfação) → **Parte G (agricultura sazonal)** → Parte C (balanço, BC por moeda com
   emissão/regimes/padrão monetário, bancos, **bolsa por país (C5) com confiança/expectativas** puxando
   investimento e consumo) → E (energia) e F (transporte).
2. **Macro político (próxima grande parte):** impostos e orçamento, subsídios e controle de preços,
   balança de pagamentos e dívida externa refinadas, previdência, salário mínimo/leis.
3. **Órgãos internacionais (com a diplomacia):** FMI (socorro com condições), agências de **rating de
   crédito** (risco soberano → juro dos títulos), ONU, troca de hegemonia da moeda global, sanções,
   blocos, geopolítica de bancos estrangeiros, remessas/migração internacional.
4. **Telas:** desenhadas uma a uma com o dono (o que cada tela precisa mostrar), depois da base.
- **Nomes:** bancos e empresas com **nomes genéricos/fictícios** (sem marcas reais, por direitos);
  bancos centrais como "Banco Central de <país/moeda>"; ocupações informais com nome genérico + termo
  local comum (camelô, mototáxi, boda-boda, tuk-tuk…).

## PARTE C — Sistema financeiro

### C1 — Contabilidade por balanço (novo invariante)
- Banco cria dinheiro ao emprestar: `money_total` conservado deixa de valer.
- Novo invariante: **todo ativo financeiro é passivo de alguém; ativos = passivos + patrimônio em cada
  agente**; soma mundial de posições financeiras = 0. Teste diário, como o atual.

### C2 — Banco central e banco agregado (v1)
- **Regra anti-espaguete:** o núcleo nunca pergunta "qual o BC deste país"; pergunta "qual a moeda deste
  país" e "qual o BC desta moeda" (`DemoCurrency.nations` já modela moeda compartilhada).
- Banco central **por moeda** (zona do euro, franco CFA, dolarizados). Juro básico, emprestador de
  última instância, reservas cambiais (liga com `fx.cpp`).
- 1 banco agregado por nação: depósitos, empréstimos, reservas, capital. Juro = básico + risco.
- Crédito a empresas e ao governo; **títulos públicos** substituem o crédito externo infinito.
- O setor de serviço `finance` (já existe em `services.json`) vira a face de emprego/receita do banco.
- **Regimes e padrão monetário (pedido do dono, 2026-11-18):**
  - Regime cambial escolhível por moeda: **flutuante, fixo (peg), banda/administrado**; troca de regime é
    decisão de política com custo (reservas, confiança).
  - **BC independente × controlado pelo governo** (onde controlado, o jogador/IA mexe no juro e na emissão
    direto; independente segue regra própria, ex. meta de inflação).
  - **Padrão monetário internacional** como estado do mundo: fiduciário (início), **Bretton Woods**
    (moedas presas à moeda-âncora, âncora presa ao ouro) ou **padrão ouro** (emissão limitada às reservas).
  - **Reservas** de cada BC: ouro + moeda(s) de reserva; a **moeda de reserva/comércio global** começa no
    dólar (tende a ser a da maior economia) — mudança de hegemonia (outro país assumir) fica como **evento
    futuro** (ONU/diplomacia).
- **Emissão de moeda (pedido do dono, 2026-11-18):**
  1. **Base monetária (M0)** criada pelo BC ao comprar títulos públicos, emprestar aos bancos ou financiar
     o Tesouro direto (monetização do déficit) — registrada como passivo do BC (invariante da C1).
  2. **M1/M2 emergem do crédito**: empréstimo bancário vira depósito; multiplicador sai do comportamento
     dos bancos (não fórmula fixa). Alavancas: juro básico e compulsório.
  3. **Inflação e câmbio** ligados à moeda × produção: emissão demais → inflação e desvalorização (dá
     sentido ao câmbio flutuante; hoje todas as moedas estão em peg).
  4. **Senhoriagem com limite**: imprimir alivia o governo no curto prazo e cobra em inflação; caso
     extremo, hiperinflação.
  5. **Zona de moeda única** (euro, CFA, dolarizados): o país não emite sozinho; sair da zona devolve a
     emissão com choque de câmbio e de confiança (liga com blocos futuros).
- Aceite: importadores sem caixa (DJI/SOL/MRT/UZB) financiam comida por crédito, com limite;
  `foreign_cash` sem crédito infinito.

### C3 — Empresa com balanço
- `DemoGroup` ganha dívida, estoque, capital fixo e patrimônio líquido. Falência atinge credores.
- Aceite: alavancagem responde ao juro; falência gera perda no banco, não some dinheiro.

### C4 — Bancos público, privado e estrangeiro + crédito às famílias
- 3 bancos por nação, donos pelo sistema da F9 (estatal / nacional / estrangeiro).
- Hipoteca e imóvel como ativo dos estratos (liga com §19 W3). Crédito dirigido do banco público.
- Aceite: preço/renda de moradia e aluguel/renda em faixa plausível; juro alto esfria construção.

### C5 — Bolsa de valores
- Grupos listados; valor = lucro esperado descontado pelo juro × confiança; atualização semanal.
  Sem livro de ordens. Donos: estratos ricos, grupos, estrangeiros, Estado.
- Três ligações: ação em alta financia investimento; queda reduz consumo dos ricos; capital
  estrangeiro contagia.
- Aceite: preço/lucro em faixa plausível; choque de juro derruba o índice.

### C6 — Crise e contágio
- Corrida e falência bancária, resgate, exigência de capital, controle de capitais.
- **Recuperação/resolução de bancos (pedido do dono, 2026-11-18)** — hierarquia BC (por moeda) →
  bancos público, privado e estrangeiro (por nação, C4):
  - **Liquidez:** BC como emprestador de última instância (banco solvente sem caixa, com garantia e juro
    punitivo).
  - **Seguro de depósito** (tipo FGC): fundo pago pelos bancos; protege depositantes até um teto.
  - **Solvência:** resgate pelo Tesouro (bail-out, vira dívida pública), bail-in (credores/acionistas
    perdem), fusão forçada com banco saudável, estatização, ou liquidação.
  - **Banco público:** capitalizado pelo Tesouro (custo fiscal; pode ser usado para crédito dirigido e
    acumular perda). **Banco estrangeiro:** matriz pode socorrer ou abandonar — canal de contágio
    externo.
  - Em zona de moeda única o BC é comum: socorro depende do bloco, não só do país.
  - Aceite: falência de banco gera perda para alguém (depositante acima do teto, credor, acionista ou
    Tesouro) — nunca dinheiro que some; invariante da C1 fecha em toda crise.

**Alavancas rurais (Parte B):** estrada e eletrificação rural, subsídio/preço mínimo agrícola, reforma
agrária, crédito rural (banco público, C4), aposentadoria rural, cobertura de escola e saúde.

**Alavancas do jogador (API no núcleo agora; painel depois):** juro básico (onde o BC não é
independente), crédito dirigido, emissão de dívida, resgate, exigência de capital, controle de capitais,
abertura da bolsa a estrangeiros.

---

## PARTE E — Energia elétrica (depois da Parte B; dados podem ser coletados já)

Hoje não existe: `sim_constants.h` registra que energia não tem preço próprio no núcleo. Já existem os
combustíveis (`coal`, `gas`, `oil`, `fuel`, `uranium`, `wood`, `charcoal`, `ethanol`) e `batteries`.

### E1 — Dados de potencial e parque instalado
- **Solar e eólica:** mapa de potencial por província, agregado por cidade (interior). Fontes candidatas:
  WorldClim 2.1 `srad` e `wind` (mesmo pipeline do clima, mensal → sazonalidade) ou Global Solar Atlas /
  Global Wind Atlas (mais precisos). Conferir licença antes de usar.
- **Hídrica:** não há mapa de potencial hoje (só rios do Natural Earth para desenho, e
  altitude/precipitação por província). Derivar potencial de precipitação × desnível × presença de rio,
  normalizado pelo potencial técnico nacional de fonte pública.
- **Parque instalado de 2026:** base pública de usinas (ex.: Global Power Plant Database — usina, fonte,
  MW, coordenada) para o estado inicial, atribuída à cidade dona da província.
- Aceite: capacidade instalada e matriz por nação na ordem do dado real; nenhum número inventado.

### E2 — Usinas e geração
- Usina = construção no **interior** da cidade, com dono (F9), obra física (§15), empregos e custo.
- Fontes: hídrica, solar, eólica (limitadas pelo mapa); carvão, gás, óleo, nuclear, biomassa (consomem
  combustível do mercado — dependem de recurso e importação).
- **Teto por cidade e por fonte** (MW) vindo do mapa; **custo crescente** (melhores locais primeiro).
- Fator de capacidade mensal para solar/eólica/hídrica (sazonalidade e seca).

### E3 — Rede, despacho e preço
- **Rede elétrica como entidade com lista de membros** (padrão igual ao de moeda/bloco): por nação no
  início; interligação entre redes com capacidade limitada.
- Eletricidade não é estocável (exceto baterias/reservatório). Balanço diário por rede.
- Despacho por ordem de custo (a usina mais barata entra primeiro; a última ligada define o preço).
- Falta de energia = racionamento: produção industrial reduzida e queda de satisfação.

### E4 — Demanda
- Famílias por estrato (consumo cresce com a renda), indústria por receita (intensidade energética por
  bem — alumínio, aço, químicos), serviços e infraestrutura urbana.

### E5 — Rede nacional, conexão das cidades e perdas (pedido do dono)
- **Uma rede nacional por nação.** Toda usina construída entra na rede e a energia é compartilhada
  nacionalmente (sem simular fluxo físico cidade a cidade).
- **Cada cidade tem um nível de conexão à rede** (capacidade em MW + perda %). Usina só injeta e cidade só
  retira até a capacidade da conexão; cidade sem conexão fica ilhada (só a própria geração).
- **Três estados por cidade (aglomeração + interior):** (1) **conectada** — participa do compartilhamento
  nacional; (2) **conexão parcial** — troca com a rede só até a capacidade da linha, o resto é local;
  (3) **isolada** — rede regional: só as usinas da própria cidade/interior abastecem a região, com preço
  e racionamento próprios. Dentro da região, a cobertura do interior é um nível separado.
- **Perda nunca chega a zero (decisão do dono):** obras de rede reduzem a perda com retorno decrescente até
  um piso técnico > 0 (referência: perdas T&D das redes mais eficientes, ~4–6%; `access_losses.json`).
- **Infraestrutura elétrica reduz perdas**, com dois níveis por cidade: urbano e interior. Estado inicial
  por dado público (perdas de transmissão/distribuição % e acesso à eletricidade % por país).
- **Venda entre nações só entre redes vizinhas com FRONTEIRA TERRESTRE** (sem cabo submarino — decisão
  do dono). Exige obra de interligação com capacidade. A vizinhança é derivada do raster de
  donos de província e recalculada quando o território muda.
- **Exportação = só o excedente da rede NACIONAL (decisão do dono).** Vende-se ao vizinho apenas o que
  sobra no balanço da rede nacional depois de atender as cidades conectadas. Geração de região
  isolada ou acima da capacidade da conexão parcial **não entra** no excedente exportável: uma mega
  usina fora da rede nacional não exporta nada (só abastece a própria região). A interligação
  internacional parte da rede nacional, nunca de uma região isolada.

### E7 — Serviço de utilidades: distribuição de energia e água (pedido do dono)
- **Novo setor de serviço** (21º em `services.json`): distribuidora de energia elétrica + água/esgoto.
  É a "última milha": leva a energia da rede (E5) e a água até famílias, empresas e prefeitura.
- **Dono pelo sistema da F9:** estatal, privado nacional ou estrangeiro; privatizar e estatizar valem
  como para qualquer grupo. Tarifa regulada pelo governo (alavanca) ou livre.
- **Tudo depende dele:** toda construção (fazenda, mina, fábrica, serviço, moradia) e toda família
  consome energia e água via distribuidora. Falta de cobertura/capacidade = produção reduzida,
  satisfação menor e, no caso da água, saúde pior.
- **Cobertura por cidade em dois âmbitos** (urbano e interior), com investimento da distribuidora;
  estado inicial por dado público (acesso à eletricidade %, acesso a água potável % por país).
- **Perdas** da distribuição (E5) ficam na conta da distribuidora; investir reduz.
- A água é só serviço de distribuição (sem hidrologia nem estoque de água nesta fase).

### E6 — Modo de mapa "Energia"
- Mesmo sistema de símbolos do mapa de recursos (`build_resource_map.py`, `marker_budget.json`):
  marcadores por cidade para potencial hídrico, solar e eólico, e para o parque instalado por fonte.
- O parque instalado real (E1) é a base para popular a rede elétrica mundial inicial.

**Alavancas:** construir/estatizar usina, subsídio por fonte, imposto sobre carbono, interligação com
vizinhos, racionamento, estoque estratégico de combustível, expandir conexão e reduzir perdas.

---

## PARTE F — Infraestrutura de transporte (depois da Parte A; independente de B, C e E)

Já existe: frota por modo (caminhão/navio/avião) e porto com capacidade, fila, nível e expansão (só no
núcleo, sem painel). A ideia é generalizar o mesmo mecanismo do porto.

- **Um tipo genérico de infraestrutura** (capacidade, fila, nível, obra de expansão), com as espécies:
  porto (naval), aeroporto (aéreo), malha rodoviária (terrestre), ferrovia, conexão elétrica (E5).
- **Nível por cidade, em dois âmbitos:** urbano e interior. No interior, a estrada reduz o custo de acesso
  ao mercado da cidade (B3).
- **Aéreo:** o nível do aeroporto limita quantos aviões a cidade opera por dia.
- **Terrestre:** o nível rodoviário limita quantos caminhões a cidade despacha/recebe por dia.
- **Naval:** continua pelo porto.
- **Ferrovia (decisão do dono — efeito pequeno, só para existir):** multiplicador leve na carga por
  caminhão nas rotas terrestres; sem veículo nem malha própria. Fica como um número ajustável.
- Aceite: gargalo aparece (fila) quando o nível é baixo; expandir resolve; dinheiro/balanço consistente.

---

## Fase futura — Blocos e organizações (com a diplomacia)

- Entidade genérica "organização" com lista de membros e módulos ligáveis: livre comércio interno,
  tarifa externa comum, moeda comum (aponta para uma moeda), livre circulação de pessoas, orçamento
  comum, defesa comum, rede elétrica comum. UE, zona do euro, Mercosul, USMCA e franco CFA viram dados.
- Entrar/sair = trocar a lista de membros + regra de transição (conversão de moeda a taxa fixa,
  redenominação, troca de tabela de tarifas com prazo).
- Hoje a tarifa é par a par (`DemoTariff` comprador|vendedor|produto), sem entidade de bloco.

---

## PARTE D — Validação por padrões reais

Além de "dinheiro/balanço consistente" e "sem fome", cada fase de B e C adiciona testes de padrão que
devem **emergir**, não ser ancorados:

- fração da renda em comida cai com a renda;
- desemprego sobe quando o PIB cai;
- comércio cai com a distância e cresce com o tamanho das economias;
- aluguel em ~20–35% da renda urbana;
- taxa de poupança maior nos estratos altos e na meia-idade;
- preço/lucro das ações em faixa plausível.

## Requisitos de toda fase (B, C, E, F)

Uma fase só fecha com os quatro itens (painel/ação do jogador fica para DEPOIS — decisão do dono):

1. **IA usa o sistema novo** (constrói usina, expande infraestrutura, toma crédito, migra). Sistema que
   só o jogador aciona deixa as outras 178 nações paradas.
2. **Dado inicial de 2026** com fonte pública registrada; sem número inventado.
3. **Migração de save** e round-trip sem diferença.
4. **Teste de padrão real** (Parte D) e sweep 365 d dentro da tolerância.

Regra de calibração: **uma recalibração grande por vez** (A6, B3, E, C2 não se sobrepõem).

## Marcos

- **M1:** Parte A completa (benchmark, CLI, IA em C++, cadência).
- **M2:** Parte B (faixas etárias, estratos, interior, migração) — mundo com 8,2 bi.
- **M2b:** B6 (patrimônio da região, junto com o desenho da C1) e B7 (informalidade, serviços informais, mortalidade, insatisfação).
- **M3:** Parte F (transporte) e Parte E (energia).
- **M4:** Parte C (bancos, crédito, hipoteca, bolsa).
- Em cada marco: parar, rodar a bateria longa e atualizar o `GAME_STATE.md` antes de seguir.

## Execução

- Cabeça (Claude) coordena; execução por agentes Flash em worktrees paralelos, com Flash diferente
  verificando cada entrega. Pro só onde o protocolo exigir parecer de núcleo.
- Paralelizável: A2 ∥ A3 depois de A1; coleta de dados da B1 (ONU) em paralelo com toda a Parte A.
- Não paralelizável: A4 → A5 (IA depende das consultas e dos comandos); C1 → C2 → C3 → C4 → C5.

## Decisões do dono (2026-11-17)

1. 5 estratos de renda e 5 faixas etárias.
2. Cadência por sistema (A6) aprovada, com recalibração dos goldens.
3. IA em C++ (A5): portar os três investidores de uma vez.
4. População fora das aglomerações: **inflar as cidades** (cidade = aglomeração + região, com o segmento
   "interior" produtivo desde a primeira versão) — não haverá bloco agregado por nação.
5. **Fora do escopo:** detalhe geográfico dentro do interior. O interior é **um bloco por cidade**;
   não haverá camada de dados por província nem redesenho das províncias neste plano. Províncias
   seguem como células internas de dado (terra, clima, jazidas) e, no futuro, controle de território.
