#pragma once
// A3 - fila de comandos (docs/plans/perf_pop_financas_plano.md, §A3;
// racional e contrato em docs/economy/refactor/A3_commands.md).
//
// Toda acao do jogador ou da IA que muda o mundo FORA do tick vira um Command.
// O comando carrega so dados (nenhum tipo do Godot): tipo, dia de emissao,
// ordem de emissao (`seq`) e parametros em slots fixos. A fila nao conhece a
// simulacao: quem aplica e a fachada (EconomySim), no inicio do dia e/ou sob
// demanda, SEMPRE na ordem de emissao - o desempate e o `seq`, estavel por
// construcao.
//
// Contrato:
//   * `push()` e a unica escrita na fila; usa so a trava propria da fila
//     (nunca o mutex recursivo da fachada).
//   * `take_pending()` devolve os pendentes em ordem de emissao e esvazia a
//     fila: o aplicador e sempre um so (a thread do dia ou a main thread).
//   * `log()` guarda o que ja foi aplicado; o log e o material do replay.
//   * `last_seq()` + `take_by_seq()` sao o caminho da IA no C++: ela emite o
//     proprio comando (e sabe o `seq` que recebeu), retira SO ele da fila e o
//     aplica na ordem de emissao. Assim ela nunca aplica nem descarta um
//     comando de terceiros (a UI) que chegou no meio.
//   * Nada aqui muda o resultado numerico do dia: a aplicacao no inicio do dia
//     seguinte e o MESMO estado que a aplicacao imediata entre dias (nenhum
//     tick acontece no meio) - por isso a equivalencia com o golden e 0 diff.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ogs {

enum class CommandType : int {
	None = 0,
	// Jogador / IA.
	Invest,
	Build,
	SpendGovernment,
	PriceShock,
	SetSuitability,
	SetDeposit,
	SetExternalDemand,
	SetExternalIncomeIndex,
	SetProductivityIndex,
	SetIncomeGini,
	SetMinimumWage,
	SetWageRigidity,
	SetTariff,
	SetCurrencyRate,
	ExpandPort,
	SetPortCapacity,
	SetMarketStock,
	SetMarketPrice,
	FleetBuy,
	FundGroup,
	GroupInvest,
	SetFdiPolicy,
	SetSectorIncentive,
	CreateStateCompany,
	Privatize,
	Nationalize,
	UrbanExpand,
	UrbanDensify,
	UrbanSocialHousing,
	SetServiceCapacity,
	SetServiceBusinessState,
	// D-086: aprofundar/expandir mina (obra física que abre o estágio seguinte).
	DeepenMine,
	// B5: obra de infraestrutura da região/estado (estrada/energia/água).
	InfraBuild,
	// B5: conceder mina/fazenda a grupo (nacional/estrangeiro) com royalty.
	Concede,
	// G2: obra de armazém agregado (silo/câmara fria/... por cidade+tipo+dono).
	StorageBuild,
	// G2: compra/liberação de estoque guardado (governo/privado).
	StorageTrade,
	// C2b2 (D-106): regime cambial por moeda e padrão monetário do mundo.
	// Sempre pela fila A3 — troca de regime/padrão nunca é automática.
	SetExchangeRegime,
	SetMonetaryStandard,
	// C3 (D-107): crédito de empresa. `SetBankCredit` liga/desliga a mecânica
	// (o default do jogo é desligado) e `GroupBorrow` pede crédito para um
	// grupo — sempre pela fila A3, como todo comando de política.
	SetBankCredit,
	GroupBorrow,
	// D-125: eficiência de produção. `ChangeProduct` troca o produto da planta
	// (transição sem produção + retenção), `SetRefineryMode` troca o modo da
	// refinaria (padrão/diesel/querosene) e `SetBuildingEquipment` liga/ajusta o
	// trator/máquina (0..1). Sempre pela fila A3, como toda mutação.
	ChangeProduct,
	SetRefineryMode,
	SetBuildingEquipment,
};

// Nome estavel do tipo (log/replay em JSON). `nullptr` para tipo invalido.
const char* command_type_name(CommandType type);
// Inverso de `command_type_name`; false se o nome nao existe.
bool command_type_from_name(const std::string& name, CommandType& out);

// Capacidade dos slots de parametro. Nao e um limite arbitrario: cada tipo usa
// um numero fixo de slots (ver `command_type_name`/A3_commands.md), entao
// aumentar aqui e mudar o formato do log - nao faca sem migrar o replay.
constexpr int kCommandStrings = 6;
constexpr int kCommandDoubles = 6;
constexpr int kCommandInts = 2;

// Teto do log de comandos aplicados (ver `CommandQueue::log`).
constexpr std::size_t kMaxCommandLog = 200000;

struct Command {
	CommandType type = CommandType::None;
	int day = 0;       // dia em que o emissor pediu o comando
	int apply_day = 0; // dia em que foi aplicado (0 = ainda na fila)
	uint64_t seq = 0;  // ordem de emissao, 1-based (desempate estavel)
	std::string s[kCommandStrings];
	double v[kCommandDoubles] = {};
	int i[kCommandInts] = {};
	bool b = false;
};

class CommandQueue {
public:
	// Enfileira e devolve a ordem de emissao (1-based).
	uint64_t push(Command cmd);
	// Move os pendentes, ja em ordem de emissao, e esvazia a fila.
	std::vector<Command> take_pending();
	// Ultimo `seq` emitido (0 = nenhum desde o reset). Combinado com
	// `take_by_seq`, e o caminho da IA: ela emite o proprio comando, retira SO
	// ele e o aplica na ordem de emissao, sem roubar comandos de terceiros (a
	// UI) que entraram na fila no meio.
	uint64_t last_seq() const;
	// Remove e devolve SO o comando com aquele `seq`, se ainda pendente;
	// preserva os demais na ordem de emissao. false = ele nao estava (mais) na
	// fila, e `out` fica intacto.
	bool take_by_seq(uint64_t seq, Command& out);
	std::size_t pending_size() const;
	void clear_pending();
	// Marca no log o comando que acabou de ser aplicado. O log para de crescer
	// em `kMaxCommandLog` entradas (proteção de memória: ~200 B × 200 mil ≈ 40
	// MB); a sessão continua e `truncated()` denuncia o corte.
	void log(const Command& cmd);
	std::vector<Command> log_entries() const;
	std::size_t log_size() const;
	bool truncated() const;
	void clear_log();
	// Novo mundo/load: pendentes e log deixam de valer.
	void reset();

private:
	mutable std::mutex mutex_;
	std::vector<Command> pending_;
	std::vector<Command> log_;
	bool truncated_ = false;
	uint64_t next_seq_ = 0;
};

} // namespace ogs
