extends RefCounted

## Digest canônico de trajetória dos harnesses de 365 d (C2b1-fix, D-105).
##
## POR QUE EXISTE: o hash antigo (`"%d:%.2f:%d" % [day, money, bankruptcies]`)
## cobria três escalares por dia e era cego para tudo o que a política monetária
## mudou: dinheiro na 4ª casa, M0, estoque da G2, crédito da safra, comida. Quatro
## mundos materialmente diferentes (C2a, E1, E2, C2b1) saíam com o MESMO hash
## (`f7631ba0…`). Ver `docs/economy/inv_c2b1_gate/README.md`.
##
## Um único dono do formato: `tools/c1_world_ai_365.gd` (também usado nas
## rodadas C2a/C2b1 do relatório, só muda `--out`) e `tools/g2_world_ai_365.gd`.
## Isto é MEDIÇÃO: lê o status/consultas do núcleo, não move dinheiro nem muda
## regra.
##
## Campos por dia, nesta ordem, separados por `:` (dígitos = casas decimais):
##   1. `day`               — dia do mundo
##   2. `money_total` (6)   — caixa total do mundo (conservação)
##   3. `bankruptcies`      — falências acumuladas
##   4. `food_ratio_sum` (9) — soma por nação de consumido/necessidade do dia
##                             (a base do portão de comida do M2)
##   5. `m0_total` (6)      — base monetária medida (C2a/D-103): soma do M0 por
##                             moeda do status do banco central
##   6. `private_stock_t` (6) — estoque PRIVADO armazenado (G2/D-101), soma por
##                             tipo do status de armazenagem
##   7. `credit_capacity` (9) — termo de crédito da safra (D-099): soma por nação
##                             do fator `1 / (1 + r · 60/365)` na taxa vigente da
##                             moeda. 179,0 = mundo inteiro com taxa zero;
##                             1,0 por nação sem moeda no estado.
##
## NOTAS DE HONESTIDADE:
## - Não existe livro de empréstimos até a C4: `credit_capacity` é o TERMO que o
##   envelope aplicaria com a taxa do dia, não um valor emprestado. Com a D-105
##   o núcleo aplica 1,0 (sem juro) porque não há credor com contraparte no
##   balanço; o termo continua no hash porque a taxa é estado do mundo e é o que
##   a C4 vai usar.
## - Os 60 dias espelham `kLossDaysBeforeClose` (D-099), o horizonte do envelope
##   em `agriculture.cpp`. Duplicação declarada: o número é medido aqui, não
##   lido do núcleo (não há getter).
## - Sem `get_central_bank_status` (mundos anteriores à C2a) a ausência é
##   declarada: `m0_total` e `credit_capacity` entram como -1.

const CREDIT_SPAN_DAYS := 60.0

const FIELDS := [
	"day",
	"money_total(6)",
	"bankruptcies",
	"food_ratio_sum(9)",
	"m0_total(6)",
	"private_stock_t(6)",
	"credit_capacity(9)",
]


## Razão consumido/necessidade por nação do dia, a partir das colunas leves do
## status (`light.p_need` / `light.p_consumed` por cidade). Mesma conta do portão
## de comida: nação sem necessidade declarada vale 1,0.
static func food_ratios(status: Dictionary) -> Dictionary:
	var light: Dictionary = status.get("light", {})
	var need: PackedFloat64Array = light.get("p_need", PackedFloat64Array())
	var consumed: PackedFloat64Array = light.get("p_consumed", PackedFloat64Array())
	var cities: Array = status.get("cities", [])
	if cities.is_empty() or need.size() < cities.size() \
			or consumed.size() < cities.size():
		return {}
	var by_nation: Dictionary = {}
	for slot in cities.size():
		var tag := String((cities[slot] as Dictionary).get("nation", ""))
		if tag == "":
			continue
		if not by_nation.has(tag):
			by_nation[tag] = {"need": 0.0, "consumed": 0.0}
		by_nation[tag]["need"] = float(by_nation[tag]["need"]) + float(need[slot])
		by_nation[tag]["consumed"] = float(by_nation[tag]["consumed"]) \
			+ float(consumed[slot])
	var out: Dictionary = {}
	for tag in by_nation:
		var total := float(by_nation[tag]["need"])
		out[tag] = float(by_nation[tag]["consumed"]) / total if total > 0.0 else 1.0
	return out


## Soma das razões por nação. Ordena as chaves para não depender da ordem de
## inserção do dicionário (determinismo do hash).
static func food_sum(ratios: Dictionary) -> float:
	var tags: Array = ratios.keys()
	tags.sort()
	var total := 0.0
	for tag in tags:
		total += float(ratios[tag])
	return total


## Totais do dia que entram no hash: M0, estoque privado e termo de crédito.
## Ausência é declarada como -1 (não se inventa zero).
static func totals(sim: Object) -> Dictionary:
	var out := {"m0": -1.0, "private_stock_t": -1.0, "credit_capacity": -1.0}
	if sim != null and sim.has_method("get_central_bank_status"):
		var bank: Dictionary = sim.get_central_bank_status()
		var totals_block: Dictionary = bank.get("totals", {})
		if totals_block.has("m0"):
			out["m0"] = float(totals_block.get("m0", 0.0))
		var capacity := 0.0
		var counted := 0
		var currencies: Dictionary = bank.get("currencies", {})
		for code: Variant in currencies:
			var row: Dictionary = currencies[code]
			var policy: Dictionary = row.get("policy", {})
			var rate := float(policy.get("rate", 0.0))
			var factor := 1.0 / (1.0 + rate * CREDIT_SPAN_DAYS / 365.0)
			for nation: Variant in Array(row.get("nations", [])):
				capacity += factor
				counted += 1
		if counted > 0:
			out["credit_capacity"] = capacity
	if sim != null and sim.has_method("get_storage_status"):
		var storage: Dictionary = sim.get_storage_status()
		var storage_totals: Dictionary = storage.get("totals", {})
		if storage_totals.has("private_stock_t"):
			out["private_stock_t"] = float(
				storage_totals.get("private_stock_t", 0.0))
	return out


## Linha canônica do dia. `status` é o `get_status()` do dia fechado; `sim` é a
## instância do núcleo (para M0/estoque/crédito).
static func line(day: int, status: Dictionary, sim: Object) -> String:
	var day_totals := totals(sim)
	return "%d:%.6f:%d:%.9f:%.6f:%.6f:%.9f" % [
		day,
		float(status.get("money_total", 0.0)),
		int(status.get("bankruptcies", -1)),
		food_sum(food_ratios(status)),
		float(day_totals["m0"]),
		float(day_totals["private_stock_t"]),
		float(day_totals["credit_capacity"]),
	]


## SHA-256 das linhas (que já vêm na ordem dos dias).
static func sha256(lines: PackedStringArray) -> String:
	var ctx := HashingContext.new()
	if ctx.start(HashingContext.HASH_SHA256) != OK:
		return ""
	ctx.update("|".join(lines).to_utf8_buffer())
	return ctx.finish().hex_encode()
