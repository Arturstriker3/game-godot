#pragma once
// H1f — declarações internas compartilhadas pelos TUs de `sim/power/`.
// Nada de regra aqui: só o que mais de um domínio usa. Os corpos continuam no
// .cpp dono (grid/plants/retail) e viajam verbatim do antigo `sim/power.cpp`.
//
// `month_of_day` e `fuel_product_of` nasceram no namespace anônimo do TU único;
// agora têm ligação no namespace `power_detail` para o grid e a liquidação
// (retail) lerem a MESMA definição. `kPowerHoursPerYear` é a calibração
// (horas do ano) usada pelo seed e pela fase; só ganhou `inline` para caber no
// header — valor e tipo não mudaram.

#include "../power.h"

namespace ogs {

// Horas do ano para o fator de capacidade anual (calibração nacional).
inline constexpr double kPowerHoursPerYear = 8760.0;

namespace power_detail {

// Mês (0-11) de um dia absoluto do calendário do sim (365 dias).
int month_of_day(int day);

// Produto de combustível de cada fonte térmica (-1 = sem compra de mercado).
int fuel_product_of(const WorldState& ws, int fuel);

} // namespace power_detail
} // namespace ogs
