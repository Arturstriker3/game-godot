# Mecânica de porto — design (v1)

> Escopo: `gdextension/src/economy_sim.*` (núcleo) e os pontos de integração em
> GDScript. UI, cena 3D e `map/` ficam para depois.
>
> Estado anterior (auditado em 2026-11-13): o porto **não** tem papel
> econômico. `demo_nearest_port()` só escolhe o porto mais próximo da
> **mesma nação e mesma massa de terra** para desenhar os trechos dos comboios
> (`TradeOrder.legs`); o frete é calculado pela distância em linha reta entre as
> cidades (`kDemoFreightShipPerTonKm = 0.02`) e a carga chega **sempre** em
> `day + 1`, qualquer que seja a rota. Sem porto, o código caía em "navio
> direto" (uma travessia marítima sem cais). `port_of` não é lido pelo núcleo:
> é só metadado do cenário.

## 1. Porto nacional (não é do dono da cidade-sede)

- Qualquer cidade pode exportar/importar por **qualquer porto da sua nação na
  mesma massa de terra**; `port_of` deixa de significar "dono exclusivo" e
  passa a ser só a **cidade-sede** (âncora do ícone, base da capacidade e
  endereço do cais).
- Rota de uma carga entre massas de terra:
  1. **terrestre** cidade vendedora → porto nacional (caminhão);
  2. **marítima** porto → porto (navio);
  3. **terrestre** porto nacional do comprador → cidade compradora (caminhão).
- Porto escolhido: o de **menor custo de frete terrestre** (o mais próximo, já
  que a tarifa por t·km é a mesma) entre os da nação e da massa de terra da
  cidade. Congestionamento **não** troca o porto na v1: ele entra na fila
  (seção 2) — trocar de cais é uma decisão de v2.
- Mesma massa de terra continua indo de caminhão direto (sem porto, como hoje).

## 2. Capacidade, fila e expansão

- Unidade: **t/dia por porto** (carga movimentada, somando embarque e
  desembarque).
- Capacidade inicial (nível 1) pela **população de simulação da cidade-sede**:

  ```
  base = clamp(1000 + 0,002 × pop_sim(cidade-sede), 1000, 50 000)  [t/dia]
  ```

  A âncora é a ordem de grandeza real de um porto médio (~3–4 Mt/ano); o teto
  evita que uma metrópole transforme o porto num cano infinito. Cenário sem
  `port_of` (bench sintético) cai no piso de 1 000 t/dia.
- **Nível 1..N**: `capacidade = base × nível`. O nível é derivado da capacidade
  (`ceil(capacidade / base)`), então expandir aumenta o nível.
- **Fila**: cada porto acumula a carga comprometida do dia (`used`). Ao
  comprometer `x` toneladas:

  ```
  overflow   = max(0, used + x − capacidade)
  delay_dias = overflow / capacidade          (fração de dia de atraso)
  demurrage  = overflow × 0,5 × delay_dias    (unidades monetárias)
  ```

  O atraso entra no tempo do pedido e **atrasa a chegada** em
  `ceil(delay_dias)` dias (`arrival_day = day + 1 + atraso`). O demurrage é
  pago pelo **orçamento público da cidade compradora** (autoridade portuária) e
  vai para o setor de frete dela — dinheiro conservado. Sem orçamento, a fila
  atrasa mas não cobra (verba não existe, não se inventa dinheiro).
- **Expandir** (API; UI depois):
  `demo_expand_port(port_id, t_por_dia)` →
  custo `500 × t_por_dia` pago pelo orçamento público da cidade-sede ao setor de
  construção; a capacidade só entra em operação depois de **30 dias** (obra).
  Retorno: `{expanded, cost, days, capacity, error}`.
  Hook de teste: `demo_set_port_capacity(port_id, t_por_dia)`.

## 3. Modais: capacidade e custo

| modal   | carga/lote | velocidade | custo (por t·km) | turnaround |
|---------|-----------:|-----------:|-----------------:|-----------:|
| caminhão| 25 t       | 65 km/h    | 0,10             | 6 h        |
| navio   | 2 000 t    | 35 km/h    | 0,02 (5× menor)  | 24 h       |
| avião   | 20 t       | 750 km/h   | 0,80             | 24 h       |

