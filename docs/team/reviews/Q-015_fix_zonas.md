# Q-015 / frente C — capacidade das zonas urbanas (RES/COM/IND)

- **Branch:** `fix/q015-zonas` (base `balance/global-resources` @ `a8d7984`), worktree `G:/projetos/opengs-wt-w1`.
- **Data:** 2026-11-17. **Dono:** `@gerente-simulacao`. Build: debug incremental da worktree.
- **Pedido:** levantar a condição **B4** do parecer Q-015 — com a população urbana corrigida (D-004), a
  cidade nasce vazia (ocupação RES mediana 0,167), o preço da terra não sobe, a prefeitura nunca
  precisa expandir e `NPL-POK` segue com ocupação 1,375 (>1 não deveria existir).
- **TL;DR:** a causa é estrutural, não um bug de uma constante: o FAR {1; 2,5; 6} é o **envelope do
  lote**, mas era aplicado como se a **cidade inteira** fosse construída no envelope. O envelope dava
  4–36× o estoque real (2.719 m² por domicílio em Nova York para "encher" a zona). A capacidade RES
  agora nasce calibrada no alvo de ocupação da faixa de população urbana (com o envelope preservado
  para expansão/adensamento), a unidade informal sai da conta da zona formal e nenhuma cidade passa de
  0,95. COM/IND: a ocupação baixa **não é erro de contagem** — é a escala da amostra de firmas do
  núcleo (evidência no §6).

## 1. Ferramenta de auditoria (objetivo 1)

Novo runner headless, sem abrir cena:

```powershell
$godot = "C:\Users\artur\OneDrive\Área de Trabalho\Godot_v4.7.2-stable_win64.exe"
& $godot --headless --path . --script res://tools/urban_zone_audit.gd -- `
    --tag=before --out=user://urban_zone_audit/q15c_before.json
