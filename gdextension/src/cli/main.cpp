// opengs_sim_cli — roda N dias SÓ do núcleo da simulação (A1).
//
// Sem Godot, sem IA, sem godot-cpp: o mundo vem de um snapshot JSON
// (`{"definitions": ..., "save": ...}`, ver tools/export_world_snapshot.gd) e
// sai no MESMO formato depois dos dias pedidos. É o lado C++ da comparação de
// equivalência contra o replay do Godot
// (tools/world_equivalence.gd + tools/world_equivalence_compare.py).
//
//   opengs_sim_cli --snapshot=<in.json> --days=<N> --out=<out.json> [--threads=<T>]
//
// Contrato:
//   * a ordem do dia é `ogs::tick_day` — este executável NÃO duplica tick;
//   * `definitions_from` ANTES de `from_value` (o save referencia os produtos,
//     receitas e necessidades já carregados);
//   * nada de `seed_industry`: o bootstrap industrial vive na fachada
//     Godot e o snapshot traz o mundo já semeado. Sem ele, o arquivo é recusado;
//   * a saída tem EXATAMENTE duas chaves — `definitions` e `save` — porque o
//     arquivo é comparado campo a campo contra o replay do Godot;
//   * stdout limpo: o resumo de execução vai para stderr.
//
// Códigos de saída: 0 ok, 1 violação de invariante, 2 erro de uso/entrada.

#include "sim/save_definitions.h"
#include "sim/save_state.h"
#include "sim/save_value.h"
#include "sim/sim_core.h"
#include "sim/sim_day.h"
#include "sim/world_state.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

struct Options {
	std::string snapshot;
	std::string out;
	int days = -1;     // -1 = ausente
	int threads = 0;   // mesmo padrão do replay do Godot: núcleo decide
};

void print_usage() {
	std::cerr << "uso: opengs_sim_cli --snapshot=<in.json> --days=<N> "
	             "--out=<out.json> [--threads=<T>]\n";
}

// Falha de uso/entrada: mensagem legível no stderr e exit 2.
[[noreturn]] void fail_usage(const std::string& message) {
	std::cerr << "[cli] erro: " << message << "\n";
	print_usage();
	std::exit(2);
}

// Inteiro não-negativo do valor de uma flag; erro de uso se não for número.
int parse_non_negative(const std::string& flag, const std::string& text) {
	if (text.empty()) {
		fail_usage("valor vazio em " + flag);
	}
	std::size_t consumed = 0;
	long long value = 0;
	try {
		value = std::stoll(text, &consumed, 10);
	} catch (const std::exception&) {
		fail_usage("valor não numérico em " + flag + ": " + text);
	}
	if (consumed != text.size()) {
		fail_usage("valor não numérico em " + flag + ": " + text);
	}
	if (value < 0) {
		fail_usage("valor negativo não faz sentido em " + flag + ": " + text);
	}
	if (value > 2147483647LL) {
		fail_usage("valor grande demais em " + flag + ": " + text);
	}
	return static_cast<int>(value);
}

Options parse_args(int argc, char** argv) {
	Options options;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		const std::string::size_type equal = arg.find('=');
		const std::string flag = equal == std::string::npos
			? arg
			: arg.substr(0, equal);
		const std::string value = equal == std::string::npos
			? std::string()
			: arg.substr(equal + 1);
		if (flag == "--snapshot") {
			options.snapshot = value;
		} else if (flag == "--out") {
			options.out = value;
		} else if (flag == "--days") {
			options.days = parse_non_negative(flag, value);
		} else if (flag == "--threads") {
			options.threads = parse_non_negative(flag, value);
		} else if (flag == "--help" || flag == "-h") {
			print_usage();
			std::exit(0);
		} else {
			fail_usage("argumento desconhecido: " + arg);
		}
	}
	if (options.snapshot.empty()) {
		fail_usage("--snapshot=<in.json> é obrigatório");
	}
	if (options.out.empty()) {
		fail_usage("--out=<out.json> é obrigatório");
	}
	if (options.days < 0) {
		fail_usage("--days=<N> é obrigatório (N >= 0)");
	}
	return options;
}

std::string format_ms(double value) {
	std::ostringstream text;
	text.setf(std::ios::fixed);
	text.precision(1);
	text << value;
	return text.str();
}