- `lote` é **um veículo**: um pedido de 500 t vira 20 caminhões ou 1 navio.
  A frota parte junto (o tempo é o do veículo, não `tempo × nº de veículos`) e
  o embarque paga uma sobrecarga por lote.
- **Transbordo** no porto custa **8 h** por operação (carga no porto de origem,
  descarga no de destino) e entra no tempo do pedido.
- O frete aéreo continua sendo a ponte de emergência (estoque < 2 dias, 35% do
  déficit), agora com lote de 20 t.

## 4. Rotas internacionais só entre nações com porto

- Uma travessia marítima exige **porto na nação do vendedor e na do
  comprador**, na massa de terra de cada cidade.
- Sem porto de um dos lados: **não há rota** (nem navio "direto"). A nação sem
  porto só comercia por terra com vizinhos da mesma massa de terra.
- Consequência assumida: nações/ilhas sem porto no roster deixam de importar
  comida por mar. É a regra estratégica pedida ("a posição do porto dá acesso
  marítimo"), não um bug; o teste do mundo real (fome/Cayenne) é o guarda.

## 5. `port_of` deixa de ser dono exclusivo

- `ports.json` mantém `port_of` (fonte única: `ports.csv`); o núcleo passa a
  lê-lo como `Port.host_city`.
- Nada no cenário muda. Nenhuma definição de economia é reescrita: `port_of`
  continua válido para cidade-sede/ícone; só deixa de limitar o uso do porto a
  essa cidade.

## 6. Impacto em balanço

- **Frete marítimo mais caro e mais honesto**: o custo da rota passa a somar
  caminhão (cidade→porto) + navio + caminhão (porto→cidade), em vez da linha
  reta com tarifa só de navio. Cidades do interior pagam o trecho terrestre;
  cidades costeiras quase não mudam.
- **Sem porto, sem importação marítima**: muda o padrão de comércio de nações
  sem litoral/porto. É a intenção; medir no `economy_world_test` (Cayenne tem
  porto e continua abastecida).
- **Capacidade/fila**: com a calibragem acima (**144 portos, 877 277 t/dia**
  somados no cenário de 834 cidades) a fila fica em zero no cenário real (vazão
  medida ~5 500 t/dia, 0,6% da capacidade); ela existe para o jogador saturar
  um porto de propósito (guerra, boom de importação) e para a expansão ter
  valor. O `port_capacity_test` força a saturação com capacidade artificial.
  Números medidos em `docs/economy/balance_review.md` §23.
- **Dinheiro**: demurrage e expansão só movem caixa entre agentes existentes
  (comprador → frete; governo da sede → construção). `money_total` conservado.

## 7. Impacto em performance

- **Uma passada por dia** para escolher o porto de cada cidade:
  `cidades × portos` da mesma nação/massa (≈ 834 × 144 no pior caso, na prática
  bem menos por nação) — feita uma vez antes do laço de compradores; medida
  dentro do ruído do tick (74,84 ms contra 75,65 ms de baseline em
  `perf_day_peak`, §23.7 do balance review).
- **Sem busca de rota nova**: eram 3 chamadas de `distance_km` por pedido
  marítimo; passam a ser 3 (as mesmas) usando os portos pré-escolhidos.
- **Fila**: O(1) por porto por embarque (soma/subtração de doubles).
- **Status**: +8 campos por porto (144 × 8 escritas/dia) — desprezível.
- **Nada muda** na fase `arrival`, no status das cidades nem no cache de
  mercados; o teto de 512 embarques/dia continua.
- Sem dependência de UI; comboios 3D só leem os trechos que já existiam (agora
  sempre com porto real, sem o fallback de navio direto).

## 8. Pendências (fora da v1)

- UI de porto (capacidade, fila, botão de expandir) — outro agente.
- Troca de porto por congestionamento e porto "internacional" (hub) — v2.
- Container/carga geral: hoje a capacidade é medida na carga da cesta
  alimentar + bens brutos que passam pela mesma rota.