```

- Carrega o mundo real (834 cidades) **como o autoload** (`demo_set_urban` com os 4 JSON do §19), semeia
  a indústria de referência (`demo_seed_industry`, que o núcleo só chama no 1º tick) e lê
  `demo_get_urban_city` de todas as cidades: uso, capacidade e ocupação das três zonas.
- Imprime p10/p50/p90 por faixa de **população urbana** (`<300 mil`, `300 mil–1 M`, `1–5 M`, `>5 M`),
  contagem de cidades com alguma zona >0,90 e >1,00, e o retrato das 6 cidades do sweep. Grava uma
  linha por cidade em JSON (antes/depois).
- Antes desta ferramenta, a foto de dia 0 do runner de mundo **não contava a indústria que existe**:
  `demo_seed_industry` roda no começo do 1º tick, então `zone_usage(IND)` do dia 0 era 0 por ausência
  de carga, não por não caber. A auditoria agora semeia antes de ler (ID do achado: **IND 0,000 era
  leitura cedo, não conta errada**).

## 2. Diagnóstico: por que a cidade nascia vazia

Capacidade "de envelope" (dia 0, antes) contra o estoque carregado da fonte:

| Cidade | capacidade RES (envelope) | estoque | razão | m²/domicílio p/ encher o envelope |
| --- | ---: | ---: | ---: | ---: |
| BRA-SAO | 80.007.520 | 6.858.006 | 11,7× | 875 m² |
| JPN-TOK | 197.529.200 | 17.725.322 | 11,1× | 836 m² |
| NGA-LAG | 26.606.160 | 4.699.571 | 5,7× | 425 m² |
| IND-MUM | 26.782.560 | 6.415.525 | 4,2× | 313 m² |
| USA-NEW | 329.054.000 | 9.076.305 | 36,3× | 2.719 m² |
| AUS-WEI | 2.750 | 1.110 | 2,5× | 248 m² |
| NPL-POK | 102.690 | 141.188 | 0,73× | — (por isso 1,375) |

O domicílio médio que o dado sustenta é ordens de grandeza menor: `household_size` da fonte
(UN DESA) × m² por pessoa da faixa de renda (UN-Habitat/OECD, 8–40 m²/pessoa) ≈ 37 m² (Lagos) a
100 m² (Nova York). Para "encher" o envelope de Nova York cada domicílio precisaria de **2.719 m²**.
Ou seja: o erro era tratar o FAR máximo do lote como FAR bruto da cidade — em qualquer metrópole real a
maior parte do solo construído é rua, pátio, uso misto e construção antiga, e a média realizada fica em
0,2–0,8, não em 2,5–6,0.

Distribuição medida (antes), 834 cidades:

| Faixa (pop. urbana) | n | RES p10 / p50 / p90 |
| --- | ---: | --- |
| <300 mil | 252 | 0,150 / **0,408** / 0,485 |
| 300 mil–1 M | 169 | 0,156 / **0,403** / 0,482 |
| 1–5 M | 314 | 0,099 / **0,144** / 0,173 |
| >5 M | 99 | 0,039 / **0,057** / 0,177 |
| todas | 834 | 0,087 / **0,167** / 0,457 · >1,0: **1** · >0,90: 1 |

## 3. O que mudou (constantes e função)

| Constante / regra | Antes | Depois | Fonte / confiança |
| --- | --- | --- | --- |
| `kDemoUrbanFar[3]` (FAR por densidade) | {1,0; 2,5; 6,0} | **idem** (comentário corrigido: é o envelope do LOTE) | Padrões de gabarito (Atlas/OECD); parâmetro de modelo, não medição |
| `kDemoUrbanUnitM2[3][3]` | {60;100;180}/{55;85;140}/{50;75;110} | idem | A fonte publica aluguel/preço por m², não por unidade |
| `kDemoUrbanServiceM2PerWorker` | 25 m² | idem | OECD (espaço de escritório 15–35 m²/pessoa) |
| `kDemoUrbanFactoryM2PerWorker` | 150 m² | idem | Parques industriais 100–200 m²/pessoa |
| **`kDemoUrbanBandOccupancy[4]`** (novo) | — | **{0,55; 0,70; 0,72; 0,85}** por faixa | **CALIBRAÇÃO declarada** (confidence: calibração) — "quão cheia" não é observável por cidade; alvo reproduz a hierarquia do mercado de solo |
| **`kDemoUrbanOccupancyCap`** (novo) | — | **0,95** | Regra geral: capacidade ≥ estoque ÷ 0,95 (NPL-POK); nenhuma cidade >1,0 sem inventar área |
| **`CityLand.zone_fill[3]`** (novo, save) | — | calculado no load: estoque **formal** ÷ (envelope × alvo) | Derivado dos dados acima; 1,0 em save antigo/cenário (comportamento anterior) |

Por que calibrar em vez de só trocar FAR/m²: nenhuma tabela única de FAR por gabarito fecha as quatro
faixas ao mesmo tempo — Nova York (7.478 km² para 22,6 M) e Mumbai (531 km² para 28,1 M) estão na mesma
classe e precisam de FAR realizado 0,16 vs 0,70. A calibração é o **último recurso**, marcado, e só
define o nível inicial: a heterogeneidade continua na capacidade por km² (varia por cidade), no preço
do terreno (aluguel/renda locais) e no espaço de crescimento (1/alvo − 1).

Efeito no jogo: expandir uma zona soma `km² × FAR × fill ÷ m²` unidades (São Paulo: ~6.900 unidades por
km² novo) e adensar multiplica o envelope (LOW→MID ×2,9; MID→HIGH ×2,7, já contando o m² de cada
gabarito). O envelope e a regra de custo crescente não mudaram.

**Unidade informal (objetivo 4) — confirmado e declarado:** `informal_units` é subconjunto do estoque
carregado e sua área vai para `informal_km2`, derivada à parte; `area_km2[zone]` vem de
`built_up × zoning_split` e **não** inclui a área informal. Além de confirmar, `zone_usage(RES)` passou a
**descontar** `informal_units`: a ocupação mede só moradia formal (a informal mora em `informal_km2`).

## 4. Resultado no dia 0 (objetivo 2)

| Faixa (pop. urbana) | n | RES antes p10/p50/p90 | RES depois p10/p50/p90 | alvo do pedido |
| --- | ---: | --- | --- | --- |
| <300 mil | 252 | 0,150 / 0,408 / 0,485 | **0,550 / 0,550 / 0,550** | 0,4–0,7 ✅ |
| 300 mil–1 M | 169 | 0,156 / 0,403 / 0,482 | **0,700 / 0,700 / 0,700** | 0,6–0,85 ✅ |
| 1–5 M | 314 | 0,099 / 0,144 / 0,173 | **0,720 / 0,720 / 0,720** | 0,6–0,85 ✅ |
| >5 M | 99 | 0,039 / 0,057 / 0,177 | **0,850 / 0,850 / 0,850** | 0,75–0,95 ✅ |
| todas | 834 | 0,087 / 0,167 / 0,457 | 0,550 / 0,700 / 0,850 | — |

- Cidades com alguma zona **>1,0: 1 → 0**; **>0,90: 1 → 0**. `NPL-POK`: 1,375 → **0,700**.
- **Limitação declarada:** a ocupação do dia 0 é degenerada dentro da faixa (p10 = p50 = p90) porque o
  alvo é a calibração e não existe medição de "quão cheia" por cidade. A variação aparece na capacidade
  por km² e, no jogo, no preço e no crescimento.

As 6 cidades do sweep (RES, dia 0):

| Cidade | antes | depois |
| --- | ---: | ---: |
| BRA-SAO | 0,086 | 0,850 |
| JPN-TOK | 0,090 | 0,850 |
| NGA-LAG | 0,177 | 0,850 |
| IND-MUM | 0,240 | 0,850 |
| USA-NEW | 0,028 | 0,850 |
| AUS-WEI | 0,404 | 0,550 |
| NPL-POK | 1,375 | 0,700 |

## 5. COM e IND (objetivo 2, segunda parte) — investigação

Ocupação no dia 0 **não mudou** (COM p50 0,000; IND 0,000) e isso é a resposta da investigação:

1. **IND = 0,000 não é conta errada.** `demo_seed_industry` (30 plantas de referência em 6 cidades)
   é chamado no começo do 1º tick, não na carga. A ferramenta antiga lia antes da semeadura — corrigido
   na auditoria (semeia antes de ler). Nas 6 cidades do probe **não existe fábrica urbana alguma**, nem
   no dia 0 nem em 365 d: IND = 0 é o retrato fiel.
2. **COM = 0,000 no dia 0** porque o mundo semeia **1 negócio de serviço por cidade com 481 empregos**
   (833/834 cidades). A zona COM física comporta 111,7 M trabalhadores em São Paulo (465 km² × FAR 6 ÷
   25 m²); o emprego setorial da fonte (ILO, `sector_employment.json`) é 3,72 M — e o negócio modelado
   ocupa 481. Em 365 d os negócios crescem e a ocupação COM chega a 0,012 (São Paulo) / 0,237 (Mumbai).
3. **Conclusão:** a amostra de firmas do núcleo é ordens de grandeza menor que o emprego setorial; a
   zona COM/IND física nunca vai encher com ela. Não é bug de contagem — é escala de modelo. Dimensionar
   COM/IND pelo emprego setorial (ou modelar as firmas na escala da cidade) mexe no gate de abertura de
   negócio e na criação de firmas (fora do escopo declarado desta frente: `services.cpp`/decisão de
   obra). **Recomendação ao cabeça**, com os números acima.

## 6. Impacto em 365 d (objetivo de medição)

Comando único (`--threads=1`, DLL debug da worktree):

```powershell
& $godot --headless --path . --script res://tools/economy_world_test.gd -- --ai=1 --days=365 `
  --urban=1 --threads=1 --fx=real `
  --baseline=res://docs/economy/perf/construction_world_sweep.json `
  --out=user://world_sweep/q15c_real_365.json `
  --urban-cities=BRA-SAO,JPN-TOK,NGA-LAG,IND-MUM,USA-NEW,AUS-WEI `
  --urban-out=user://q15c_urban_365.json
```

