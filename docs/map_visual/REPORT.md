# WarEconomy — Pipeline de avaliação visual do mapa

Data: 2026-11-09 · Engine: Godot 4.7.2 (Forward+, Vulkan) · Resolução das capturas: 1600×900

## 1. Resumo executivo

| Verificação | Resultado |
| --- | --- |
| Modelo principal (DeepSeek) recebe imagens? | **Não nesta sessão.** Leitura de PNG é recusada pela ferramenta com "Current model cannot read image. Please use a multimodal model". A escolha de modelo do Trae é remota; `settings.json` local não define modelo. |
| MCP Godot captura screenshots? | **Não de forma operacional neste ambiente.** A ponte do `godot-editor-mcp` responde (lê `godot://project/info`), mas ferramentas sem parâmetros não são chamáveis por este cliente e várias falham na validação do schema. As ferramentas `editor_screenshot`/`runtime_screenshot` pertencem ao `godot-mcp-toolkit`, cujo addon **não está instalado** no projeto (porta 6550 fechada). |
| Pipeline de captura automatizada | **Funcional.** `tools/map_capture.tscn` gera 20 PNGs em 4 níveis de zoom, 2 modos e 3 variantes de mapa, com `manifest_*.json` contendo câmera e resolução. |
| Comparação 2D × 3D | **Executada** com câmera idêntica para o mapa 2D histórico e o 3D atual (pitch −56°, FOV 75°, distâncias 120/52/26/13) e com um baseline plano usando os mesmos assets. |
| Inspeção visual por modelo | **Não realizada.** Nenhum modelo com visão processou as imagens. Todas as conclusões abaixo vêm de métricas de pixels, leitura de código e cálculo de câmera. |

Credenciais de visão: `VISION_API_BASE`, `VISION_API_KEY`, `VISION_MODEL` **não estão configuradas** (`vision_status: "sem credenciais VISION_*"`).

## 2. Verificação do suporte multimodal

Evidências coletadas:

1. Leitura direta de `docs/economy_panel/panel.png` retornou: *"Current model cannot read image. Please use a multimodal model"*.
2. `%APPDATA%\Trae\User\settings.json` não contém configuração de modelo, provedor ou visão — a seleção é feita no servidor do Trae.
3. O pipeline (`tools/vision_review.py --vision`) reporta `visual_inspection: false` e `vision_status: "sem credenciais VISION_*"`.

Conclusão: **o modelo principal desta sessão não processa imagens**. O pipeline já está pronto para um modelo multimodal compatível com a API OpenAI; basta configurar as variáveis de ambiente. O DeepSeek continua sendo o modelo principal de programação — a camada visual é externa e opcional.

## 3. Verificação do MCP Godot

| Teste | Resultado |
| --- | --- |
| `mcp_godot-mcp-toolkit` `discover_tools` | OK (servidor responde) |
| `mcp_godot-mcp-toolkit` `game_start` | Falha: `CONNECT_FAILED — connect ECONNREFUSED 127.0.0.1:6550` |
| `addons/` do projeto | Contém apenas `godot_mcp` (do `godot-editor-mcp`); o addon do toolkit não existe |
| Porta 6550 | Sem listener, mesmo com o editor e uma segunda instância abertos |
| `mcp_godot-editor-mcp` `godot_read_resource(godot://project/info)` | OK — ponte viva, Godot 4.7.2 |
| `mcp_godot-editor-mcp` `godot_health_check` | Responde, mas o conteúdo estruturado não bate com o schema declarado (servidor/addon com versões divergentes) |
| Ferramentas sem parâmetros do editor-mcp | Não chamáveis por este cliente: `arguments` vazio vira string e é rejeitado (`expected record, received string`) |

Conclusão: **captura via MCP não é confiável aqui**. O caminho suportado é a captura por CLI, que não depende de plugin, funciona com a GPU real e permite controlar câmera e zoom de forma determinística.

## 4. Pipeline de captura

Arquivos:

- `tools/map_capture.gd` + `tools/map_capture.tscn` — captura da viewport.
- `tools/vision_review.py` — métricas objetivas e ponte opcional para modelo multimodal.

Conjuntos (`--set`):

