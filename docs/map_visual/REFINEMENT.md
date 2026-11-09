# WarEconomy — Refinamento gráfico do mapa mundial

Data: 2026-11-09 · Godot 4.7.2 (Forward+, Vulkan, RTX 2060) · Capturas 1600×900

> **Nenhuma imagem foi inspecionada visualmente.** Não há modelo com visão disponível nesta sessão (`VISION_*` ausente; o modelo principal recusa imagens). Todas as conclusões abaixo são métricas de pixels, leitura de código ou cálculo. A validação estética final precisa de um humano.

## 1. Escopo preservado

Não foram alterados: simulação econômica, distribuição de cidades, algoritmo de fronteiras, prospecção, população e produção industrial. As mudanças ficaram restritas a shader, importação de textura, configuração de renderização e planos de recorte da câmera.

## 2. Inspeção das texturas

| Textura | Resolução | Importação | Situação |
| --- | --- | --- | --- |
| `satellite.jpg` | 8192×4096, 6,3 MB (JPEG) | antes: VRAM **sem compressão** (`.ctex` de 128 MB) | agora: VRAM comprimida BC1 + mipmaps (42,7 MB) |
| `normal.png` | 5000×2500 | normal map BC5, mipmaps, 15,9 MB | **reaproveitada** do mapa antigo para o relevo |
| `height.png` | 5000×2500 | VRAM comprimida, 8 MB | elevação 8 bits (nível do mar = 135); origem do normal map |
| `country_borders.png` | 5000×2500 (5 px/unidade) | lossless | intacta — algoritmo de fronteiras preservado |
| `nation_fill.png` | 2500×1250 | VRAM | intacta |

Filtragem: todos os samplers do mapa usam `filter_linear_mipmap_anisotropic`; o projeto não definia o nível global de anisotropia (padrão 4×). Mipmaps já estavam ativos. Não há nitidez artificial (sharpen) no shader — evita halos e granulação.

A/B de compressão (mesmo shader, mesmo enquadramento): nitidez 0,00864 (BC1) vs 0,00879 (sem compressão) em d=052 — **diferença < 2%**, então a compressão VRAM foi mantida e economizou **66,7 MB** de VRAM.

## 3. O que mudou

### Shader da base (`map/map_shaders/satellite_map.gdshader`)

- **Oceanos**: substituídos os tons ciano por uma rampa `raso → médio → profundo` (azul-ardósia) derivada da luminância do satélite, com plataforma costeira suave e detalhe batimétrico monocromático. `ocean_saturation` 0,9 remove o excesso de ciano; dither de 1/255 só na água quebra o banding dos gradientes JPEG.
- **Relevo**: `normal.png` amostrado e convertido em hillshade relativo (luz fixa noroeste). Terreno plano fica exatamente em 1,0; encostas variam; piso de ambiente 0,62 evita sombras pretas; teto 1,35 evita montanhas artificiais.
- **Terra**: saturação 0,95, contraste 1,02 e brilho 1,09 — gradação conservadora, sem estourar cores.
- Material continua `unshaded` (cartográfico): leitura constante em qualquer ângulo, sem brilho especular/plástico. A luz direcional da cena continua afetando apenas cidades e objetos 3D.

### Renderização (`project.godot`)

| Chave | Valor | Motivo |
| --- | --- | --- |
| `textures/default_filters/anisotropic_filtering_level` | 3 (8×) | nitidez em ângulos oblíquos |
| `anti_aliasing/quality/msaa_3d` | 1 (2×) | reduzir serrilhado em fronteiras/cidades |
| `anti_aliasing/quality/use_debanding` | true | reduzir banding de gradientes |

### Câmera (`game/player/player.tscn`)

- `near` 0,05 → **0,5** e `far` 4000 → **2000**: melhora ~10× a precisão do depth buffer (a camada política fica 0,13 unidade acima do terreno).
- **FOV mantido em 75°**: a inclinação de −56° dá boa leitura estratégica; reduzir para 65° cortaria ~18% da área visível sem resolver serrilhado (já tratado por MSAA/aniso). Controles e distância inicial (`52`) preservados.

## 4. Antes × depois (mesma câmera: pitch −56°, FOV 75°)

| Enquadramento | Brilho | Saturação | Contraste | Água (hex) | Sat. água | Nitidez terra |
| --- | --- | --- | --- | --- | --- | --- |
| Continental d=120 — antes | 0,405 | 0,631 | 0,911 | `#114B5E` | 0,737 | 0,01206 |
| Continental d=120 — depois | 0,344 | 0,429 | 0,882 | `#1F3138` | 0,466 | 0,01254 |
| Regional d=052 — antes | 0,398 | 0,777 | 0,608 | `#0B6E85` | 0,944 | 0,01614 |
| Regional d=052 — depois | 0,327 | 0,484 | 0,533 | `#253E47` | 0,512 | 0,01557 |
| Intermediário d=026 — antes | 0,422 | 0,661 | 0,346 | `#068598` | 0,956 | 0,00773 |
| Intermediário d=026 — depois | 0,382 | 0,420 | 0,440 | `#26434D` | 0,512 | 0,00647 |
| Cidade d=013 — antes | 0,460 | 0,543 | 0,286 | `#0A8E9F` | 0,928 | 0,00529 |
| Cidade d=013 — depois | 0,439 | 0,369 | 0,404 | `#2B4B55` | 0,497 | 0,00395 |