| Métrica | antes (`q15c_before_real_365`) | depois (`q15c_real_365`) |
| --- | ---: | ---: |
| Falências de construção | 6 | **5** (limite 28) |
| Dinheiro (inicial 2.200.539.587,40) | 2.200.539.587,40019 → drift +0,0002 | 2.200.539.587,39874 → **drift −0,0013** (tol. 0,01) |
| Serviço (falências / negócios) | 7.083 / 53.324 | **6.912 / 53.462** (sem travamento) |
| Comida: nações abaixo de −2,5 p.p. | 2 (AFG −11,29; SOL −3,45) | **1 (SOL −4,00)**; DJI −0,92; MNG −0,78 |

Comida: **SOL −4,00 p.p.** contra `construction_world_sweep.json` — na classe de importador sem
agricultura que o dono já aceitou (JOR/YEM/SOL/DJI/SAH), mas acima da tolerância de 2,5 p.p. AFG, que
antes caía 11,29 p.p. contra o mesmo baseline, fecha o run em 1,000. Não instrumentei a causa dessa
diferença (é efeito indireto de aluguel/caixa urbano sobre a POP); fica registrada como medição, não
como explicação. Nenhuma outra nação passa de −1 p.p.

As 6 cidades em 365 d (ocupação RES/COM/IND · preço da terra RES XGS/m² · obras):

