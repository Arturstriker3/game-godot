#pragma once
// B3 — economia da região (estados inteiros). Módulo de domínio do interior:
// um bloco por cidade, simplificado (sem solo/moradia e sem prefeitura própria).
//
// O interior reaproveita B1 (5 coortes etárias, semeadas) e B2 (5 quintis de
// renda). Cadência A6:
//   - consumo de comida DIÁRIO (compra no mercado da cidade-polo, sem frete);
//   - emprego/salário/renda SEMANAL (`(dia - índice) % 7 == 0`): o setor
//     primário da cidade (fazendas/minas/madeira) é o empregador; salário com
//     desconto rural declarado (kRegionRuralWageDiscount) e imposto kTaxRate
//     pago à prefeitura;
//   - estratos/poupança MENSAIS (`(dia - índice) % 30 == 0`).
//
// Conservação: toda transferência tem contraparte (o salário sai do caixa da
// firma; o imposto entra no orçamento municipal; a comida é paga do caixa da
// região para o caixa da firma). `money_total` inclui `region.cash`.
//
// A fase `SimCore::phase_region` está implementada em region.cpp. A semeadura
// (coortes, quintis, mix de emprego) vive em sim_core.cpp (`seed_region*`).
// Ver docs/plans/b3_state_regions.md e a decisão D-085.
