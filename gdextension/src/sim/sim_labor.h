#pragma once
// Parâmetros e funções do mercado de trabalho (F7/F9), compartilhados pelo
// núcleo simulado. C++ puro (só STL) — nada de godot_cpp.

#include <algorithm>
#include <cmath>

namespace ogs {

// Perfil derivado do índice de renda externa (PIB PPP per capita normalizado
// para a média mundial = 1,0). O cenário NÃO traz estrutura etária nem
// escolaridade por nação: estes são parâmetros declarados, na ordem de
// grandeza de ILO/World Bank (participação e idade ativa) e Barro-Lee/UNDP
// (escolaridade). São calibráveis no laboratório; não são dados observados.
constexpr double kLaborIncomeSaturation = 3.0;
constexpr double kLaborWorkingAgeLow = 0.57;
constexpr double kLaborWorkingAgeHigh = 0.66;
constexpr double kLaborRetireeLow = 0.04;
constexpr double kLaborRetireeHigh = 0.17;
constexpr double kLaborBasicEduLow = 0.80;
constexpr double kLaborBasicEduHigh = 0.25;
constexpr double kLaborHigherEduLow = 0.03;
constexpr double kLaborHigherEduHigh = 0.30;
constexpr double kLaborParticipationLow = 0.74;
constexpr double kLaborParticipationHigh = 0.65;
constexpr double kLaborParticipationByLevel[3] = {0.70, 0.80, 0.88};
constexpr double kLaborStudentShareLow = 0.02;
constexpr double kLaborStudentShareHigh = 0.11;
constexpr double kLaborRecruitShare = 0.55;
// F7 calibração — SALÁRIO COMO PARCELA DO VALOR ADICIONADO.
//
// A âncora absoluta antiga (0,030/0,044/0,060 XGS/dia) foi herdada da escala do
// setor de serviços e ficou ~60× abaixo da receita por trabalhador da produção
// primária (20 t/dia ÷ 7,5 empregos/t × 40/t ≈ 5,3 por trabalhador/dia). O
// resultado era um mercado de trabalho dual: a folha pagava uma fração mínima
// do valor adicionado e o resto virava lucro distribuído igualmente a toda a
// POP — 832/834 cidades no nível de consumo máximo.
//
// Agora o NÍVEL do salário é a parcela do trabalho no valor adicionado do dia
// (`city.gdp` = valor adicionado das construções + receita de serviços), entre
// 50% e 65% conforme a renda real do país, e a folha é normalizada por essas
// razões: Σ(vagas preenchidas × salário) = participação × VA. A riqueza que
// sobra é LUCRO e vai para os DONOS (estrato superior), não para toda a POP.
//
// Só as razões entre níveis são fixas (básico 1,00 / intermediário 1,15 /
// superior 1,30) e pequenas de propósito: o PRÊMIO por educação emerge do aperto
// de cada nível (vagas sem candidato × desempregados), como na F7.
constexpr double kLaborWageRatio[3] = {1.00, 1.15, 1.30};
// Salário inicial de criação do mundo (XGS/dia por vaga): um palpite de ordem
// de grandeza do valor adicionado por vaga pago ao trabalho — NÃO é o salário
// de equilíbrio. O primeiro dia com valor adicionado conhecido ancora o salário
// na parcela do trabalho (demo_update_wages) sem inércia.
constexpr double kLaborWageInitial = 0.055;
// Mistura de escolaridade das vagas de serviços urbanos (varejo, reparos,
// transporte local): maioria básico, minoria com superior.
constexpr double kLaborServiceMix[3] = {0.62, 0.30, 0.08};
// Qualificação: duração média da formação (3 e 4 anos), taxa anual de matrícula
// em função do prêmio salarial, fração jovem do nível de origem e teto de
// retirada do desemprego do nível (a escola não pode esvaziar a produção).
constexpr double kLaborStudyDurationDays[2] = {1095.0, 1460.0};
constexpr double kLaborStudyPremiumThreshold = 1.15;
constexpr double kLaborStudyElasticity = 0.12;
constexpr double kLaborStudyMaxAnnual = 0.08;
constexpr double kLaborStudyYouthShare = 0.10;
constexpr double kLaborStudyJoblessShare = 0.25;
constexpr double kLaborStudyMaxDailyShare = 0.002;
// Ajuste salarial gradual: sensibilidade ao aperto e à margem das firmas.
constexpr double kLaborWageTightnessGain = 1.20;
constexpr double kLaborWageMarginGain = 0.60;
constexpr double kLaborIncomeElasticity = 0.30;

// ---------------------------------------------------------------------------
// F7 calibração — distribuição de renda por Gini.
//
// A repartição da POP em classes usa uma lognormal: para um Gini G, o sigma é
// `sqrt(2) * Φ⁻¹((1+G)/2)` e a média é ancorada no PIB PPP per capita. As
// funções abaixo são as aproximações padrão (Acklam para Φ⁻¹ e erf para Φ);
// nenhum número de nação é inventado aqui — Gini e índice vêm dos dados.
// ---------------------------------------------------------------------------
inline double normal_cdf(double z) {
	return 0.5 * (1.0 + std::erf(z / std::sqrt(2.0)));
}

// Φ⁻¹(x) — Peter Acklam, erro absoluto < 1,15e-9 (domínio (0,1)).
inline double normal_quantile(double p) {
	if (p <= 1e-12) {
		return -8.0;
	}
	if (p >= 1.0 - 1e-12) {
		return 8.0;
	}
	static const double a[6] = {
		-3.969683028665376e+01, 2.209460984245205e+02,
		-2.759285104469687e+02, 1.383577518672690e+02,
		-3.066479806614716e+01, 2.506628277459239e+00};
	static const double b[5] = {
		-5.447609879822406e+01, 1.615858368580409e+02,
		-1.556989798598866e+02, 6.680131188771972e+01,
		-1.328068155288572e+01};
	static const double c[6] = {
		-7.784894002430293e-03, -3.223964580411365e-01,
		-2.400758277161838e+00, -2.549732539343734e+00,
		4.374664141464968e+00, 2.938163982698783e+00};
	static const double d[4] = {
		7.784695709041462e-03, 3.224671290700398e-01,
		2.445134137142996e+00, 3.754408661907416e+00};
	const double low = 0.02425;
	const double high = 1.0 - low;
	if (p < low) {
		const double q = std::sqrt(-2.0 * std::log(p));
		return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q
			+ c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
	}
	if (p > high) {
		const double q = std::sqrt(-2.0 * std::log(1.0 - p));
		return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q
			+ c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
	}
	const double q = p - 0.5;
	const double r = q * q;
	return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r
		+ a[5]) * q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r
		+ b[4]) * r + 1.0);
}

