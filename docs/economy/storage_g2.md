# G2 — armazenagem, reserva nacional, IPC e IA de armazém (D-101)

> Parte **G2** de `docs/plans/perf_pop_financas_plano.md` (dados em D-100,
> `docs/data/storage_g2.md`). Branch `feat/g2-storage`; save **v25**.
>
> Escopo: ponte `core/economy/storage_service.gd` → núcleo C++
> (`sim/storage.cpp`, `sim/ai/ai_storage.cpp`), o pulmão região→cidade, a
> reserva nacional do dia 1, a IA privada/governo de armazém, o IPC na camada
> nacional (leitura) e o save v25.
>
> Este documento registra a **decisão de escala** (D-101), o que é dado e o que
> é modelo, e as medições de 60/365 d, goldens e CLI×Godot. Nenhum número aqui
> é inventado: o que não foi medido aparece como não medido.

---

## 1. Decisão de escala (D-101)

O volume do jogo **ainda não está na escala real** — o mesmo problema já
resolvido para os portos (D-080). Aplicar a tonelagem real como absoluta
produzia capacidade sem sentido para o volume do jogo (grãos com dezenas de
meses de cobertura: **89 meses** no país-sonda).

Regra aplicada (decisão do coordenador):

```
capacidade_jogo = cobertura_real (meses) × fluxo_jogo (t/ano) / 12
reserva_jogo    = cobertura_real (dias)   × fluxo_jogo (t/ano) / 365
```

- **cobertura_real** = capacidade ou estoque real ÷ produção/consumo real do
  país — a grandeza que **independe** da escala.
- **fluxo_jogo** = produção anual do jogo daquele país e tipo: as fazendas e
  minas iniciais que a geografia já semeia (mesma base do rateio entre
  cidades, `output_tons_per_day × 365`).
- Quando a produção do jogo for calibrada para a escala real, os números
  absolutos coincidem.
- **Ouro do BC** fica em toneladas reais: não é fluxo de produção do jogo; o
  cofre é estrutura nesta parte (D-100). Lido e reportado, não semeado.

## 2. O que é dado, o que é modelo

| tipo | cobertura (dado) | sem dado → regra declarada |
|---|---|---|
| silo (grãos) | `grain.months_of_production` (7 nações observadas; 163 pela regra `k = 1,1007 × produção anual` do próprio arquivo) | 2 meses (`kStorageInitialMonths`) — 9 nações sem capacidade publicada |
| câmara fria | **sem base**: o dado é m³ observados (17 nações) e a regra é m³/habitante; não existe produção real de perecíveis por nação no repositório | 2 meses (o m³ real continua reportado em `real_data.cold_m3`) |
| tanque (privado) | **sem base** publicada de capacidade da indústria | 2 meses |
| tanque (governo) | `oil.days_net_imports` (39 nações; Eurostat/EIA) | 0 — o arquivo diz "reserva inicial = 0" |
| gás | capacidade de trabalho TWh ÷ produção real (`extraction_production_2024`, kt → TWh a 70,5) | 0 (regra do arquivo: sem capacidade documentada o jogo começa com 0) |
| pátio de granéis | **sem base** publicada | 2 meses |
| armazém geral | **sem base** publicada | 2 meses (na prática 0: nenhum bem de armazém geral é produzido por fazenda/mina inicial) |
| reserva de grãos (pública) | estoque público real ÷ **consumo real** do país (`crop_stocks`, USDA PSD) = dias | sem consumo publicado → reserva não escalada (`reserve_unscaled`) |
| ouro | toneladas reais (IMF IFS) | — |

Modelo declarado na repartição: **25 % da capacidade é do governo**
(`kStorageGovernmentShare`), exceto onde o dado dá a rede pública (silo:
Índia/Indonésia/Filipinas; tanque: reserva pública). Rateio nacional→cidade
pelo peso de produção do tipo (→ população → igual).

**Sem fluxo do jogo não há capacidade**: a ausência é explícita (capacidade 0)
e a IA privada constrói depois pela regra de payback. O relatório publica
`flow_zero_nations` (quantas nações têm cobertura mas nenhum fluxo do jogo
naquele tipo) e `reserve_unscaled` (reserva real sem denominador de fluxo —
30 nações com estoque público de petróleo e sem produção de óleo no jogo).

## 3. Números medidos no mundo real (849 cidades, 179 nações)