| Cidade | RES | COM antes → depois | IND | preço antes → depois | obras |
| --- | ---: | ---: | ---: | ---: | ---: |
| BRA-SAO | 0,841 | 0,012 → 0,012 | 0,000 | 0,58 → **5,14** | 0 |
| JPN-TOK | 0,844 | 0,005 → 0,004 | 0,000 | 0,33 → **2,13** | 0 |
| NGA-LAG | 0,841 | 0,087 → 0,087 | 0,000 | 0,52 → **4,55** | 0 |
| IND-MUM | 0,846 | 0,236 → 0,237 | 0,000 | 0,36 → **2,93** | 0 |
| USA-NEW | 0,843 | 0,001 → 0,001 | 0,000 | 0,65 → **3,61** | 0 |
| AUS-WEI | 0,546 | 0,048 → 0,000 | 0,000 | 1,91 → **2,50** | 0 |

- **Preço da terra RES subiu 4–9×** (o sinal de ocupação agora existe; antes a pressão era 0,03–0,24).
- **Ocupação RES cai devagar** (0,85 → 0,84): o estoque deprecia 1%/ano e **nenhuma unidade é
  construída** — é o bloqueio **B1** da revisão Q-015 (moradia não abre obra), de outra frente.
- **Obras da prefeitura: 0** — o gatilho é 0,90 e a cidade agora nasce a 0,85 (antes, 0,06): o gatilho
  ficou ao alcance (6% de crescimento), mas continua intocável com B1/B3 de pé. B3 (`committed_cost` ×
  caixa) está explicitamente fora do meu escopo.

## 7. Determinismo, save e testes

- **Threads 1 × 8:** 60 d com `--threads=1` e `--threads=8` produzem relatórios **byte-idênticos**
  (SHA256 `BF6A848B7CABC60D957CF421CF612DE4040C29B8E9CA9FAADE5580F4B155B701`).
- **Save:** campo aditivo `zone_fill[3]` no bloco `urban`; save antigo (ou cenário sintético) carrega
  **1,0** = comportamento anterior. `urban_land_test` valida round-trip v14 idêntico. Nenhuma versão
  de save subiu (campo opcional com default).
- **Goldens:** os hashes de save/status mudam (campo novo no bloco urbano e ocupação inicial nova). Não
  regenerei nenhum golden — `economy_integration_test` passou sem alteração.
- **Testes rodados (todos exit 0):**
  - `tools/urban_zone_audit.gd` (novo) — antes e depois, 834 cidades.
  - `tools/urban_land_test.gd` — expectativas atualizadas para o alvo calibrado (AGRIA declara 6 M de
    população urbana → alvo 0,85; PORTUS 200 mil → 0,55) e demais asserções (COM/IND, expansão,
    adensamento, save/load, threads) intactas.
  - `tools/economy_integration_test.gd`.
  - `tools/validate_urban_land.py`, `tools/validate_housing_city.py`, `tools/validate_scenario_data.py`
    — exit 0 (dado/JSON inalterados).
  - 365 d com `--urban=1` (§6); 60 d threads 1/8 (§7).
- **Custo:** a mudança é O(1) por cidade no load e uma subtração em `zone_usage`; nenhum laço novo por
  tick. Não medi perf (DLL debug, máquina compartilhada).

## 8. Pendências honestas

1. **Ocupação inicial degenerada por faixa** — é calibração, não medição. Sem fonte de "quão cheia" por
   cidade, não há como distribuir dentro da faixa sem inventar. Alternativa futura: derivar do déficit
   habitacional/vacância por cidade (não existe no dado atual).
2. **COM/IND seguem ~0** — escala da amostra de firmas (§5). Precisa de decisão do cabeça.
3. **B1/B3 seguem de pé** — nenhuma unidade de moradia construída e nenhuma obra da prefeitura em 365 d;
   o gatilho de 0,90 agora é alcançável, mas depende de B1 (construção) para subir.
4. **`NPL-POK`** — área do Atlas 2014 × população 2026; a regra de capacidade ≥ estoque/0,95 resolve a
   ocupação >1 sem inventar área, mas o dado de base continua inconsistente (é do `@gerente-dados`).
5. Relatório de comida: SOL −4,00 p.p. aceito pelo dono por classe, mas acima da tolerância de 2,5 p.p.

## 9. Reprodução

```powershell
# dia 0 (antes/depois)
& $godot --headless --path . --script res://tools/urban_zone_audit.gd -- `
  --tag=depois --out=user://urban_zone_audit/q15c_after.json

# testes
& $godot --headless --path . --script res://tools/urban_land_test.gd
& $godot --headless --path . --script res://tools/economy_integration_test.gd
```
