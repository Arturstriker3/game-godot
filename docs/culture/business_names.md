# Nomes de empresas por língua (Fase F8 — serviços)

Base de dados **somente leitura** para batizar os negócios de serviço dos 20
setores da F8. Não está ligada à simulação, ao save/load nem à UI: um sistema
futuro chama `BusinessNameGenerator` e recebe uma string.

## Onde fica

| Caminho | Papel |
|---|---|
| `data/definitions/culture/business_names/<code>.json` | uma língua por arquivo (32) |
| `data/definitions/culture/culture_language.json` | cultura → língua + aproximações documentadas |
| `core/culture/business_name_generator.gd` | gerador determinístico (`BusinessNameGenerator`) |
| `tools/build_business_names.py` | gera os JSON a partir de `tools/business_names_data/*.py` |
| `tools/validate_business_names.py` | valida a base inteira |
| `tools/business_name_test.tscn` | teste headless (amostras + 5.000 gerações) |

## Contrato de cada arquivo de língua

```json
{
  "code": "pt",
  "language": "Português",
  "script": "latin",
  "native_script": false,
  "sector_words": {"retail": "Comércio", "…": "…"},
  "types": {"retail": ["Mercearia", "…10 por setor…"], "…": []},
  "qualifiers": ["Central", "…40…"],
  "surnames": ["Silva", "…60 sobrenomes reais…"],
  "legal_suffixes": ["Lda.", "S.A.", "Unipessoal Lda."],
  "patterns": ["{type} {qualifier}", "{type} {surname}", "…5 a 8…"]
}
```

- **20 setores**, chaves fixas: `retail`, `wholesale`, `food_service`,
  `hospitality_tourism`, `freight_logistics`, `passenger_transport`, `finance`,
  `insurance_pension`, `health`, `education`, `telecom`, `it`,
  `media_advertising`, `professional_services`, `real_estate`, `construction`,
  `maintenance_repair`, `personal_services`, `leisure_culture`,
  `cleaning_security_waste`.
- **10 tipos** por setor, **40 qualificadores**, **60 sobrenomes reais**,
  **3–5 sufixos jurídicos** do país, **5–8 padrões**.
- `sector_words` é a palavra usada em nome de empresa ("Comércio", "Logística"),
  não o rótulo da tela.
- Placeholders aceitos nos padrões: `{type}` `{qualifier}` `{surname}`
  `{sector_word}` `{place}` `{legal}`. Literais ("&", "de", "i", "-") são
  permitidos. Todo arquivo tem ao menos um padrão com `{place}` e um sem
  (o lugar pode chegar vazio).
- `{surname}` repetido no mesmo padrão gera **dois sobrenomes diferentes**
  (sócios).
- Línguas de escrita não latina trazem o bloco `native` com a grafia nativa
  alinhada índice a índice; o nome gerado por padrão é romanizado (escrita
  latina) para o HUD, e `generate_native()` devolve a escrita nativa.

## Uso

```gdscript
var gen := BusinessNameGenerator.new()
var name := gen.generate("brasileiro", "food_service", "Lisboa", 42)
# → "Restaurante Silva" (exemplo; depende dos dados)
var native := gen.generate_native("japones", "retail", "Tóquio", 42)
```

- Determinístico: mesma instância + mesma sequência de chamadas + mesma
  (`culture_id`, `sector`, `place_name`, `seed`) → mesmo nome. A semente é um
  FNV-1a dos argumentos; não usa `randi()` global.
- Registro de nomes usados: ao colidir, tenta outro padrão, depois acrescenta o
  lugar (`Nome - Lugar`) e, por fim, um número (`Nome 2`).
- Cultura/setor inexistente devolve `""` e registra `last_error()` — a ausência
  é declarada, não inventada.
- `reset()` limpa o registro (nova partida). Romanizado e nativo têm registros
  separados.

## Cultura → língua

`culture_language.json` cobre as **206 culturas** de `cultures.json`. Quando a
língua da cultura não tem arquivo próprio, a cultura usa a **língua mais
próxima**, registrada em `nearest` com o motivo escrito (ex.: culturas
francófonas da África Ocidental usam `fr`; crioulos anglófonos usam `en`;
eslavos meridionais em latim usam `pl`). O validador exige que toda cultura
fora da língua exata esteja documentada.

## Verificação

```powershell
& "C:\Users\artur\AppData\Local\Python\bin\python.exe" tools/build_business_names.py
& "C:\Users\artur\AppData\Local\Python\bin\python.exe" tools/validate_business_names.py
& $godot --headless --path . res://tools/business_name_test.tscn
```

O validador falha se faltar cultura, setor, tipo, qualificador, sobrenome,
sufixo jurídico, se houver termo repetido, placeholder inválido, padrão sem
`{place}`/todos com `{place}`, ou grafia nativa desalinhada. O teste headless
imprime 10 nomes por setor para pt, en, ja e ar (2 amostras por setor) e exige
zero duplicatas em 5.000 gerações.

## Amostras

Saída real do teste headless (2 por setor; aqui 1 por setor). O lugar é o
passado por quem chama (`generate(cultura, setor, lugar, seed)`).

| Língua | Varejo | Alimentação | Finanças | Saúde |
|---|---|---|---|---|
| pt (brasileiro) | Florista Lisboa | Simões & Jesus | Finanças Antunes | Correia & Filhos |
| en (britânico) | Edwards Grocers | Beautiful Brewery | Green Ltd | Tide Optometry |
| ja (japonês) | Matsuda Pan'ya | Izakaya Gōdō gaisha | Lisboa Shōken gaisha | Yamada Iryō |
| ar (emiratense) | Sultan wa Bakr | Zayd & Taha Sh.M.Kh | Tamwil Lisboa | Mukhbar tibbi Lisboa |

Grafia nativa (mesma semente, mesmo negócio):

- ja: `Hasegawa Kutsuya` → `長谷川 靴屋`
- ar: `Maktaba Khaleeji` → `مكتبة خليجي`

## Limitações declaradas

- A base cobre 32 línguas; as demais línguas do cenário caem na mais próxima
  (documentada). Não há arquivo para pashto, albanês, croata, búlgaro etc.
- As grafias nativas foram curadas à mão; não passaram por revisor nativo.
- Concordância de gênero/número dos qualificadores **não é gerada**: os padrões
  foram escolhidos para evitar posposição de adjetivo onde a língua não a
  aceita, mas um qualificador adjetival pode não concordar com um tipo de outro
  gênero (ex.: um tipo masculino com qualificador feminino). Qualificadores
  nominais/marca não têm esse problema.
- Os nomes são fictícios: sobrenomes reais + tipo de negócio, sem relação com
  empresas existentes.
- `native` existe nas 14 línguas de escrita não latina (ru, uk, el, ar, fa, he,
  hi, bn, ur, zh, ja, ko, th, am); nas latinas a grafia nativa é a própria
  romanizada (`native_script: false`).