StorageService aplicado no dia 0 (`sites = 3.973`, `113/113` produtos
mapeados):

| tipo | capacidade (t) | fluxo do jogo (t/ano) | cobertura | fonte |
|---|---:|---:|---:|---|
| silo | 540.953.687 | 478.369.911 | **13,57 meses** | 540,23 Mt do dado + 0,72 Mt da regra |
| câmara fria | 4.201.327 | 25.207.962 | 2,00 meses | regra |
| tanque | 353.371 | 983.336 | 4,31 meses | 225,6 kt do dado (governo) + regra 2 meses (privado) |
| gás | 33.378 | 360.074 | 1,11 meses | dado (capacidade ÷ produção 2024) |
| pátio | 2.685.492 | 16.112.951 | 2,00 meses | regra |
| armazém geral | 0 | 0 | — | sem fluxo |
| **total** | **548.227.254** | | | |

No esquema anterior (tonelagem absoluta) o mesmo mundo dava **4.515.919.399 t**
em **6.068 sites** — 8,2× a capacidade atual e cobertura de dezenas de meses
nos grãos. Agora são **3.973 sites**: só existe armazém onde o jogo tem fluxo
daquele tipo (ver lacuna 6).

Reserva nacional do dia 1 (aplicada sobre o fluxo do jogo):

- **petróleo público**: 73.916 t (EUA, 125 dias de importação líquida do EIA);
- **grãos públicos**: 7.813.081 t (Índia/Indonésia/Filipinas; 93,9 dias do
  consumo real agregado);
- **ouro do BC**: 32.839,7 t lidas, **não** semeadas (cofre é estrutura).

Magnitude real lida e reportada (`real_data`): 3.559,4 Mt de grãos,
1.346,6 hm³ de câmara fria, 4.392,9 TWh de gás, 122.634 kt de capacidade e
237.471 kt de estoque público de petróleo.

## 4. Mecânica no núcleo

`phase_storage` roda depois de `phase_agriculture` e antes da produção:

1. **Pulmão região→cidade**: a colheita do dia que tem silo regional privado é
   desviada do mercado para o estoque do silo (até a capacidade livre);
2. **Perdas**: perecibilidade diária (prateleira do bem; câmara fria sem
   energia ×3) e perda mensal de armazenagem agravada pela qualidade do
   armazém (B5);
3. **Manutenção**: dono paga `kStorageMaintenancePerTonDay[tipo] × capacidade`
   ao setor de serviços (dinheiro conservado);
4. **Escoamento do pulmão**: `inflow / kStorageLungReleaseDays` (60 d) volta ao
   mercado da cidade-polo, pagando o custo de acesso da estrada (B5) ao
   transporte — dinheiro conservado.

Comandos: `storage_trade` (compra paga / liberação paga / socorro gratuito) e
`storage_build` (obra de 90 d, custo `kStorageCostPerTon[tipo]`, vira receita
da construção). Nada de preço novo: a armazenagem muda o **volume ofertado**
(D-091/D-092 seguem donos do preço).

Leitura nacional (`get_storage_status().nations`): estoque público e privado
por produto, dias de consumo coberto (`government_days_cover`), **IPC** (razão
dos preços da cesta ponderada pelos quintis) e preço médio por produto. Não
existe mercado nacional — só a leitura agregada.

## 5. IA de armazém

- **Privada**: constrói quando o spread observado (máx-mín do histórico de
  preço) pagaria a obra em 6 anos; compra na colheita (preço ≤ 0,98 ×
  referência) e vende perto da próxima safra (≥ 1,03 × referência); libera
  comida de graça se a satisfação local cair abaixo de 0,85.
- **Governo**: compra até 10 % do **excedente** do mercado (`Market::surplus`
  = estoque acima de 3 dias de demanda) com preço ≤ 0,95 × referência na
  colheita; libera 20 % do guardado com preço ≥ 1,05 × referência na
  entressafra, **libera onde falta** (satisfação < 0,95, sem pagamento) e dá
  **socorro** (sem pagamento) abaixo de 0,80.
- **Piso de segurança alimentar (`kStorageFoodSecureSatisfaction` = 0,95,
  D-101)**: vale para toda decisão que RETIRA comida do mercado. Satisfação =
  consumido/necessidade do dia, o mesmo número que a IA lê. Sem ele, a reserva
  do governo comprava justamente onde faltava comida (ilhas pequenas: ATG, DMA,
  GRD, KNA, LCA) e o portão de comida piorava.