inline double labor_income_factor(double income_index) {
	// Riqueza do país: PIB per capita elevado paga salários maiores na âncora.
	// Expoente 0,30 (renda não é repassada 1:1, pois o nível de preços do modelo
	// é comum) entre 0,55 e 1,9 — Nigéria ~0,66; EUA ~1,46.
	const double income = std::clamp(income_index, 0.0, 8.0);
	return std::clamp(std::pow(std::max(income, 1e-6), kLaborIncomeElasticity),
		0.55, 1.9);
}

// Espelho compilado de data/definitions/economy/sector_employment.json. A fonte
// de verdade é o JSON (carregado por demo_set_sector_employment); este espelho
// existe para testes/benchmark rodarem sem o arquivo.
constexpr int kDemoSectorAnchorCount = 5;
struct DemoSectorAnchorDefault {
	double income;
	double agriculture;
	double industry;
	double informal;
};
constexpr DemoSectorAnchorDefault kDemoSectorAnchorDefaults[kDemoSectorAnchorCount] = {
	{0.371, 0.35, 0.12, 0.92},   // NGA
	{0.457, 0.43, 0.25, 0.90},   // IND
	{0.912, 0.09, 0.20, 0.37},   // BRA
	{2.935, 0.013, 0.27, 0.08},  // DEU
	{3.503, 0.015, 0.19, 0.06},  // USA
};

}  // namespace ogs