| Conjunto | O que captura |
| --- | --- |
| `map3d` | Mapa atual (`CityWorld`): satélite + camada política + cidades 3D |
| `flat` | Baseline plano com os mesmos shaders/texturas, câmera ortográfica (sem perspectiva nem cidades) |
| `legacy2d` | Mapa 2D histórico (`editor/editor.tscn` → `map/map.tscn`), UI oculta, mesma câmera |
| `all` | `map3d` + `flat` |

Modos (`--mode`): `political` (padrão) e `geographic` (sem a camada de fronteiras/preenchimento).

Comandos usados:

```powershell
godot --path . res://tools/map_capture.tscn -- --set=all --mode=political --tag=before --run=baseline_before
godot --path . res://tools/map_capture.tscn -- --set=all --mode=geographic --tag=before --run=baseline_before
godot --path . res://tools/map_capture.tscn -- --set=legacy2d --mode=political --tag=before --run=baseline_before
```

Saída: `docs/map_visual/<run>/` com PNGs de 1600×900, `manifest_<set>_<mode>.json` (câmera, FOV, distância, resolução, engine, data) e `metrics.json`/`metrics.md`.

Enquadramento fixo: pitch −56°, FOV 75°, distâncias 120/52/26/13 unidades; centro = média das cidades (mesmo procedimento de `game/main_game.gd`). Análise comparativa: `tools/vision_review.py --compare A B --vision`.

## 5. Capturas e comparação

20 imagens em `docs/map_visual/baseline_before/`: 8 do 3D (2 modos × 4 zooms), 8 planas, 4 do 2D histórico.

Métricas (pixel-metrics, 1600×900):

| imagem | brilho | contraste | saturação | nitidez (Lap.) | bordas |
| --- | --- | --- | --- | --- | --- |
| `map3d_political_d052` | 0,398 | 0,608 | 0,777 | 0,0100 | 0,247 |
| `map3d_political_d120` | 0,405 | 0,911 | 0,631 | 0,0090 | 0,147 |
| `map3d_political_d013` | 0,460 | 0,286 | 0,543 | 0,0045 | 0,063 |
| `map3d_geographic_d052` | 0,396 | 0,609 | 0,819 | 0,0106 | 0,260 |
| `flat_political_s250` | 0,324 | 0,835 | 0,854 | 0,0044 | 0,145 |
| `legacy2d_political_d052` | **0,683** | 0,339 | 0,429 | **0,0008** | 0,029 |

Comparações automáticas:

| A | B | MAE | SSIM | Δbrilho | pixels alterados |
| --- | --- | --- | --- | --- | --- |
| `map3d_political_d052` | `flat_political_s250` | 0,301 | 0,184 | +0,074 | 95,9% |
| `map3d_political_d052` | `legacy2d_political_d052` | 0,369 | 0,347 | **−0,284** | 100% |
| `map3d_political_d120` | `flat_political_s500` | 0,326 | 0,230 | +0,016 | 90,9% |

Leitura objetiva (sem julgamento estético): o 3D tem 12× mais alta frequência (nitidez 0,0100 vs 0,0008) e 8,5× mais densidade de bordas que o 2D histórico no mesmo enquadramento, porém é **42% mais escuro** (0,398 vs 0,683). O SSIM baixo entre 3D e plano é esperado: projeções diferentes alteram a geometria pixel a pixel; ele não mede qualidade.

## 6. Problemas encontrados (por evidência)

