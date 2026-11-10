# Contrato econômico único — proposta

> Documento de decisão para as melhorias de modelo (1.1, 1.2, 1.4, 1.10, 1.11, 1.13).
> Nada aqui é implementado ainda; define o alvo para não quebrar o save e o balanço.

## Situação

O `EconomySim` tem **dois contratos**:

- **legacy** (`tick`): só trigo por cidade; é o que o jogo ao vivo usa.
- **demo** (`tick_demo`): ciclo completo (dinheiro, preços, salários, PIB,
  investimento, save/load) — usado por testes/IA, não ligado ao loop principal.

Crescer sobre o legacy é ampliar o modelo errado.

## Alvo

Um único modelo, data-driven, com dinheiro estável:

1. **Um contrato**: o `tick_demo` vira o contrato do jogo. O legacy sai (ou vira
   um caso degenerado do demo com 1 produto).
2. **Bens por ID string** (não índices 0..3): lista de bens e receitas
   (`input_per_output`) em JSON (`data/definitions/economy/goods.json`), lidos no
   `initialize`. Novo bem = editar JSON, sem recompilar.
3. **Preço por mercado** (regional/nacional/internacional) para todos os bens,
   com piso de custo, suavização e banda (como o demo já faz para poucos bens).
4. **Dinheiro inteiro (centavos)**: `int64` em vez de `double` para caixa/preços/
   salários; evita drift em partidas longas. (Decisão de implementação: manter
   `double` só onde for proporção/índice.)
5. **Transporte com capacidade**: cada rota tem teto (t/dia) por modo; a cidade
   precisa de capacidade (porto/estrada) para mover o que compra/vende.
6. **Extração**: reserva do depósito influencia a **taxa** máxima (não esgota);
   capacidade de extração da cidade + porto; **offshore só por porto**.
7. **Invariantes em runtime**: por tick, verificar conservação de dinheiro,
   caixa/estoque ≥ 0, preço finito na banda; se violar, logar e reverter o tick.

## Migração (etapas, cada uma com rebuild + testes)

1. Ligar o `tick_demo` ao jogo vivo (substituir `tick`), com os dados do cenário.
2. Extrair bens/receitas para JSON.
3. Generalizar preço para N bens.
4. Adicionar capacidade de transporte.
5. Extração + offshore.
6. Migrar dinheiro para inteiro (por último, é a mais invasiva).

## Compatibilidade

- `kDemoSaveVersion` (já adicionado) protege saves de versões futuras.
- `balance.json` já parametriza o demo (`demo_set_balance`).
- `GeographyService.apply_to_sim` e a IA leem o mesmo estado — manter fonte única.

## Critérios de aceite

- `economy_integration_test` e `economy_ai_test` verdes a cada etapa.
- `economy_long_run_test` verde antes de mesclar qualquer etapa de modelo.
- Save round-trip (to_dict/from_dict) idêntico.
- Sem NaN/Inf; caixa e estoques ≥ 0; preço na banda.
