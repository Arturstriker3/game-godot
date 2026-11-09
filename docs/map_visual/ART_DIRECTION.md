# WarEconomy — Direção artística e refinamento visual Grand Strategy

Data: 2026-11-09 · Godot 4.7.2 (Forward+, Vulkan, RTX 2060) · Capturas 1600×900

> **Nenhuma imagem foi inspecionada visualmente.** Não há modelo com visão nesta sessão (`VISION_*` ausente). Todos os números abaixo vêm de métricas de pixels, contagem de nós/draw calls, benchmark e leitura de código. A aprovação estética final exige desenvolvedor ou modelo multimodal.

## 1. Viabilidade da abordagem 2.5D

O projeto já tem as peças; nada foi reconstruído:

| Elemento | Situação | Uso nesta fase |
| --- | --- | --- |
| Base geográfica (satélite + relevo) | `satellite.jpg` 8192×4096 + `normal.png` | mantida |
| Relevo | hillshade no shader (normal map) | mantido |
| Cidades 3D modulares | `UrbanGrowthManager` + `UrbanRenderer` (MultiMesh near/far) + biblioteca de 20 módulos | **integrado ao mapa**, sob demanda |
| Perfil industrial | `IndustryProfile.derive(city)` | já derivado do status real |
| Fazenda/mina/indústria/porto | `assets/models/{farm,mine,industry,port}.glb` | existem, **ainda não usados** no mapa |
| Fronteiras | `country_borders.png` + shader | inalteradas |
| Modos de mapa | novo `MapModeLayer` | 6 modos sobre a mesma base |

Decisão: o terreno continua plano com relevo por normal map (sem deslocamento de vértices). Isso preserva o alinhamento exato com a camada política e o sistema territorial, como pedido.

## 2. Escala e níveis de zoom

Regras implementadas em `map/city_world.gd` (distância da câmera à cidade):

| Faixa | Rótulo | Marcador | Modelo compacto | Cidade modular |
| --- | --- | --- | --- | --- |
| > 92 u (continental) | sim, ~14 px fixos | sim | não | não |
| 30–92 u (regional) | sim | sim | sim (fade 66→92) | não |
| 26–30 u | sim | fade 26→34 | não | sim |
| < 26 u (local) | sim | não | não | sim |

- Rótulos com tamanho de tela constante: `pixel_size = 0.0008 × distância` (≈14 px em qualquer zoom). Resolve a ilegibilidade apontada no relatório anterior.
- Cidade modular construída com distância ≤ 30 u e liberada em > 44 u (histerese), sem tocar em save/load.
- Medição real no manifesto: cidades modulares ativas = **5 em d=013**, **3 em d=026**, **0 em d=052 e d=120**.

## 3. Integração das cidades ao terreno

- Modelo compacto reduzido para **1,5 unidade** e assentado em **y = 0,16** (a camada política fica em 0,13): cabe dentro do marcador (1,7 de diâmetro), sem aparência de peça solta.
- No zoom local o marcador (três discos, a "base" visível) desaparece por fade e dá lugar à cidade modular — sem base quadrada aparente.
- Cidade modular em escala **0,55**, sobre ruas e lotes do próprio `UrbanRenderer` (telhados, árvores, praça, distrito industrial). Paris (estágio 2) fica com ~6,6 unidades ≈ 260 km — estilização explícita, típica de Grand Strategy, documentada para não parecer erro de escala.
- Posições continuam vindo de `city["x"]/["y"]` (projeção real); nenhuma coordenada foi alterada.
- Perfil industrial vem de `IndustryProfile.derive`, que já lê produção, industrialização, renda e estoque — nenhuma economia paralela.

## 4. Identidade visual

Herdada do refinamento anterior e mantida consistente:

- Oceano em azul-ardósia com profundidade por luminância, saturação 0,94 → 0,51 e rampa raso/médio/profundo.
- Terra dessaturada (0,59 → 0,45) com leve aumento de brilho; relevo por hillshade (MAE 0,0095–0,0139, 3–5% dos pixels).
- Sem glow, sem brilho especular, sem ícones grandes; marcadores só aparecem a partir de 26 u.
- Paleta dos modos limitada a azul-ardósia → latão/verde/âmbar/oliva (baixa saturação).

## 5. Modos de mapa

`map/map_mode_layer.gd` + `map/map_shaders/map_modes.gdshader`: um único plano sobre a base desenha halos de calor a partir das 5 cidades. Trocar de modo só altera uniformes — **nenhuma geometria é recriada**. Os valores são recalculados uma vez por dia simulado em `Economy.status_updated`.

| Modo | Fonte real | Normalização |
| --- | --- | --- |
| Geográfico | — | sem overlay |
| Político | `nation_fill.png` + `country_borders.png` | preenchimento 0,11 / bordas 0,55 |
| Populacional | `city.population` | log(1+p) / log(1+max) |
| Econômico | `city.production` | produção / máxima |
| Industrial | `city.industrialization` | 0..1 direto |
| Recursos | `GeographyService` (aptidão de trigo + depósitos) | `0,75·aptidão + 0,08·min(depósitos,4)` |

