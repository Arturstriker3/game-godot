# Briefing — Caminho A: base de mapa político "atlas" com cidades 3D

> Entregue ao agente `senior-architect` (Claude Opus 5.5) para produzir um **plano**,
> não uma implementação. O jogo é de 2026; **não** adotar facções/conteúdo da WWII.

## Papel

Você é o arquiteto sênior do OpenGS Game / WarEconomy. Sua tarefa é **planejar**
a adoção do Caminho A: trocar a base visual do mapa de satélite por um **mapa
político chapado estilo atlas histórico**, mantendo a câmera 3D, as cidades 3D e
os veículos existentes.

Leia `GAME_STATE.md` e o código real antes de propor. Não invente; cite arquivos.

## Contexto do jogo

- Godot 4.7 (Forward+), núcleo econômico em C++ (GDExtension `EconomySim`).
- Grand strategy de comércio de trigo, 5 nações/cidades, início em 2026.
- O mapa atual é **satélite 3D**: `map/city_world.gd` monta um `PlaneMesh` com
  `map/map_shaders/satellite_map.gdshader` (`satellite.jpg` + `normal.png`) e
  sobrepõe `political_overlay.gdshader` (`country_borders.png` + `nation_fill.png`).
- Cidades 3D vêm de `core/urban/*`; veículos de `core/economy/trade_convoys.gd`.

## Ativos que existem hoje

- `map/map_data/satellite.jpg` (8192×4096, base atual).
- `map/map_data/country_borders.png` (5000×2500, L8, fronteiras Natural Earth).
- `map/map_data/nation_fill.png` (2500×1250, RGBA, preenchimento das 5 nações).
- `map/map_data/nav_mask.png` (2500×1250, L; 255=terra, 0=água, 128=costa).
- `map/map_data/normal.png` (relevo; hoje ~plano).
- Pipeline: `tools/generate_opengs_map.py`, `tools/build_political_layer.py`,
  `tools/generate_nav_mask.py`.
- Dados naturais: `data/definitions/natural/*.json`.

## O que foi apagado na limpeza e pode ser restaurado

Commit `6f2e45e` (anterior à limpeza) ainda contém:

- `map/map_data/provinces.png` (5000×2500, cor-chave por província);
- `map/map.gd` + `map/map_shaders/map2d.gdshader` + `map3d.gdshader`
  (sistema 2D legado com SDF/JFA de fronteiras);
- `map/map_data/ne2.png`, `height.png` e previews.

Use `git show 6f2e45e:<caminho>` para inspecionar/restaurar. O gerador
`tools/generate_opengs_map.py` também pode regenerar `provinces.png`.

## Objetivo do Caminho A

Que o **modo político** pareça um atlas histórico chapado:

- água lisa (teal), terra com preenchimento por nação;
- fronteiras de país mais grossas e de província/região finas;
- sem textura de satélite nesse modo;
- cidades 3D, marcadores, rótulos e veículos continuam por cima;
- o **modo geográfico** pode continuar satélite (transição limpa entre modos).

## Decisões que o plano deve fechar

1. **Escopo do modo:** o atlas substitui só o modo "político" e mantém o
   "geográfico" satélite (recomendado), ou vira base única?
2. **Fronteiras de província:** restaurar `provinces.png` + shader de borda, ou
   começar só com fronteiras de país? (a referência tem províncias)
3. **Preenchimento dos países:** `nation_fill.png` cobre só as 5 nações; como
   pintar o resto da Europa (neutro/parchment) sem inventar fronteiras?
4. **Paleta:** cores modernas das 5 nações (2026) sobre base parchment; **não**
   usar facções da WWII.
5. **Câmera:** manter a perspectiva 3D atual ou oferecer visão ortográfica
   top-down só no modo atlas? Custo/benefício.
6. **Relevo:** desligar no modo atlas (chapado) ou manter sombreado sutil?
7. **Modos de calor** (`map_modes.gdshader`: população/economia/indústria/recursos):
   garantir legibilidade sobre a base chapada.
8. **Legenda/UI:** precisa de legenda de nações no modo atlas? Reutilizar o HUD.

## Restrições

- Não alterar economia, regras, save/load nem o núcleo C++.
- Não quebrar cidades 3D, LOD, seleção pelo corpo da cidade nem comboios.
- Performance: o modo atlas deve ser igual ou mais leve que o satélite.
- Preservar i18n pt-BR/en/es e os estados vazios honestos.
- Mudanças pequenas e reversíveis; não reescrever `city_world.gd` inteiro sem necessidade.
- Se restaurar arquivos apagados, liste exatamente quais e de qual commit.

## Entregável esperado (somente plano)

1. Decisão de arquitetura para cada um dos 8 pontos, com justificativa.
2. Lista de arquivos a criar/alterar/restaurar (caminhos exatos).
3. Abordagem técnica do shader (composição água/terra/fronteiras/cor por nação;
   uniforms; ordem de blend).
4. Plano de implementação em etapas, cada uma testável isoladamente.
5. Riscos e mitigação (ex.: `provinces.png` ausente, `nation_fill` parcial,
   contraste das fronteiras, custo de regenerar dados).
6. Estimativa de esforço (dias) e critérios de aceite objetivos.
7. O que **não** fazer nesta fase.

## Critérios de aceite (implementação futura)

- Modo político lê como atlas: água chapada, terra por nação, fronteiras nítidas.
- Cidades 3D, rótulos, seleção e comboios continuam funcionando.
- Alternar político ↔ geográfico sem recarregar o mapa nem "piscar".
- `--headless --quit-after 8` sem erros de script; benchmark de cidades dentro do
  orçamento; 60 fps no modo atlas.
- `GAME_STATE.md` atualizado.

## Formato da resposta

Plano objetivo em português, com tabelas e caminhos de arquivo. Comece pela
decisão de escopo (ponto 1) e só depois detalhe o resto. Não edite arquivos nesta
passada; se algo for ambíguo, liste a pergunta em vez de assumir.
