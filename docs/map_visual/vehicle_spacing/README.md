# Separação de caminhões e aviões — evidência

Implementação em `map/convoy_spacing.gd` (extensão de `TradeConvoys`; o jogo e
os testes instanciam essa extensão). O núcleo econômico em `core/economy/` não
foi alterado.

## O que mudou

- **Caminhões 33% menores:** comprimento de 0,6875 u → **0,460625 u**
  (`0,6875 × 0,67`). Navio (0,6875 u) e avião (0,55 u) seguem iguais.
- **Faixas:** caminhões usam 3 faixas laterais de 0,16 u; aviões usam 3 camadas
  de altitude (0,07 u) + 0,22 u de offset lateral.
- **Fila:** caixa direcional ao longo da rota (0,52 u no caminhão, 0,62 u no
  avião) impede empilhar no mesmo ponto. Empates exatos são desfeitos pelo id
  do nó, então dois veículos que nascem juntos se separam.
- **Custo:** índice espacial uniforme (célula 0,8 u), consulta 3×3 — sem varrer
  todos os pares. O navio mantém o empurrão radial curto que já existia.
- A faixa do caminhão é descartada se cair no terreno errado; o veículo nunca é
  empurrado para a água.

## Números medidos

| medição | antes | depois |
|---|---|---|
| `vehicle_spacing_test` — 24 caminhões no mesmo ponto | — | **0,606 ms/frame** |
| `vehicle_spacing_capture --bench=1` — frame 1600×900, 24 comboios | — | **4,24 / 4,25 / 4,35 ms** (média/p95/máx) |
| `perf_day_peak` — pump por frame (média/p95/máx) | 1,56 / 1,67 / 5,68 ms | **1,56 / 1,67 / 4,87 ms** |

O pico da virada do dia no `perf_day_peak` continua dominado pelo tick C++
síncrono (~80 ms médios); o caminho real do jogo usa o worker (ver
`GAME_STATE.md` §13.4/13.5).

## Fotos

- `before/` — script-base (`--legacy=1`): caminhão no tamanho antigo, sem faixas.
- `after/` — implementação nova.
  - `road_trucks_close_d009.png` / `road_trucks_d013.png`: par de caminhões real
    mais próximo do dia 2 (6,33 u entre eles; o mapa não desenha estradas).
  - `truck_scale_d003.png`: close de 3 u para comparar a escala do caminhão
    com o `before/truck_scale_d003.png`.
  - `port_ship_d013.png` / `port_ship_d026.png`: navio a 2,34 u do porto de
    partida (Batumi) e a saída do porto.

A captura usa pedidos reais da economia (nada de comboio sintético). Devido ao
LOD de comércio (pedidos internacionais viram navio), o dia 2 tem 4 caminhões e
20 navios; os caminhões ficam espalhados, então o par mais próximo é o único
enquadramento honesto de "estrada". O teste headless cobre o caso denso
(24 caminhões no mesmo ponto).

## Comandos

```powershell
$godot = "C:\Users\artur\OneDrive\Área de Trabalho\Godot_v4.7.2-stable_win64.exe"

& $godot --headless --path . res://tools/vehicle_spacing_test.tscn
& $godot --path . res://tools/vehicle_spacing_capture.tscn -- --run=before --tag=before --legacy=1 --days=1
& $godot --path . res://tools/vehicle_spacing_capture.tscn -- --run=after --tag=after --days=2 --bench=1
```