- **Pulmão**: cidade abaixo do piso **não entrega a colheita** ao silo — a
  safra do dia vai direto ao mercado. A leitura é a satisfação de ontem
  (`satisfaction_prev`), porque o `phase_reset` do dia zera o campo corrente.
  Era o que fazia a ilha perder a safra por 60 dias dentro do silo privado.

**Resultado em 365 d**: 5.923 t de capacidade de silo construídas (sites
existentes; 0 site novo). Com a cobertura real de grãos em 13,6 meses, a
capacidade inicial já supera o alvo da IA (2 meses), então ela quase não tem o
que construir — comportamento esperado, não bug.

## 6. Save v25 e migração

- Bloco novo `storage` (sites: cidade, tipo, dono, capacidade, estoque por
  produto, pulmão). **Aditivo**: v24 → v25 mantém tudo e o bloco nasce vazio;
  o `StorageService` reaplica a capacidade do **dado** (`only_missing`), sem
  inventar tonelada.
- `StorageService`, no load, roda com `only_missing: true` e **sem semente** de
  reserva: o que veio no save manda (inclusive o que a IA construiu).
- Efeito nos goldens S0: `status_sha256` **idêntico**; só o `save_sha256` muda,
  pelo bloco vazio v25 (ver §8).

## 7. Medições de 365 dias (mundo real, IA, `threads=1`)

`tools/g2_world_ai_365.gd`, 365 d, 849 cidades, 179 nações. Evidências:
`docs/economy/perf/g2_world_ai_365_d101.json` (1ª rodada, antes das regras de
comida) e `docs/economy/perf/g2_world_ai_365_d101_final.json` (final; o arquivo
`_rerun.json` segue sendo o artefato da D-097).

| métrica | base D-099 | G2 (1ª rodada) | **G2 (final)** |
|---|---:|---:|---:|
| falências | 2 | 2 | **2** (1 cobre, 1 ferro; 0 com plano de safra) |
| portão de comida (< 0,975) | 84/179 | 91/179 | **85/179** |
| mediana do portão | 0,9832 | 0,9740 | **0,9798** |
| pior nação | PSX 0,2038 | SLB 0,1991 | SLB 0,1594 |
| drift de dinheiro | +0,000811 | +0,002589 | **+0,002208** (limite 0,01) |
| violações de invariante | 0 | 0 | **0** |
| hash da trajetória | `80d7dc1c.` | `80d7dc1c.` | **`80d7dc1c.`** (igual) |

Evidência final: `docs/economy/perf/g2_world_ai_365_d101_final.json`; a segunda
execução (`_final2.json`, mesma build) sai **byte-idêntica** (determinismo).

**A piora do portão (84 → 91) foi fechada em duas regras de comida**, não em
calibração de preço nem em esconder o número:

1. **Reserva só do excedente, e só com comida segura** (§5): o governo deixou
   de comprar do estoque total do mercado e passou a comprar do `surplus`
   (estoque acima de 3 dias de demanda), apenas com satisfação ≥ 0,95, e a
   liberar o guardado onde falta. 91 → 88.
2. **Pulmão não desvia safra de cidade abaixo do piso** (§5): a colheita de
   quem está com déficit vai direto ao mercado. 88 → 85.

**Resultado honesto: 85/179, um acima da meta de 84.** As nações que ainda
entraram no portão nesta rodada são NOR, CRI, DMA, PER, GRD, LCA e MEX (7), e
saíram MDA, PRK, LVA, RUS, LBR e CHL (6). O efeito restante é medido e não é a
compra da reserva: é o **primeiro ano de enchimento do pulmão** — o silo
privado retém ~60 dias de colheita até encher, e num mundo com estoque inicial
de 937,9 Mt o preço não reage (preço quase plano, §7.3), então o grão retido
demora a voltar. As nações importadoras dependentes (MEX, CRI) também perdem
importação quando o preço local cai. O silo privado no dia 365 cai de 90,9 Mt
(1ª rodada) para **59,7 Mt** com a regra nova. Próximo passo declarado (não
calibrado aqui, para não inventar número): **ciclo de estoque** do privado com
liberação mínima por tempo de guarda, e semente do silo privado no estado
estacionário para o ano 1 não pagar o enchimento.

