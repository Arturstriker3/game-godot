# F9 — investimento e propriedade: sweep de 365 d (5 variações)

Comando (DLL debug, 15/11/2026):

```powershell
& $godot --headless --path . --script res://tools/economy_world_sweep.gd -- `
  --days=365 --baseline=res://docs/economy/perf/f4_world_ai_365.json `
  --out=res://docs/economy/perf/f9_invest_world_sweep.json
```

Comparação dupla, como pedido:

- **contrato (baseline `f4_world_ai_365.json`, tolerância 2,5 p.p.)** — é o
  critério que o script aplica;
- **baseline vigente da F9 (`f9_world_sweep.json`, medido antes do investimento
  por grupo)** — mede o que esta camada mudou.

## Resultado

| critério | resultado |
| --- | --- |
| variações (real, real+seed1, real+seed2, half, xgs) | 5/5 rodaram, mesmo número de falências em todas |
| nações dentro da tolerância (vs `f4`) | **178/179** |
| nações fora (vs `f4`) | **MRT 0,9516 (-4,8 p.p.)** — já fora na F9 vigente (0,9515, -4,9 p.p.); item aberto de modelo |
| falências de construções | **16** (limite 28; baseline F4 26) |
| dinheiro | conservado (`money_total` idêntico, drift 0,0028) |
| negócios de serviço (informativo) | 37.982 abertos / 217.757 falências em todas as variações |

## O que mudou vs. a F9 vigente (pior caso por nação)

Melhoras grandes (a camada de grupos devolve lucro ao estrato dono como renda
de capital e o investimento abre capacidade onde o retorno paga):

| nação | F9 vigente | agora | delta |
| --- | --- | --- | --- |
| PAK | 0,519 | 1,000 | +48,1 p.p. |
| SDN | 0,651 | 1,000 | +34,9 p.p. |
| SOL | 0,593 | 0,973 | +38,1 p.p. |
| DJI | 0,343 | 0,472 | +12,8 p.p. (sai da lista de falhas) |
| UZB | 0,905 | 1,000 | +9,5 p.p. (sai da lista de falhas) |
| SGP | 0,906 | 1,000 | +9,4 p.p. |
| MRT | 0,9515 | 0,9516 | +0,02 p.p. (inalterado) |

Movimentos para baixo, todos **dentro** da tolerância do contrato vs `f4`:

| nação | F9 vigente | agora | vs `f4` |
| --- | --- | --- | --- |
| LBY | 0,9753 | 0,9302 | -1,7 p.p. (limite -2,5) |
| PSX | 0,1855 | 0,1665 | -1,9 p.p. (limite -2,5) |
| MNG | 0,9950 | 0,9925 | -0,2 p.p. |

Nenhuma outra nação caiu mais de 0,01 p.p. O resto do mundo ficou igual (as
demais 168 nações variam só em ruído de 1e-14).

## Diagnóstico dos movimentos

- **LBY/PSX**: nenhuma remoção de comida pelo investimento — `in_kind_food` de
  LBY é 0 e o de PSX é 13,8 t-eq (piso de subsistência intacto). O que muda é o
  **roteamento do lucro**: parte do lucro que ficava na firma local passa ao
  grupo (imposto no anfitrião + dividendo/retenção) e volta como renda de
  capital do estrato dono. Em importadores com caixa familiar curto (LBY-BEN é
  a pior cidade, 0,759; PSX-GAZ 0,162) a chegada do dividendo não compensa o
  capital de giro que sai da firma no mesmo dia. Continua acima do piso de
  aceitação; fica registrado para o A/B de modelo (mesma lacuna de
  renda/tesouro de importador já documentada na F3/F8).
- **MRT**: inalterado em relação à F9 vigente; a causa medida é o piso de
  subsistência em espécie (A/B `f9_world_inkind0_365.json`: MRT 0,954 sem o
  piso, UZB 0,896 e DJI 0,413) — trade-off de modelo, não desta camada.
- **Falências**: 16 em todas as variações (baseline F4 26, limite 28). O
  investimento não fecha firmas: a única falência nova possível seria ativo de
  grupo — não houve nenhuma (as 16 são construções do mundo, como antes).

## Repro

Relatórios por variação em `user://world_sweep/{real,real_seed1,real_seed2,half,xgs}_365.json`.
O agregado comparável é `docs/economy/perf/f9_invest_world_sweep.json`.