| # | Problema | Evidência | Tipo |
| --- | --- | --- | --- |
| P-01 | Base sem relevo: o shader do satélite só amostra a textura, sem altura/normal | `satellite_map.gdshader` não tem `height`/`normal`; `map_data/height.png` (5000×2500) existe e não é usado pelo `CityWorld` | código |
| P-02 | Nitidez cai no zoom aproximado | nitidez 0,0045 em d=013 vs 0,0100 em d=052; `country_borders.png` tem 5 px/unidade → 9× de ampliação em d=013; `nation_fill.png` 2,5 px/unidade → 18× | métrica + cálculo |
| P-03 | Rótulos de cidade ilegíveis no zoom padrão | `font_size 30 × pixel_size 0,019` = 0,57 unidade; ~6,4 px de altura em d=052 (distância padrão `CAMERA_DISTANCE = 52`) e ~2,8 px em d=120 | cálculo |
| P-04 | Mapa atual muito mais escuro que o 2D histórico | Δbrilho −0,284 (0,398 vs 0,683) no mesmo enquadramento | métrica |
| P-05 | Fronteiras finas e suaves no zoom próximo | `border_opacity 0.45`, textura de 5000×2500 sobre 1000×500 unidades | código |
| P-06 | Camada política e marcador de cidade se interceptam | overlay em y = 0,13; disco-base do marcador em y = 0,14 com altura 0,10 (faixa 0,09–0,19) | código |
| P-07 | Sem antialiasing configurado | `project.godot` não define `msaa_3d`/TAA; bordas de alto contraste (0,247 de densidade) | configuração |
| P-08 | Escala urbana estilizada e não documentada | modelo de cidade = 2,8 unidades ≈ 112 km de terreno; marcador 1,7 ≈ 68 km | cálculo |
| P-09 | Paleta com ciano saturado dominante | 24% `#0390A2` + 17% `#06616F` em d=052; se o oceano parece natural exige inspeção visual | métrica |
| P-10 | Só existem modos político e geográfico no mapa 3D | `CityWorld` tem apenas satélite + camada política; modo econômico não existe na camada de mapa | código |

## 7. Plano de melhorias priorizado

| Prioridade | Ação | Impacto | Validação |
| --- | --- | --- | --- |
| P0 | Configurar `VISION_*` e rodar `vision_review.py --vision` | Habilita julgamento estético real (cores, iluminação, escala) | `visual_inspection: true` no `metrics.json` |
| P1 | Ativar MSAA 2×/4× e TAA | Reduz serrilhado nas fronteiras (P-07) | densidade de bordas e contagem de pixels de alto contraste antes/depois |
| P1 | Regerar `country_borders.png` em 8192×4096 (ou SDF em runtime) e `nation_fill.png` em 5000×2500 | Bordas nítidas no zoom próximo (P-02, P-05) | nitidez em d=013 deve subir |
| P1 | Rótulos com tamanho adaptativo por distância e prioridade/collision | Legibilidade em d≥52 (P-03) | altura em px calculada ≥ 12 px em todas as distâncias |
| P2 | Relevo discreto com `height.png` (normal/parallax leve no shader) | Aproxima a direção 2.5D pedida (P-01) | manter brilho/contraste na faixa atual; comparar d=120 |
| P2 | Ajustar brilho/contraste e saturação do oceano | Corrige 42% de escurecimento e ciano saturado (P-04, P-09) | brilho alvo 0,50–0,60 e Δbrilho vs 2D < 0,15 |
| P2 | Resolver offset/parallax da camada política e interseção do marcador | Elimina artefatos em ângulos rasos (P-06) | capturas em pitch −56° e −80° sem costura |
| P3 | Modos de mapa como camadas independentes (político, geográfico, econômico) | Requisito de direção visual (P-10) | captura dos 3 modos com `--mode` |
| P3 | Documentar a escala urbana intencional | Evita leitura de erro de escala (P-08) | nota em `DESIGN.md` |

## 8. Limites deste relatório

- **Nenhuma imagem foi inspecionada visualmente.** Não há afirmação sobre estética, naturalidade de cores ou legibilidade percebida que não esteja ancorada em métrica, código ou cálculo.
- As capturas usam a GPU local (RTX 2060, Forward+); resultados de iluminação podem variar em outro renderer.
- O mapa 2D histórico foi capturado pelo `editor/editor.tscn`; a UI foi ocultada, mas o carregamento usa o cache de texturas do editor.
- O pipeline não altera economia, cidades nem cenas do jogo: apenas lê o status e fotografa.

## 9. Como repetir o ciclo antes/depois

```powershell
# depois de qualquer alteração visual, capture com a mesma tag
godot --path . res://tools/map_capture.tscn -- --set=all --mode=political --tag=after --run=baseline_after

python tools/vision_review.py --images "docs/map_visual/baseline_after/*.png" `
  --out docs/map_visual/baseline_after/metrics.json `
  --markdown docs/map_visual/baseline_after/metrics.md `
  --compare docs/map_visual/baseline_before/map3d_political_d052.png docs/map_visual/baseline_after/map3d_political_d052.png
```