### 7.1 Estoques mês a mês (t)

| dia | silo privado | silo governo | tanque governo | pátio privado |
|---:|---:|---:|---:|---:|
| 30 | 7.901.330 | 7.476.005 | 73.729 | 0 |
| 60 | 19.192.877 | 7.121.491 | 73.601 | 0 |
| 90 | 38.980.440 | 6.805.363 | 73.561 | 0 |
| 120 | 49.194.390 | 6.507.762 | 73.544 | 0 |
| 150 | 54.153.994 | 6.246.553 | 73.544 | 0 |
| 180 | 58.632.466 | 5.989.414 | 73.544 | 1 |
| 210 | 75.262.772 | 5.939.011 | 73.544 | 1 |
| 240 | 72.922.034 | 6.208.145 | 73.544 | 2 |
| 270 | 63.450.434 | 6.026.732 | 73.544 | 14 |
| 300 | 75.841.228 | 5.755.179 | 73.544 | 46 |
| 330 | 81.670.450 | 5.434.062 | 73.544 | 104 |
| 360 | 62.898.090 | 5.127.649 | 73.544 | 169 |
| 365 | 59.659.785 | 5.080.226 | 73.544 | 178 |

A reserva pública de petróleo cai de 73.916 t (dia 1) para 73.544 t no ano
(manutenção/liberações do governo): 372 t, 0,5 %.

### 7.2 IPC por país (amostra, dia 365)

| país | IPC | estoque governo (t) | estoque privado (t) | capacidade (t) |
|---|---:|---:|---:|---:|
| USA | 1,0105 | 72.605 | 13.543 | 12.698.733 |
| BRA | 1,0105 | 0 | 17.095 | 7.863.009 |
| CHN | 1,0314 | 0 | 0 | 95.701.217 |
| IND | 1,0100 | 4.647.648 | 13.458.823 | 33.613.387 |
| FRA | 1,0089 | 939 | 387.009 | 3.804.892 |
| NGA | 1,0214 | 1.999 | 1.911.442 | 10.956.915 |
| ARG | 1,0089 | 0 | 0 | 2.610.215 |
| DEU | 1,0089 | 29.397 | 686.732 | 4.324.780 |
| JPN | 1,2937 | 0 | 205.548 | 4.020.131 |
| RUS | 1,0089 | 25.821 | 3.110.591 | 34.587.145 |
| IDN | 1,0913 | 0 | 928.145 | 10.591.651 |
| MEX | 1,1082 | 0 | 993.883 | 9.636.958 |

O estoque privado no dia 365 é **59,7 Mt** (era 90,9 Mt antes da regra do
pulmão): as cidades abaixo do piso alimentar deixaram de entregar a colheita ao
silo, então o grão fica no mercado. A IA construiu **5.252 t** de capacidade
(0 site novo: a cobertura inicial de 13,6 meses já supera o alvo de 2 meses).

### 7.3 Sazonalidade de preço por hemisfério (razão ao preço-base, 13 amostras de 30 d)

| cultura | norte (mín–máx) | amplitude | sul (mín–máx) | amplitude |
|---|---|---:|---|---:|
| trigo | 1,0196 (m6) – 1,0418 (m10) | 2,2 % | 1,0603 (m6) – 1,0800 (m9) | 1,9 % |
| arroz | 1,0349 (m3) – 1,0642 (m4) | 2,8 % | 1,0000 (m1) – 1,0426 (m7) | 4,3 % |
| milho | 1,0000 – 1,0000 | 0,0 % | 1,0000 – 1,0000 | 0,0 % |
| soja | 1,0241 (m8) – 1,0354 (m9) | 1,1 % | 1,0000 – 1,0000 | 0,0 % |
| batata | 1,0000 – 1,0000 | 0,0 % | 1,0000 – 1,0000 | 0,0 % |

A sazonalidade **continua pequena** (o mesmo diagnóstico da D-097): o estoque
inicial de 937,9 Mt satura o mercado no primeiro ano e o preço fica perto da
base. A armazenagem criou uma oscilação nova no **trigo do sul** (3,05 %, antes
0,5 %) e no **arroz do sul** (3,76 %), mas nada comparável a uma entressafra
real. Milho, soja e batata não têm preço amostrado fora da base. A conclusão
segue a da D-097: enquanto o estoque do dia 1 não for recalibrado, a
sazonalidade não aparece na média do ano; a sonda sem estoque inicial é o
instrumento para medir o mecanismo.

