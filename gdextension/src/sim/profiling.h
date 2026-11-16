#pragma once

// Profiler por fase do núcleo econômico (docs/economy/core_refactor_plan.md §7,
// passo S2).
//
// Regras:
// - `sim/` é STL puro: aqui só <chrono>/<vector>. Nada de godot_cpp.
// - Em RELEASE `OGS_PROFILING` NÃO é definido (ver gdextension/CMakeLists.txt):
//   as macros expandem para `((void)0)` — nenhum `steady_clock::now()`, nenhum
//   branch, nenhum símbolo no .obj.
// - Em DEBUG o hospedeiro instala um `Sink` (array de slots em µs) com
//   `SinkScope`; o toggle em runtime (`demo_set_profiling`) via `Sink::enabled`
//   decide se a marca de relógio é paga — desligado, o custo é um load.
//
// Fase sequencial (padrão "marca", herdado do `lap()` original: o rótulo é a
// fase que ACABOU de rodar):
//
//     OGS_SIM_PHASE_LAP(kDemoPhaseReset);
//
// Escopo (RAII: mede o próprio bloco e grava no slot ao sair):
//
//     {
//         OGS_SIM_ZONE(kDemoPhaseStatus);
//         build_status();
//     }
//
// Contadores não-temporais (plano §7.2):
//
//     OGS_SIM_COUNTER("offers_used", static_cast<double>(used));

#if defined(OGS_PROFILING)

#include <chrono>
#include <vector>

namespace ogs {
namespace profile {

// Destino das amostras: slots de microssegundos do hospedeiro.
struct Sink {
	// Array de slots (µs) — ex.: EconomySim::demo_profile_us_.
	double *slots = nullptr;
	int slot_count = 0;
	// Toggle em runtime; nullptr = sempre ligado.
	const bool *enabled = nullptr;
};

// Sink instalado na thread atual (nullptr = sem coleta).
Sink *current_sink() noexcept;
void set_current_sink(Sink *sink) noexcept;

// Há coleta? (sink instalado, slots válidos e toggle ligado)
bool sink_active(const Sink *sink) noexcept;

// Instala um `Sink` na thread atual e o restaura ao sair. A construção (re)arma
// a marca de fase — é o `lap_mark = now()` no topo do tick original.
class SinkScope {
public:
	explicit SinkScope(Sink *sink) noexcept;
	~SinkScope();
	SinkScope(const SinkScope &) = delete;
	SinkScope &operator=(const SinkScope &) = delete;

private:
	Sink *previous_ = nullptr;
};

// Fecha a fase corrente: grava no slot `phase` o tempo desde a marca anterior e
// rearma a marca. Equivale ao `lap(phase)` que este módulo substitui.
void lap(int phase) noexcept;

// Zona RAII: mede o próprio escopo e grava no slot `phase` na saída.
class Zone {
public:
	explicit Zone(int phase) noexcept;
	~Zone();
	Zone(const Zone &) = delete;
	Zone &operator=(const Zone &) = delete;

private:
	int phase_ = -1;
	Sink *sink_ = nullptr;
	std::chrono::steady_clock::time_point start_{};
};

// Contador não-temporal: guarda o último valor de cada nome na thread.
// `name` precisa ter vida longa (literal) — o texto é lido com strcmp.
struct CounterSample {
	const char *name = nullptr;
	double value = 0.0;
};
void counter(const char *name, double value) noexcept;
const std::vector<CounterSample> &counter_samples() noexcept;

}  // namespace profile
}  // namespace ogs

#define OGS_CAT_INNER(a, b) a##b
#define OGS_CAT(a, b) OGS_CAT_INNER(a, b)
#define OGS_SIM_ZONE(phase) ::ogs::profile::Zone OGS_CAT(_ogs_zone_, __LINE__)(phase)
#define OGS_SIM_PHASE_LAP(phase) ::ogs::profile::lap(phase)
#define OGS_SIM_COUNTER(name, value) ::ogs::profile::counter((name), (value))

#else  // sem OGS_PROFILING: custo zero, nenhum símbolo

#define OGS_SIM_ZONE(phase) ((void)0)
#define OGS_SIM_PHASE_LAP(phase) ((void)0)
#define OGS_SIM_COUNTER(name, value) ((void)0)

#endif
