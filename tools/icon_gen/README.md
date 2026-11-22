# icon_gen — ícones ilustrados do OpenGS (backend OpenAI)

Gera a arte **ilustrada** dos bens e das construções numa folha 4×4 (16 ícones por
chamada), recorta cada célula, tira o fundo e salva em 128 px e 64 px. É o gerador
canônico da arte aprovada que o jogo usa em `assets/icons/illustrated/`.

```
tools/icon_gen/
├─ openai_grid.py            backend OpenAI (folha 4×4 → 16 PNGs 128/64 + metadados)
├─ gen_batches_v3.py         lote da produção v3 (158 novos ícones, 10 lotes)
├─ batches_v3_production.json  ids/descrições do lote de produção v3
└─ batches_v3_retry.json     reexecução dos ids que voltaram vazios
```

Os PNGs brutos de `assets/icons/illustrated_source/gen_api/<id>/` (v1, v2, v3 e
metadados `.json`), o gasto (`api_spend.json`), a folha de revisão e o venv
**não** são versionados — estão no `.gitignore`. A arte que entra no jogo é
copiada (aprovada) para `assets/icons/illustrated/`.

## Uso

```powershell
python tools/icon_gen/openai_grid.py --dry-run --quality medium   # plano, sem gastar
python tools/icon_gen/openai_grid.py --quality medium             # chamada real
python tools/icon_gen/openai_grid.py --tag v3                     # não sobrescreve lote anterior
python tools/icon_gen/openai_grid.py --refs a.png|b.png           # referências extras de estilo
python tools/icon_gen/openai_grid.py --spend                      # gasto acumulado
python tools/icon_gen/openai_grid.py --rebuild-sheet              # refaz a folha de revisão
python tools/icon_gen/openai_grid.py --serve                      # servidor MCP generate_grid
```

- Estilo vigente: **v3 semi-realista** — materiais e proporções reais, paleta
  dessaturada, luz dura de cima-esquerda, contorno escuro fino para legibilidade
  em 16–32 px.
- A chave vem de `OPENAI_API_KEY` ou do arquivo em `OGS_ICONAPI_KEY_FILE`
  (padrão `G:/projetos/opengs-icons-private/secrets/openai_api_key.txt`); as
  referências de estilo, de `OGS_ICONAPI_REF_DIR`. Nada disso entra no git e a
  chave nunca é logada.
- Orçamento duro de US$ 4,00 acumulados (`api_spend.json`); uma chamada é
  recusada acima disso.
- Sem retry automático: uma tentativa = uma cobrança (a exceção é o fallback de
  fundo transparente, registrado).

## Depois de gerar: aprovação e integração

1. Revisar a folha (a pessoa dona do projeto aprova a variação).
2. Copiar a versão aprovada para `assets/icons/illustrated/<id>.png` e a de 64 px
   para `assets/icons/illustrated/64/<id>.png`. Construções, infraestrutura e
   finanças vão para `assets/icons/illustrated/places/` (e `places/64/`).
3. Registrar a origem em `assets/icons/CREDITS.md`.
4. Rodar a captura de UI (`tools/ui_capture.tscn`) e conferir nitidez em 16, 24 e
   32 px, além dos testes de UI que tocam `ProductIcons`.

## Obsoleto: gerador local (LoRA/Fooocus)

O pipeline local anterior — LoRA `ogsicon` sobre SDXL/Juggernaut, daemon
`worker.py --serve`, branch `feat/icon-mcp` — está **obsoleto**. Ele exigia GPU
local e ~5–16 min por ícone, e a arte aprovada passou a ser a do backend OpenAI
(`openai_grid.py`). O diretório `tools/icon_gen/` deste branch contém apenas o
backend OpenAI; `worker.py`, `server.py`, `guards.py` e `vendor/` não fazem parte
deste conjunto e não devem ser reintroduzidos sem decisão explícita.

O `tools/icon_forge` (SVG determinístico) **não** é obsoleto: ele segue como
fallback dos bens e como arte ativa dos 20 setores de serviço. Ver
`tools/icon_forge/README.md`.
