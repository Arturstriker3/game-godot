# Q-013 — Revisão SÓ DE DADOS do commit `8ec57a2` (solo urbano, habitação, finanças municipais)

- **Escopo revisado:** `data/definitions/economy/{urban_land,housing,municipal_finance}.json`
  + geradores/validadores `tools/{build,validate}_{urban_land,housing,municipal_finance}.py`
  + `docs/economy/urban_housing_data.md` + GAME_STATE §14 D-059 + QUADRO D-003.
- **Ambiente:** revisão só de dados — **nenhum** Godot nem build C++ rodado (o build está
  com o @gerente-simulacao). Python 3.14.2 (`C:\Users\artur\AppData\Local\Python\bin\python.exe`).
- **Veredito:** **APROVADO COM RESSALVA** (2 ressalvas não-bloqueantes; nenhum erro de dado
  encontrado, nenhum arquivo de dado corrigido).

## 1. Validação (rodada nesta revisão)

```
python tools/validate_urban_land.py          → 0  (834/834 cidades; 117 observadas + 717 derivadas)
python tools/validate_housing.py             → 0  (179/179 nações)
python tools/validate_municipal_finance.py   → 0  (179/179 nações)
```

Os três retornam 0, como declarado no relatório. O validador de `urban_land` confere a
densidade/área observada contra o cache local do Atlas (`tools/data/urban_land_cache/`); o de
`housing` recalcula o estoque por padrão a partir do Gini de `inequality.json`; o de
`municipal_finance` confere as flags `*_estimated` por campo.

## 2. Conferência contra fontes (verificador-flash independente — outro Flash)

Amostra **18 entidades** (11 cidades + 5 nações de habitação + 4 nações de imposto sobre
imóveis), **41 conferências numéricas**, todas **APROVADO**:

| Bloco | Conferências | Resultado |
| --- | --- | --- |
| Área construída observada (Atlas AUE 2016) | 9 cidades | 9/9 exatas (desvio ≤0,05 km²) |
| Densidade observada (Atlas `persons/ha ×100`) | 9 cidades | 9/9 exatas |
| População da aglomeração (citypopulation.de 2026) | 9 cidades | 9/9 exatas |
| Favela ODS 11.1.1 (World Bank API `EN.POP.SLUM.UR.ZS`) | BRA/IND/NGA/DEU/USA | 5/5 exatas |
| Casa própria (OECD HM1.3) | DEU/USA | 2/2 exatas; BRA/IND/NGA proxy corretos |
| Imposto sobre imóveis (OECD RS 4100 ÷ tributação total) | BRA/USA/FRA/CHN | 4/4 exatas (BRA 1,9178→1,92 · USA 10,4324→10,43 · FRA 4,4790→4,48 · CHN 2,7087→2,71) |

Entidades cobrindo o pedido: São Paulo, Tóquio, Lagos, Mumbai, Cidade do México, Paris,
Nova York, Cairo, Jacarta, **Suva (Pacífico)** e **Mbabane/Port-Gentil (África, derivada)**
— casa própria/favela de BRA, IND, NGA, DEU, USA — imposto sobre imóveis de BRA, USA, FRA, CHN.

## 3. Plausibilidade (auditoria transversal)

- **Densidade hab/km² em faixa real:** min **1.700** (EUA suburbano) a máx **55.200**
  (Daca — máximo real do Atlas; Hong Kong 46.700). Tudo dentro da faixa real de densidade
  urbana construída (~2.000–55.000). Nenhum valor fora do teto de 60.000.
- **Soma das shares de zona = 1:** **0 violações** nas 834 cidades (residencial 0,55–0,68 /
  comercial 0,17–0,32 / industrial 0,13–0,15).
- **Metrópoles com gabarito alto:** **96/97** aglomerações ≥5 M são `high`. A única exceção é
  Washington (ver ressalva 1).
