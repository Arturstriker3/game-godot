#pragma once
// Pool de threads próprio do núcleo (S16 do docs/economy/core_refactor_plan.md).
// C++ puro (só STL) — nada de godot_cpp. Usado SOMENTE dentro do tick (que já
// roda sob o mutex do writer); nunca a partir de get_demo_status.
//
// Partição ESTÁTICA e contígua por cidade: [0, n) é dividido em até
// `thread_count()` blocos e cada worker (mais a thread chamadora) processa o
// seu. As fases paralelas escrevem apenas na cidade "dona" do bloco, então o
// resultado independe de N e é bit-idêntico ao serial — nada de work-stealing,
// nada de std::execution, nada de atomic<double> (dinheiro nunca é somado em
// acumulador compartilhado; a redução, quando existe, roda em ordem fixa numa
// passada serial separada).

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ogs {

// Mundo pequeno demais para valer a pena acordar as threads do pool. O cenário
// de validação AGRIA/PORTUS (2 cidades) e mundos de teste continuam seriais,
// o que mantém o golden estável e evita o custo de sincronização em vazio.
inline constexpr std::size_t kThreadPoolMinCities = 256;

class ThreadPool {
public:
	explicit ThreadPool(int threads) : threads_(std::max(1, threads)) {
		// threads_ - 1 workers; a thread chamadora também trabalha.
		for (int i = 1; i < threads_; ++i) {
			workers_.emplace_back([this] { worker_loop(); });
		}
	}
	~ThreadPool() {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			shutdown_ = true;
		}
		cv_.notify_all();
		for (std::thread& worker : workers_) {
			if (worker.joinable()) {
				worker.join();
			}
		}
	}
	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	int thread_count() const { return threads_; }

	// Executa fn(begin, end) sobre blocos estáticos e contíguos de [0, n).
	// Bloqueia até o fim. Com threads_ == 1 (ou n < kThreadPoolMinCities),
	// roda serial na própria chamadora.
	void parallel_for(std::size_t n,
			const std::function<void(std::size_t, std::size_t)>& fn) {
		if (n == 0) {
			return;
		}
		if (threads_ <= 1 || n < kThreadPoolMinCities) {
			fn(0, n);
			return;
		}
		const std::size_t chunk_count =
			std::min(static_cast<std::size_t>(threads_), n);
		{
			std::lock_guard<std::mutex> lock(mutex_);
			fn_ = &fn;
			n_ = n;
			chunk_count_ = chunk_count;
			next_chunk_ = 0;
			remaining_ = chunk_count;
			++generation_;
		}
		cv_.notify_all();
		run_chunks();  // a chamadora também processa blocos
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [this] { return remaining_ == 0; });
			fn_ = nullptr;
		}
	}

private:
	// Pega blocos contíguos até esgotar e executa cada um. Retorna quando não
	// há mais blocos (a thread chamadora e os workers chamam isto).
	void run_chunks() {
		for (;;) {
			std::size_t chunk = 0;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if (next_chunk_ >= chunk_count_) {
					return;
				}
				chunk = next_chunk_++;
			}
			run_chunk(chunk);
			{
				std::lock_guard<std::mutex> lock(mutex_);
				--remaining_;
			}
			cv_.notify_all();
		}
	}
	void run_chunk(std::size_t chunk) {
		const std::size_t base = n_ / chunk_count_;
		const std::size_t rem = n_ % chunk_count_;
		const std::size_t begin = chunk * base + (chunk < rem ? chunk : rem);
		const std::size_t end = begin + base + (chunk < rem ? 1 : 0);
		if (end > begin) {
			(*fn_)(begin, end);
		}
	}
	void worker_loop() {
		std::size_t seen_generation = 0;
		for (;;) {
			{
				std::unique_lock<std::mutex> lock(mutex_);
				cv_.wait(lock, [this, &seen_generation] {
					return shutdown_ || generation_ > seen_generation;
				});
				if (shutdown_) {
					return;
				}
				seen_generation = generation_;
			}
			run_chunks();
		}
	}

	int threads_;
	std::vector<std::thread> workers_;
	std::mutex mutex_;
	std::condition_variable cv_;
	bool shutdown_ = false;
	std::size_t n_ = 0;
	std::size_t chunk_count_ = 0;
	std::size_t next_chunk_ = 0;
	std::size_t remaining_ = 0;
	std::size_t generation_ = 0;
	const std::function<void(std::size_t, std::size_t)>* fn_ = nullptr;
};

} // namespace ogs