Comparação pixel a pixel (antes → depois): MAE 0,093–0,140 · SSIM 0,79–0,90 · 44–82% dos pixels alterados. O oceano deixou de ser ciano saturado: saturação da água caiu ~46% e o matiz migrou para azul-ardósia, mantendo gradiente de profundidade.

Terra: luminância praticamente estável (0,445 → 0,454 no regional) e saturação menor (0,594 → 0,451) — menos “colorido artificial”, sem perder estrutura. A nitidez da terra caiu 3,5% no regional e 16–25% nos zooms próximos; o A/B abaixo mostra que a causa é o MSAA, não a textura.

### A/Bs de atribuição

| Experimento | Resultado |
| --- | --- |
| Compressão BC1 × sem compressão | nitidez 0,00864 × 0,00879 (−1,7%) → compressão mantida |
| MSAA 2× × desligado (d=013) | nitidez total 0,00348 × 0,00460; terra 0,00389 × 0,00521 |
| MSAA 2× × desligado (d=052) | terra 0,01494 × 0,01527 (−2,2%) |
| Relevo desligado × ligado | MAE 0,0095–0,0139; 3,2–5,3% dos pixels alterados; contraste local da terra +0,0017 em d=120 |

O MSAA suaviza bordas de 1 px (menos serrilhado), o que reduz a variância do laplaciano — métrica de “nitidez” que penaliza antialiasing. A nitidez **sem** MSAA é idêntica à original no zoom próximo (0,00521 vs 0,00529). Mantivemos o MSAA por priorizar o requisito de reduzir serrilhado; para voltar à nitidez bruta basta `msaa_3d=0`.

## 5. Desempenho (medido com a câmera parada, vsync desligado)

| Distância | Frame antes | Frame depois | Draw calls | VRAM antes | VRAM depois |
| --- | --- | --- | --- | --- | --- |
| d=120 | 4,24 ms (236 fps) | 4,24 ms (236 fps) | 64 | 550,1 MB | 483,4 MB |
| d=052 | 4,24 ms (236 fps) | 4,24 ms (236 fps) | 177 | 550,1 MB | 483,4 MB |
| d=026 | 4,24 ms (236 fps) | 4,24 ms (236 fps) | 174 | 550,1 MB | 483,4 MB |
| d=013 | 4,24 ms (236 fps) | 4,24 ms (236 fps) | 240 | 550,1 MB | 483,4 MB |

Tempo por frame inalterado (o mapa é limitado por CPU, não por GPU), draw calls idênticos e **−12,1% de VRAM**. O shader ganhou uma amostra de textura (normal map), absorvida com folga.

## 6. Problemas remanescentes

| # | Problema | Evidência | Próximo passo |
| --- | --- | --- | --- |
| R-01 | Rótulos de cidade ilegíveis na distância padrão | 0,57 unidade de altura ≈ 6,4 px em d=052; 2,8 px em d=120 | tamanho adaptativo por distância + prioridade de rótulos |
| R-02 | Fronteiras suaves no zoom próximo | textura de 5 px/unidade → 9× de ampliação em d=013 | regenerar `country_borders.png` (algoritmo intacto, só resolução) |
| R-03 | Relevo sutil no continental | 3,2% dos pixels alterados em d=120 | reavaliar com validação humana; `relief_strength` 2,2 → 3,0 se necessário |
| R-04 | Origem do satélite é um JPEG de 6,3 MB | artefatos de compressão em gradientes | substituir a fonte por mosaico de maior qualidade |
| R-05 | Nitidez métrica caiu com MSAA | ver A/B | decisão humana: MSAA on (menos serrilhado) × off (mais variância local) |

## 7. Validação humana recomendada

Compare, nas três distâncias (continental, regional e cidade):

- `docs/map_visual/refine_before/map3d_political_d{120,052,013}.png`
- `docs/map_visual/refine_after/map3d_political_d{120,052,013}.png`

Perguntas objetivas: o oceano parece natural e com profundidade? O relevo é perceptível sem parecer artificial? A leitura do terreno piorou em alguma região? O MSAA compensa a suavização?

## 8. Como reproduzir

```powershell
# captura + benchmark (3 distâncias, 4 no total)
godot --path . res://tools/map_capture.tscn -- --set=map3d --mode=political --run=refine_after --tag=after --bench=1

# métricas e comparações
python tools/vision_review.py --images "docs/map_visual/refine_after/*.png" `
  --out docs/map_visual/refine_after/metrics.json `
  --markdown docs/map_visual/refine_after/metrics.md `
  --compare docs/map_visual/refine_before/map3d_political_d052.png docs/map_visual/refine_after/map3d_political_d052.png
```