Validação objetiva (d=052, comparação pixel a pixel contra o modo anterior): populacional MAE 0,067 (SSIM 0,898, 42% dos pixels); econômico 0,038; industrial 0,026; recursos 0,020. Todos alteram a imagem de forma mensurável, mantendo a leitura do terreno (brilho do populacional 0,325 → 0,390).

## 6. Fronteiras territoriais

- Algoritmo e textura **intactos**. Ajuste apenas de renderização: `border_opacity` 0,55 no político e 0,42 nos modos de dados; `fill_opacity` 0,11 / 0,028.
- Achado da inspeção: `nation_fill.png` tem apenas **0,25% dos pixels com alfa** e 4 cores (NED ausente). O modo político hoje lê quase só pelas fronteiras. Corrigir isso é regenerar a textura de preenchimento — fora do escopo desta fase para não tocar no algoritmo territorial.

## 7. Comparação com o mapa 2D anterior

Mesmo enquadramento (pitch −56°, FOV 75°), capturas `docs/map_visual/baseline_before/legacy2d_*`:

| Enquadramento | 3D atual | 2D histórico |
| --- | --- | --- |
| d=120 | brilho 0,344 · sat 0,429 · nitidez 0,0081 · bordas 0,133 | brilho 0,676 · sat 0,407 · nitidez 0,0030 · bordas 0,079 |
| d=052 | brilho 0,327 · sat 0,485 · nitidez 0,0089 · bordas 0,227 | brilho 0,683 · sat 0,429 · nitidez 0,0008 · bordas 0,029 |
| d=013 | brilho 0,435 · sat 0,368 · nitidez 0,0052 · bordas 0,069 | brilho 0,622 · sat 0,550 · nitidez 0,0001 · bordas 0,005 |

Leitura: o 3D tem 3–40× mais alta frequência e 2–14× mais bordas (detalhe real), porém é ~50% mais escuro e mais contrastado. O 2D é chapado e claro. **Reaproveitado do 2D**: `height.png`/`normal.png` (relevo), a lógica de preenchimento político e a leitura plana dos rótulos. **Não descartado**: o 3D continua sendo a base, com o 2D servindo de referência de legibilidade.

## 8. Desempenho

Benchmark com câmera parada, vsync desligado (mesmo método da fase anterior):

| Distância | Frame | Draw calls (antes → depois) | VRAM |
| --- | --- | --- | --- |
| d=120 | 4,24 ms / 236 fps | 64 → **31** | 483,4 MB |
| d=052 | 4,24 ms / 236 fps | 177 → 177 | 483,4 MB |
| d=026 | 4,24 ms / 236 fps | 174 → **406** | 484,5 MB |
| d=013 | 4,24 ms / 236 fps | 240 → **530** | 485,2 MB |

O custo sobe no zoom local por causa da cidade modular (MultiMesh por arquétipo), mas o tempo por frame não mudou (limite é CPU). No continental o LOD **reduz** draw calls à metade. Streaming evita manter as 5 cidades modulares vivas quando não estão em uso.

## 9. Validação e limites

- Cena independente: `tools/map_capture.tscn` (parâmetros `--set`, `--mode`, `--tag`, `--run`, `--bench`).
- Capturas desta fase: `docs/map_visual/art_after/` — 6 modos × 4 distâncias (24 PNGs) + `manifest_map3d_political.json` com câmera, LOD e benchmark.
- Métricas: `docs/map_visual/art_after/metrics.json` e `metrics.md`.
- **Sem inspeção visual.** Nada aqui afirma se “parece bonito”; afirma o que foi medido.
- Validação humana sugerida: comparar, no mesmo zoom, `map3d_geographic_d052.png` × `map3d_population_d052.png` × `map3d_political_d052.png`; e `map3d_political_d013.png` para julgar a cidade modular (escala, encaixe no terreno, distrito industrial).

## 10. Próximos passos priorizados

| Prioridade | Ação | Motivo |
| --- | --- | --- |
| P1 | Validação visual humana das 24 capturas | única forma de aprovar estética e escala das cidades |
| P1 | Regenerar `nation_fill.png` (preenchimento hoje em 0,25%) | modo político depende quase só das bordas |
| P2 | Aumentar `country_borders.png` para 8192×4096 | nitidez no zoom local (9× de ampliação atual) |
| P2 | Usar `farm/mine/industry/port.glb` como instalações no zoom regional | assets já existem; falta vínculo visual com recursos |
| P3 | Botões de modo no HUD | hoje os modos são acionados por `CityWorld.set_mode`/captura |
| P3 | Relevo geométrico opcional (deslocar vértices e assentar cidades na altura) | exigiria ajustar a camada política junto |

Restrições respeitadas: economia, mercado, população, recursos, prospecção, crescimento urbano, sistema territorial e save/load não foram alterados — o mapa apenas lê o status e desenha.