- **Estimativas marcadas `estimated` com método:** sim — `housing` tem campo `method` por
  entrada; `urban_land` documenta o método na `refs[0].title` ("Faixa de densidade urbana por
  nível de renda … proxy … não é medição desta cidade") + `*_source` (`income_group_proxy`,
  `population_threshold`); `municipal_finance` documenta na `refs` ("estimativa (não é
  medição): faixa renda …") — ver ressalva 2.

## 4. Ressalvas (não bloqueiam)

**RESSALVA 1 — `height_class` usa a população de cenário, não a aglomeração real.**
`tools/build_urban_land.py:757-762` aplica o limiar sobre `city["population"]` (população
nacional rateada de `cities.csv`), não sobre `agglomeration_population` (referência real).
Consequência medida: **Washington** (`USA-WAS`, aglomeração real 8,8 M) fica `mid` (cenário
4,8 M) e **Portland** (`USA-POR`, real 2,5 M) fica `low` (cenário 466 k), enquanto ~191
cidades secundárias (1–5 M reais) ficam `high` por cenário inflado (Berlim, Roma, Sevilha…).
É uma escolha **defensável** (o gabarito acompanha a densidade simulada no jogo, não a
real — o jogo simula Nova York com 61 M), e a fonte está transparente
(`height_class_source="population_threshold"`). Mas o doc diz "Metrópoles começam com área
grande e densidade alta" e o relatório `urban_housing_data.md` diz "population ≥ 5 M → high"
sem dizer qual população. **Não corrigi** (é regra de negócio do @gerente-dados); recomendo
**documentar explicitamente** a escolha (cenário × aglomeração) ou trocar para
`agglomeration_population` quando disponível.

**RESSALVA 2 — `municipal_finance` sem `method` por entrada.** Ao contrário de `housing`
(`estimated=true` exige `method`), as 72 nações 100% proxy de `municipal_finance` não têm
campo `method` próprio — o método só aparece na `refs.title` ("estimativa (não é medição):
faixa renda …"). Funcional e honesto, mas estruturalmente inconsistente com os outros dois
arquivos. Recomendo alinhar (campo `method` por entrada) numa rodada futura.

## 5. O que falta para o modelo §19 (lacunas honestas, já declaradas no relatório)

1. **Relevo/topografia** — a maior lacuna: `expandable_reserve_km2` usa só `coast+landmass`
   (×0,60; ×0,55 costeira; ×0,40 ilha; teto 3×). O heightmap do projeto é placeholder plano;
   encostas/alagáveis não limitam a expansão. Declarado em `urban_housing_data.md §6.2`.
2. **Aluguel/preço por cidade** — `rent_to_income_ratio` é por **nação** (OECD). O §19 itens
   7–8 precisa de aluguel/preço **local** por cidade, que vem do modelo de mercado (fase c).
3. **Estoque habitacional absoluto** — `housing_stock_split` é **share** (popular/médio/alto),
   não contagem de unidades. O estoque absoluto por cidade (área × densidade × domicílio) é
   derivável mas ainda não está no dado.
4. **Posse por estrato de renda** — `homeownership_rate` é agregado por nação; o §19 item 7
   quer casa própria × aluguel **por estrato** (popular/médio/alto). Só a ligação via
   `housing_stock_split` existe hoje.
5. **Preço de terreno por zona** — §19 item 6; é modelo de mercado, fora do escopo de dados.
6. **Finanças municipais por cidade** — o dado é por nação; a "prefeitura por cidade" (§19)
   exigirá alocar a receita/imposto nacional às 834 cidades.

## 6. Conclusão

Dados **fiéis às fontes e plausíveis**; validadores 3/3 verdes; 41/41 conferências independentes
batem com fonte viva ou cache citado. Nenhum número inventado, nenhum proxy disfarçado de
medição. Nenhum erro de dado simples a corrigir — as duas ressalvas são de **modelagem/estrutura**
(regra do gabarito e campo `method`), endereçadas ao @gerente-dados, sem bloquear o commit.
