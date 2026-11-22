# Icon Forge — ícones do OpenGS sem LLM

Gerador determinístico (Python puro, sem dependências) dos ícones de matéria-prima.
Os ícones são **descritos** em `resource_icons.json` com primitivas SVG; o script aplica o
estilo da casa e escreve SVGs brancos 24×24 que o Godot tinge (`modulate`).

> **Papel atual (2026-11):** a arte ativa dos BENS é o PNG ilustrado aprovado em
> `assets/icons/illustrated/` (gerado por `tools/icon_gen`, backend OpenAI). O SVG
> do Icon Forge continua no jogo como **fallback** de `ProductIcons` enquanto um id
> não tiver PNG, e como conjunto ativo dos **20 setores de serviço** em
> `assets/icons/services/`. Para bem novo, gere o PNG ilustrado primeiro; o SVG só
> é necessário se o PNG faltar.

```powershell
python tools/icon_forge/icon_forge.py check                                   # valida as regras
python tools/icon_forge/icon_forge.py build --out assets/icons/resources      # gera os SVGs
python tools/icon_forge/icon_forge.py sheet --out docs/ui/icons_sheet.html    # folha de revisão (humano)
python tools/icon_forge/icon_forge.py new cobalt                              # esqueleto de um ícone novo
```

## Como um agente (DeepSeek) desenha um ícone novo

1. **Metáfora**: escolha UM objeto físico reconhecível que represente o recurso
   (ferro = bigorna, cobre = bobina de fio, ouro = lingotes, urânio = trevo de radiação,
   madeira = tora). Evite letras/símbolos químicos e evite repetir a silhueta de outro ícone.
2. **Silhueta primeiro**: 1 forma principal `fill` ocupando ~60–80% da área 2..22.
   O ícone precisa ser reconhecível em 16 px só pela silhueta.
3. **No máximo 3 detalhes**, sempre como recorte (`cut` para linhas, `cutfill` para furos/sementes).
   Nunca use outra cor: só existe branco + transparente.
4. **Contornos** (`stroke`, 1.8) só quando o objeto é naturalmente linear (bobina, lâmpada, caule).
5. **Grade**: coordenadas em múltiplos de 0.5; centro em (12,12); área segura 1.5..22.5.
   Para simetria radial use `rot: [0,120,240]` em vez de copiar caminhos.
6. Rode `check` (tem de dar 0 problemas) e depois `build`. Em seguida rode a captura de UI
   do jogo para conferir no contexto.

## Primitivas de camada

| campo | valores |
|---|---|
| `el` | `path` (`d`), `circle` (`cx,cy,r`), `ellipse` (`cx,cy,rx,ry`), `rect` (`x,y,width,height,rx?`), `polygon` (`points`) |
| `mode` | `fill` · `stroke` · `cut` · `cutfill` |
| `w` | largura do traço (padrão 1.8 em `stroke`, 1.2 em `cut`; mínimo 1.0) |
| `rot` | lista de ângulos para repetir girando em torno de `origin` (padrão 12,12) |
| `transform` | transform SVG livre (ex. `rotate(-30 8 9.5)`) |

## Exemplo

```json
"lead": {"label": "Chumbo", "color": "#6B7480", "layers": [
  {"el": "path", "d": "M8.5 9h7l3.5 11H5z", "mode": "fill"},
  {"el": "path", "d": "M9.5 9V7a2.5 2.5 0 0 1 5 0v2", "mode": "stroke"}
]}
```

`color` é a cor do recurso no mapa (usada no chip da legenda e na folha de revisão);
`label` é o nome pt-BR para a folha. Os ícones são arte própria do projeto (sem licença de terceiros).