## 8. Testes, goldens e equivalência

- **`tools/storage_g2_test.gd`**: **114 verificações verdes** (rodado pelo
  `tools/run_tests.ps1`) — serviço (113/113 produtos, cobertura × fluxo, reserva
  por dias, ouro não aplicado, `only_missing`), conservação de dinheiro e de
  bens (compra/liberação/socorro/obra), capacidade limitando compra e liberação,
  pulmão (janela de safra no dia 1, A/B com e sem silo, **só em cidade com
  comida segura**), IA privada construindo com spread e caixa, IPC nacional,
  save v25 + migração v24→v25 e o bloco (8) da regra nova: **ilha faminta não
  perde comida para o silo do governo** (45 d com satisfação < 0,95: 0 compra,
  silo sem crescer) e **a reserva libera onde falta** (100 t → mercado em 10 d,
  sem pagamento). O helper de satisfação do teste lia o padrão 1,0 (a ficha da
  cidade não tem o campo solto, tem o bloco `pop`); corrigido, o cenário (5)
  virou um mundo em paz de verdade (PORTUS menor) e a checagem de "não causar
  fome" passou a valer contra a mesma cidade **sem** IA.
- **A0 60 d (IA + urbano), 2 rodadas**: idênticas (0 diferenças). Golden
  regenerado: `save 925c3096. → 8b8be710.`, `status 40203cc9. → db07f756.`
  (o mundo muda por desenho: 3.973 sites de armazém; `service_businesses`
  39.841 → 39.959).
- **S0 (`economy_refactor_equiv`)**: `real_60_ai`, `real_60_noai` e
  `demo_365_noai` — **`status_sha256` idêntico** ao golden anterior em todos;
  só o `save_sha256` muda, pelo bloco `storage` vazio do v25. Goldens
  regravados.
- **CLI × Godot, 10 d (IA, `--threads=1`, sem urbano)**: **byte-idênticos**,
  78.223.424 bytes, SHA-256
  `23E7D0F3446E39F98A3F6802AF24151DB5BFCCF30A64CECC9596D05F1EFD3786`,
  0 diferenças no comparador, 0 violações, `money_total` 6.523.551.982,90.
- **Bateria completa**: ver `GAME_STATE.md` (D-101) — o número de verdes/
  vermelhos e as falhas herdadas ficam registrados lá.

## 9. Lacunas declaradas

1. **Câmara fria sem cobertura real**: falta produção real de perecíveis
   (carne/laticínios) por nação; hoje é a regra de 2 meses. O m³ observado
   (17 nações) está no relatório para quando a base existir.
2. **Reserva pública de petróleo sem fluxo em 30 nações**: o estoque real
   existe, mas o país não produz óleo no jogo para escalar os dias
   (`reserve_unscaled`).
3. **Portão de comida**: 84 → **85** nações abaixo de 0,975 (era 91 antes da
   regra). A meta de 84 **não foi atingida por 1 nação**. Causa restante
   medida: o primeiro ano de enchimento do pulmão (o silo privado retém ~60
   dias de colheita até encher) e a queda de importação das nações
   importadoras quando o preço local cai. Próximo passo declarado: ciclo de
   estoque do privado (liberação mínima por tempo de guarda) e semente do silo
   privado no estado estacionário — nenhum dos dois calibrado aqui.
4. **Sazonalidade** segue pequena no primeiro ano (estoque inicial de 937,9 Mt);
   é a mesma dívida da D-097, não um efeito da armazenagem.
5. **Consumo do jogo**: a reserva é escalada pela **produção** do jogo porque
   não existe consumo declarado no dia 0; quando houver leitura de consumo
   (o status já publica `demand`), a reserva pode usar o consumo.
6. **Fluxo só de fazenda/mina inicial**: a capacidade de um tipo só existe
   onde as fazendas/minas iniciais produzem aquele bem. Por isso a câmara fria
   aparece em poucas cidades (4 fazendas de batata no mundo; `flow_zero_nations.cold
   = 177`) e a carne — que nasce em planta de processamento depois do
   `StorageService` — ainda não gera capacidade de frio. Próximo passo: incluir
   a cadeia de processamento no fluxo do tipo (`s_annual` da IA e a base do
   `StorageService`).

