#pragma once
// G — AGRICULTURA SAZONAL (D-096). Ver docs/economy/agriculture_seasonal.md.
//
// O dado (calendário de plantio/colheita, área colhida, rendimento, perda
// pós-colheita, fertilizante N, estoque de fim de safra e prateleira) chega pelo
// `AgricultureService` (GDScript) e vive em `WorldState::crop_plans` e nos
// vetores por produto. Aqui ficam:
//   * as conversões de calendário (dia do jogo -> ano/dia-do-ano/mês);
//   * as janelas de safra (inclusive as que atravessam o ano);
//   * a fase `phase_agriculture` (plantio/fertilizante, colheita na janela,
//     perda de armazenagem e perecibilidade), implementada no SimCore.
//
// Regra dura: o preço NÃO é formado aqui (D-091/D-092, `prices.cpp`). A
// sazonalidade aparece no VOLUME: fora da janela de colheita o fluxo é zero, o
// estoque drena e a oferta concentrada move o preço livre.

#include <string>

namespace ogs {

// Data civil derivada do dia do jogo: dia 0 = 1º de janeiro de `kAgriStartYear`
// (2027), a mesma âncora de `Economy.START_UNIX`. Sem fuso, sem hora.
// Anos bissextos (2028, 2032, ...) têm 366 dias — o dia-do-ano é 1..365/366.
struct GameDate {
	int year = 2027;
	int doy = 1;    // dia do ano, 1..365/366
	int month = 1;  // 1..12
	int day = 1;    // 1..31
};

GameDate game_date_from_day(int day);
// Dias do ano civil (365/366). Exposto para o teste/status.
int game_year_days(int year);
// `doy` está na janela inclusiva [start, end]? `start > end` = atravessa o ano
// (ex.: colheita de nov a jan). Janela degenerada (start == end) = 1 dia.
bool doy_in_window(int doy, int start, int end);
// Comprimento em dias da janela inclusiva (>= 1).
int window_length(int start, int end);
// Posição 0..len-1 do dia dentro da instância da janela que o contém; -1 se
// `doy` está fora da janela.
int window_position(int doy, int start, int end);
// Ano civil em que começou a instância da janela que contém `doy` (o ano de
// `doy` quando a janela não atravessa; o ano anterior quando `doy` já está na
// parte "de janeiro" de uma janela que virou o ano).
int window_start_year(int year, int doy, int start);

} // namespace ogs
