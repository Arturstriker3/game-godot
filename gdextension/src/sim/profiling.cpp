#include "profiling.h"

// Em release o arquivo compila VAZIO: nenhum símbolo de perfil sai no .obj e,
// portanto, a DLL não paga nada pelo módulo.

#if defined(OGS_PROFILING)

#include <cstring>

namespace ogs {
namespace profile {

namespace {

// Marca de fase da thread. Guarda o sink dono da marca: se o sink mudar (por
// exemplo, o sink das sub-fases de status entrando e saindo), a próxima marca
// só se arma, sem gravar um intervalo que atravessaria dois sinks.
struct MarkState {
	const Sink *sink = nullptr;
	std::chrono::steady_clock::time_point mark{};
};

thread_local Sink *t_sink = nullptr;
thread_local MarkState t_mark;
thread_local std::vector<CounterSample> t_counters;

}  // namespace

Sink *current_sink() noexcept { return t_sink; }

void set_current_sink(Sink *sink) noexcept { t_sink = sink; }

bool sink_active(const Sink *sink) noexcept {
	return sink != nullptr && sink->slots != nullptr && sink->slot_count > 0 &&
			(sink->enabled == nullptr || *sink->enabled);
}

SinkScope::SinkScope(Sink *sink) noexcept : previous_(t_sink) {
	t_sink = sink;
	t_mark.sink = nullptr;
	if (sink_active(sink)) {
		t_mark.sink = sink;
		t_mark.mark = std::chrono::steady_clock::now();
	}
}

SinkScope::~SinkScope() { t_sink = previous_; }

void lap(int phase) noexcept {
	if (!sink_active(t_sink)) {
		// Desligado no runtime: nenhum relógio é lido (custo de um load).
		t_mark.sink = nullptr;
		return;
	}
	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	if (t_mark.sink == t_sink && phase >= 0 && phase < t_sink->slot_count) {
		t_sink->slots[phase] =
				std::chrono::duration<double, std::micro>(now - t_mark.mark).count();
	}
	t_mark.sink = t_sink;
	t_mark.mark = now;
}

Zone::Zone(int phase) noexcept
		: phase_(phase), sink_(sink_active(t_sink) ? t_sink : nullptr) {
	if (sink_ != nullptr) {
		start_ = std::chrono::steady_clock::now();
	}
}

Zone::~Zone() {
	if (sink_ == nullptr) {
		return;
	}
	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	if (phase_ >= 0 && phase_ < sink_->slot_count) {
		sink_->slots[phase_] =
				std::chrono::duration<double, std::micro>(now - start_).count();
	}
}

ZoneAdd::ZoneAdd(int phase) noexcept
		: phase_(phase), sink_(sink_active(t_sink) ? t_sink : nullptr) {
	if (sink_ != nullptr) {
		start_ = std::chrono::steady_clock::now();
	}
}

ZoneAdd::~ZoneAdd() {
	if (sink_ == nullptr) {
		return;
	}
	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	if (phase_ >= 0 && phase_ < sink_->slot_count) {
		// Diferença da `Zone`: SOMA no slot (zonas dentro de laço).
		sink_->slots[phase_] +=
				std::chrono::duration<double, std::micro>(now - start_).count();
	}
}

void reset_phase(int phase) noexcept {
	if (!sink_active(t_sink)) {
		return;
	}
	if (phase >= 0 && phase < t_sink->slot_count) {
		t_sink->slots[phase] = 0.0;
	}
}

void counter(const char *name, double value) noexcept {
	if (name == nullptr) {
		return;
	}
	for (CounterSample &sample : t_counters) {
		if (std::strcmp(sample.name, name) == 0) {
			sample.value = value;
			return;
		}
	}
	try {
		t_counters.push_back(CounterSample{name, value});
	} catch (...) {
		// Sem memória: um contador perdido não pode derrubar o tick.
	}
}

const std::vector<CounterSample> &counter_samples() noexcept { return t_counters; }

namespace {
// Guarda de regressão (Q-006): duas `OGS_SIM_ZONE` no mesmo escopo precisam de
// identificadores distintos. A macro usa a indireção `OGS_CAT` para expandir
// `__LINE__` antes da colagem; sem ela (`##__LINE__` cola o literal), as duas
// declarações abaixo colidem e o build debug falha aqui.
[[maybe_unused]] void zone_same_scope_compile_guard(
		int phase_a, int phase_b, int phase_c) noexcept {
	OGS_SIM_ZONE(phase_a);
	OGS_SIM_ZONE(phase_b);
	OGS_SIM_ZONE_ADD(phase_c);
}
}  // namespace

}  // namespace profile
}  // namespace ogs

#endif  // OGS_PROFILING