std::string format_money(double value) {
	std::ostringstream text;
	text.setf(std::ios::fixed);
	text.precision(2);
	text << value;
	return text.str();
}

double ms_since(std::chrono::steady_clock::time_point start) {
	const std::chrono::duration<double, std::milli> elapsed =
		std::chrono::steady_clock::now() - start;
	return elapsed.count();
}

// Grava o arquivo inteiro; erro de escrita não é silencioso.
bool write_text_file(const std::string& path, const std::string& text,
	std::string& error) {
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	if (!file) {
		error = "não foi possível criar o arquivo: " + path;
		return false;
	}
	file << text;
	file.flush();
	if (!file) {
		error = "falha ao gravar o arquivo: " + path;
		return false;
	}
	return true;
}

} // namespace

int main(int argc, char** argv) {
	const Options options = parse_args(argc, argv);
	const std::chrono::steady_clock::time_point start =
		std::chrono::steady_clock::now();

	// 1. Snapshot: abrir, parsear e exigir os dois blocos do contrato.
	ogs::save::Value snapshot;
	std::string error;
	if (!ogs::save::parse_json_file(options.snapshot, snapshot, error)) {
		std::cerr << "[cli] erro: snapshot ilegível (" << options.snapshot
			<< "): " << error << "\n";
		return 2;
	}
	if (snapshot.get_type() != ogs::save::Value::DICTIONARY) {
		std::cerr << "[cli] erro: o snapshot não é um objeto JSON: "
			<< options.snapshot << "\n";
		return 2;
	}
	if (!snapshot.has("definitions") || !snapshot.has("save")) {
		std::cerr << "[cli] erro: o snapshot precisa das chaves "
			<< "\"definitions\" e \"save\": " << options.snapshot << "\n";
		return 2;
	}

	// 2. Mundo e núcleo.
	ogs::WorldState ws;
	ogs::SimCore core(ws);
	core.set_sim_threads(options.threads);

	// 3. Definições primeiro (o save referencia produtos/receitas/necessidades),
	// depois o estado dinâmico.
	ogs::save::definitions_from(ws, snapshot["definitions"]);
	ogs::save::from_value(ws, core, snapshot["save"]);

	// 4. O snapshot tem de ser o estado de partida real do jogo: mundo ativo e
	// bootstrap industrial já executado (ele vive na fachada Godot).
	if (!ws.active || !ws.industry_seeded) {
		std::cerr << "[cli] erro: snapshot não é um mundo inicializado"
			<< " (active=" << (ws.active ? "true" : "false")
			<< ", industry_seeded=" << (ws.industry_seeded ? "true" : "false")
			<< "). O bootstrap industrial roda na fachada Godot"
			<< " (seed_industry) e precisa estar no snapshot.\n";
		return 2;
	}

	// 5. N dias da ordem do dia do núcleo, parando na primeira violação.
	for (int day = 0; day < options.days; ++day) {
		ogs::tick_day(ws, core);
		if (!ws.invariant_violations.empty()) {
			std::cerr << "[cli] invariante violado no dia " << ws.day << " ("
				<< ws.invariant_violations.size() << " ocorrência(s)):\n";
			for (const std::string& violation : ws.invariant_violations) {
				std::cerr << "  - " << violation << "\n";
			}
			return 1;
		}
	}

	const double ms_total = ms_since(start);

	// 6. Saída: exatamente `definitions` + `save`, nessa ordem.
	ogs::save::Value out = ogs::save::Value::make_object();
	out["definitions"] = ogs::save::definitions_to(ws);
	out["save"] = ogs::save::to_value(ws, core);
	if (!write_text_file(options.out, ogs::save::dump_json(out, 2), error)) {
		std::cerr << "[cli] erro: " << error << "\n";
		return 2;
	}

	// 7. Resumo numa linha no stderr (stdout fica limpo).
	const double ms_day = options.days > 0
		? ms_total / static_cast<double>(options.days)
		: 0.0;
	std::cerr << "[cli] dias=" << options.days
		<< " threads=" << core.sim_threads()
		<< " day=" << ws.day
		<< " cidades=" << ws.cities.size()
		<< " ms_total=" << format_ms(ms_total)
		<< " ms_dia=" << format_ms(ms_day)
		<< " money_total=" << format_money(core.money_total())
		<< "\n";
	return 0;
}
