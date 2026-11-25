extends RefCounted
class_name GroupInvestor

## LEGADO (D-131 fase C): o jogo roda o investidor de grupo em C++
## (`gdextension/src/sim/ai/ai_group.cpp`). Este porte GDScript não é chamado
## pelo jogo nem pelo caminho do cérebro: só ferramentas/testes históricos
## (`investment_test.gd`, `balance_review.gd`) o instanciam. `CASH_SHARE = 0,5`
## abaixo é a regra ANTIGA e não é critério de decisão do jogo.

## F9 — decisão de investimento por GRUPO/EMPRESA (docs §14).
##
## Cada grupo decide em cadência BAIXA: um grupo de índice `i` decide nos dias em
## que `day % interval_days == i % interval_days`. Nunca por tick.
##
## Fatiamento por frame (F9-perf): `queue_day()` — chamado no plano da IA — só
## agenda o dia; `run_slice(budget_us)` faz o trabalho em pedaços, rodado pelo
## `pump()` da IA nos frames seguintes. Nem a virada do dia nem um frame de pump
## pagam uma varredura inteira: tanto a montagem da base de um recurso quanto a
## avaliação dos candidatos respeitam o orçamento.
##
## Custo por decisão sem varrer 834 cidades: por dia, cada RECURSO tem uma base
## única — cidades com sinal de mercado (escassez para comida; escassez/venda/
## demanda para os demais), margem unitária > 0 e capital por capacidade —,
## calculada uma vez e reaproveitada por todos os grupos que a pedem. A base é
## independente do grupo; o que resta por grupo é o risco (tarifa, confiança,
## joint venture), a política de IDE e o caixa. Nada de `best_recipe` por
## (grupo × cidade × recurso).
##
## Retorno esperado × risco: a margem unitária (preço de mercado − custo
## variável da melhor receita) vira retorno diário sobre o capital; o risco
## desconta confiança da nação (nacionalização), tarifa do anfitrião (no IDE),
## abertura do setor e joint venture. Financia com o caixa do grupo.
##
## A proteção alimentar existente é preservada: enquanto a satisfação alimentar
## média do mundo estiver abaixo de `FOOD_SECURE_SATISFACTION`, o grupo só
## investe em alimento (nunca em bem de luxo/industrial).

const Templates := preload("res://core/economy/ai/industry_templates.gd")
const CultureDb := preload("res://core/culture/culture_db.gd")
const BusinessNameGenerator := preload("res://core/culture/business_name_generator.gd")

## Cadência por grupo (dias). Baixa por desenho: 30 dias.
const INTERVAL_DAYS := 30
## Caixa mínimo para abrir um ativo (abaixo disso o grupo poupa).
const MIN_CAPITAL := 6000.0
## Capacidade máxima de um ativo por decisão (t/dia).
const MAX_CAPACITY := 12.0
## Retorno diário mínimo (margem / capital) para investir, após risco.
const MIN_RETURN := 0.0018
## Satisfação alimentar média abaixo da qual só se investe em comida.
const FOOD_SECURE_SATISFACTION := 0.98
## Fração do caixa comprometida por decisão — REGRA ANTIGA (legado). O caminho
## do jogo (C++) dimensiona pelo forecast/R1, não por esta constante.
const CASH_SHARE := 0.5
## Cidades/candidatos entre duas leituras do relógio (fatiamento).
const YIELD_EVERY := 16

## Setor do bem (goods.json) → setor de nome de empresa (business_names).
const SECTOR_TO_BUSINESS := {
	"agro": "wholesale", "food": "food_service", "mining": "construction",
	"energy": "construction", "metal": "construction",
	"machinery": "construction", "chemical": "construction",
	"wood": "construction", "textile": "retail", "luxury": "retail",
	"packaging": "retail", "electronics": "it",
	"transport": "freight_logistics", "military": "professional_services",
	"construction": "construction", "primary": "wholesale",
	"industry": "construction", "state": "finance",
}

## Estágios do trabalho fatiado.
const STAGE_IDLE := 0
const STAGE_PREPARE := 1
const STAGE_DECIDE := 2

var _sim: Object
var _geo: Object
var _groups: Array = []
var _decisions: Array = []
var _sector_of: Dictionary = {}
var _sector_ready := false

## Dia em andamento: estágio, status e fila de grupos (índices do ranking).
var _stage := STAGE_IDLE
var _day := 0
var _status: Dictionary = {}
var _food_secure := true
var _queue: Array[int] = []
var _queue_pos := 0
var _prepare_step := 0

## Sinais do dia (confiança por nação, políticas de IDE, incentivos).
var _signals: Dictionary = {}
var _policy_cache: Dictionary = {}
var _tariff_cache: Dictionary = {}
## Índice "nação|recurso" → {id do grupo: true} das construções do status.
var _incumbents: Dictionary = {}
var _incumbents_ready := false

## Sinais leves por cidade (S7-c): arrays planos de mercados/POP/incumbentes do
## bloco `status["light"]`, indexados pela posição da cidade em `status["cities"]`.
## A varredura diária de TODAS as cidades lê daqui (não dos dicionários pesados).
var _light: Dictionary = {}
var _light_product_count := 0

## Base por recurso (independente do grupo), válida para o dia corrente:
## `_base[r]` = candidatos já aceitos, `_base_cursor[r]` = cidade corrente,
## `_base_done[r]` = base completa, `_base_context[r]` = template pré-lido.
var _base: Dictionary = {}
var _base_cursor: Dictionary = {}
var _base_done: Dictionary = {}
var _base_context: Dictionary = {}

## Grupo corrente: recursos na ordem de decisão e cursor do recurso/candidato.
var _current := -1
var _resources: Array[String] = []
var _res_index := 0
var _cursor := 0
var _best: Dictionary = {}
var _best_score := 0.0


func setup(sim: Object, geo: Object) -> void:
	_sim = sim
	_geo = geo
	refresh_groups()


## Recarrega o ranking de grupos do núcleo (sob demanda: não é status do tick).
## Usa o ranking LEVE quando o núcleo o oferece: a decisão só precisa de id,
## origem, setor, caixa e regime, nunca da varredura de ativos.
func refresh_groups() -> void:
	_groups = []
	if _sim == null:
		return
	if _sim.has_method("get_group_ranking"):
		_groups = _sim.get_group_ranking()
	elif _sim.has_method("get_groups"):
		_groups = _sim.get_groups()


func get_decisions() -> Array:
	return _decisions.duplicate(true)


func clear_decisions() -> void:
	_decisions.clear()


## Devolve e esvazia as decisões produzidas (o orquestrador drena a cada pump).
func take_decisions() -> Array:
	if _decisions.is_empty():
		return []
	var out := _decisions
	_decisions = []
	return out


## Nomeia os grupos pela cultura de origem (business_name_generator.gd). O
## `place_by_nation` (opcional) dá o nome da cidade-sede usado nos padrões com
## `{place}`. Garante unicidade dentro do lote; o núcleo só armazena.
func name_groups(place_by_nation: Dictionary = {}) -> int:
	if _sim == null:
		return 0
	if _groups.is_empty():
		refresh_groups()
	if _groups.is_empty():
		return 0
	if not BusinessNameGenerator.load_data() or not CultureDb.load_data():
		return 0
	var generator := BusinessNameGenerator.new()
	var names: Dictionary = {}
	var used: Dictionary = {}
	for group: Dictionary in _groups:
		var id := String(group.get("id", ""))
		if id.is_empty():
			continue
		var origin := String(group.get("origin_nation", ""))
		var sector := String(group.get("sector", ""))
		var culture := CultureDb.national_culture(origin)
		var business_sector := String(SECTOR_TO_BUSINESS.get(sector, "wholesale"))
		var place := String(place_by_nation.get(origin, ""))
		var seed_value := absi(hash(id)) % 100000
		var name := ""
		if culture != "" and BusinessNameGenerator.has_sector(
				BusinessNameGenerator.language_for(culture), business_sector):
			name = generator.generate(culture, business_sector, place, seed_value)
		if name.is_empty():
			# Fallback declarado: rótulo do setor + nação. Nunca inventa cultura.
			name = "%s %s" % [business_sector.capitalize(), origin]
		var key := name.to_lower()
		if used.has(key):
			var suffix := 2
			while used.has("%s %d" % [key, suffix]):
				suffix += 1
			name = "%s %d" % [name, suffix]
			key = name.to_lower()
		used[key] = true
		names[id] = name
	if names.is_empty():
		return 0
	return int(_sim.set_group_names(names))


## Síncrono (ferramentas, testes e `EconomicAI.run()`): agenda e executa o dia
## inteiro de uma vez. O jogo usa `queue_day()` + `run_slice()`.
func run_day(day: int, status: Dictionary) -> void:
	queue_day(day, status)
	drain()


## Agenda o dia: barato por contrato (nenhuma varredura de cidade/receita).
## Chamado no plano da IA, no frame da virada.
func queue_day(day: int, status: Dictionary) -> void:
	if is_busy():
		# Garantia de ordem: um dia novo nunca começa com a fila do anterior
		# pela metade (no jogo o dia dura 60 s; em ferramentas, run() é síncrono).
		drain()
	_day = day
	_status = status
	_stage = STAGE_PREPARE
	_prepare_step = 0
	_queue.clear()
	_queue_pos = 0
	_prepare_caches()
	_current = -1
	_resources.clear()
	_res_index = 0
	_cursor = 0
	_best = {}
	_best_score = MIN_RETURN


## Executa até esvaziar a fila do dia (caminho síncrono de testes/ferramentas).
func drain() -> void:
	while is_busy():
		run_slice(1 << 30)


## Trabalha até `budget_us` e devolve `true` quando o dia terminou. Chamado pelo
## `pump()` da IA; nunca no frame da virada.
func run_slice(budget_us: int) -> bool:
	if _stage == STAGE_IDLE:
		return true
	var start := Time.get_ticks_usec()
	var deadline := start + budget_us
	while _stage != STAGE_IDLE:
		if _stage == STAGE_PREPARE:
			_prepare_slice()
			if Time.get_ticks_usec() >= deadline:
				break
			continue
		if _current < 0:
			_current = _next_queued()
			if _current < 0:
				_stage = STAGE_IDLE
				break
			_begin_group(_current)
			if _current < 0:
				# Grupo sem capital para decidir: passa para o próximo.
				continue
		_advance_group(deadline)
		if _current < 0:
			# Grupo concluído: aproveita o orçamento restante no próximo.
			if Time.get_ticks_usec() >= deadline:
				break
			continue
		break
	return _stage == STAGE_IDLE


func is_busy() -> bool:
	return _stage != STAGE_IDLE


## Melhor oportunidade para um grupo (exposta para o teste de aceite). Devolve
## {} quando nada compensa. Não executa. Percorre as cidades informadas — é o
## caminho de teste/ferramenta, não o laço fatiado do jogo.
func best_opportunity(group: Dictionary, cities: Array, status: Dictionary,
		food_secure: bool = true) -> Dictionary:
	if float(group.get("cash", 0.0)) < MIN_CAPITAL:
		return {}
	_ensure_sector_map()
	_status = status
	_incumbents_ready = false
	_load_signals(status)
	var best: Dictionary = {}
	var best_score := MIN_RETURN
	for resource in _resources_for(String(group.get("sector", "")), food_secure):
		var context := _resource_context(resource)
		if context.is_empty():
			continue
		var base: Array = []
		for city_index in cities.size():
			_append_candidate(base, city_index, cities[city_index], resource, context)
		for entry: Dictionary in base:
			var candidate := _score_candidate(group, entry)
			if candidate.is_empty():
				continue
			if float(candidate["score"]) > best_score:
				best = candidate
				best_score = float(candidate["score"])
	return best


# --- Preparação do dia -------------------------------------------------------

func _prepare_caches() -> void:
	_signals = {}
	_policy_cache.clear()
	_tariff_cache.clear()
	_incumbents.clear()
	_incumbents_ready = false
	_base.clear()
	_base_cursor.clear()
	_base_done.clear()
	_base_context.clear()


## Um passo de preparação por fatia: (0) status/ranking + fila, (1) sinais/dia.
func _prepare_slice() -> void:
	if _prepare_step == 0:
		if _status.is_empty() and _sim != null \
				and _sim.has_method("get_status"):
			# Ferramentas chamam `plan()` sem o resumo do tick: busca o status
			# aqui (fora do frame da virada), nunca no plano.
			_status = _sim.get_status()
		refresh_groups()
		_queue.clear()
		var phase := _day % INTERVAL_DAYS
		for index in _groups.size():
			if index % INTERVAL_DAYS == phase:
				_queue.append(index)
		_prepare_step = 1
		return
	_ensure_sector_map()
	_food_secure = _compute_food_secure(_status)
	_load_signals(_status)
	_prepare_step = 2
	_stage = STAGE_DECIDE


func _next_queued() -> int:
	while _queue_pos < _queue.size():
		var index: int = _queue[_queue_pos]
		_queue_pos += 1
		if index >= 0 and index < _groups.size():
			return index
	return -1


func _begin_group(index: int) -> void:
	var group: Dictionary = _groups[index]
	if float(group.get("cash", 0.0)) < MIN_CAPITAL:
		# Sem capital não há decisão: evita preparar a base do setor à toa.
		_current = -1
		return
	_resources = _resources_for(String(group.get("sector", "")), _food_secure)
	_res_index = 0
	_cursor = 0
	_best = {}
	_best_score = MIN_RETURN


## Recursos na ordem de decisão: a cesta alimentar sempre; o setor do grupo só
## quando o mundo está com a comida segura. Duplicatas de comida são puladas,
## como no laço original (cidade × recurso).
func _resources_for(sector: String, food_secure: bool) -> Array[String]:
	var resources: Array[String] = []
	for resource: String in Templates.FOOD_RESOURCES:
		resources.append(resource)
	if not food_secure:
		return resources
	var sector_resources: Array = _sector_of.get(sector, [])
	for value: Variant in sector_resources:
		var resource := String(value)
		if Templates.FOOD_RESOURCES.has(resource):
			continue
		resources.append(resource)
	return resources


## Avalia um grupo até o fim (ou até o orçamento). Ao terminar, executa a
## melhor oportunidade e libera `_current`.
func _advance_group(deadline_us: int) -> void:
	var group: Dictionary = _groups[_current]
	var ticks := 0
	while _res_index < _resources.size():
		var resource: String = _resources[_res_index]
		if not _base_ready(resource):
			# Base do recurso montada em fatias: nunca um frame com a varredura.
			_build_base_slice(resource, deadline_us)
			if not _base_ready(resource):
				return
		var base: Array = _base[resource]
		var size := base.size()
		while _cursor < size:
			var candidate := _score_candidate(group, base[_cursor])
			_cursor += 1
			if not candidate.is_empty() and float(candidate["score"]) > _best_score:
				_best = candidate
				_best_score = float(candidate["score"])
			ticks += 1
			if ticks % YIELD_EVERY == 0 and Time.get_ticks_usec() >= deadline_us:
				return
		_res_index += 1
		_cursor = 0
		if Time.get_ticks_usec() >= deadline_us:
			return
	_finish_group()


func _finish_group() -> void:
	if not _best.is_empty():
		_execute(_groups[_current], _best)
	_current = -1
	_best = {}
	_best_score = MIN_RETURN


func _base_ready(resource: String) -> bool:
	return bool(_base_done.get(resource, false))


## Continua a base do recurso até o orçamento. A primeira chamada pré-lê o
## template e registra o contexto; recurso sem template fecha na hora.
func _build_base_slice(resource: String, deadline_us: int) -> void:
	if not _base_context.has(resource):
		_base_context[resource] = _resource_context(resource)
		_base[resource] = []
		_base_cursor[resource] = 0
		if (_base_context[resource] as Dictionary).is_empty():
			_base_done[resource] = true
			return
	var context: Dictionary = _base_context[resource]
	var cities: Array = _status.get("cities", [])
	var out: Array = _base[resource]
	var index: int = int(_base_cursor.get(resource, 0))
	var ticks := 0
	while index < cities.size():
		_append_candidate(out, index, cities[index], resource, context)
		index += 1
		ticks += 1
		if ticks % YIELD_EVERY == 0 and Time.get_ticks_usec() >= deadline_us:
			break
	_base_cursor[resource] = index
	if index >= cities.size():
		_base_done[resource] = true


## Parâmetros do template pré-lidos uma vez por recurso/dia.
func _resource_context(resource: String) -> Dictionary:
	if not Templates.has(resource):
		return {}
	var template: Dictionary = Templates.all_templates().get(resource, {})
	if template.is_empty():
		return {}
	var product := int(template.get("output", -1))
	if product < 0:
		return {}
	return {
		"product": product,
		"is_food": Templates.FOOD_RESOURCES.has(resource),
		# Custo puramente interno (jobs × salário + opex): piso do custo
		# variável. Usado para descartar candidato sem montar receita.
		"base_unit": float(template.get("jobs_per_ton", 0.0)) \
			* float(template.get("wage", 0.0)) \
			+ float(template.get("opex_per_ton", 0.0)),
		"input_product": int(template.get("input", -1)),
		"input_ratio": float(template.get("input_ratio", 0.0)),
		"has_recipes": template.has("recipes"),
		"capital_per_capacity": float(
			template.get("capital_per_capacity", 2000.0)),
	}


## Leitura de um campo de mercado dos sinais leves (S7-c): o MESMO double que o
## bloco pesado publicava em `city["markets"][product][field]`. Indexado por
## posição da cidade × produto. Devolve 0.0 quando o status não tem o bloco
## `light` (status montado à mão em teste/ferramenta).
func _light_market(city_index: int, product: int, field: String) -> float:
	if _light_product_count <= 0:
		return 0.0
	var values: PackedFloat64Array = _light.get(field, PackedFloat64Array())
	var at := city_index * _light_product_count + product
	if at < 0 or at >= values.size():
		return 0.0
	return values[at]


## Capacidade futura em obras (t/dia) na cidade `city_index` para `product`
## (obra nova + expansão), do bloco `light` (§15/D-060). A IA subtrai isso da
## escassez para não reconstruir a mesma lacuna enquanto uma obra está em voo.
## 0 sem bloco light ou sem obra.
func _pending_capacity(city_index: int, product: int) -> float:
	if _light_product_count <= 0:
		return 0.0
	var c_offset: PackedInt32Array = _light.get("c_offset", PackedInt32Array())
	if city_index < 0 or city_index + 1 >= c_offset.size():
		return 0.0
	var c_output: PackedInt32Array = _light.get(
		"c_output_product", PackedInt32Array())
	var c_capacity: PackedFloat64Array = _light.get(
		"c_capacity", PackedFloat64Array())
	var total := 0.0
	for index in range(int(c_offset[city_index]), int(c_offset[city_index + 1])):
		if int(c_output[index]) == product:
			total += float(c_capacity[index])
	return total


## Capacidade futura em obras por produto (soma em TODAS as cidades), do bloco
## light. Recalculada a cada `_load_signals` (1× por dia). A IA subtrai isso da
## escassez de cada candidato para não abrir mina/fazenda concorrente à mesma
## lacuna global em voo (§15/D-060).
var _pending_global := PackedFloat64Array()

func _measure_pending_global() -> void:
	_pending_global = PackedFloat64Array()
	if _light_product_count <= 0:
		return
	_pending_global.resize(_light_product_count)
	var c_output: PackedInt32Array = _light.get(
		"c_output_product", PackedInt32Array())
	var c_capacity: PackedFloat64Array = _light.get(
		"c_capacity", PackedFloat64Array())
	for index in c_output.size():
		var product := int(c_output[index])
		if product >= 0 and product < _light_product_count:
			_pending_global[product] += float(c_capacity[index])


## Alternativa de menor custo corrente na cidade a partir dos sinais leves —
## replica `Templates.best_recipe` (que lê `anchor_price` = preço de mercado)
## sem tocar em `city["markets"]`.
func _light_best_recipe(resource: String, city_index: int) -> Dictionary:
	var best: Dictionary = {}
	var best_cost := INF
	for recipe: Dictionary in Templates.recipes_for(resource):
		var cost := 0.0
		var available := true
		for item: Dictionary in recipe["inputs"]:
			var product := int(item["product"])
			if product < 0 or product >= _light_product_count:
				available = false
				break
			var price := _light_market(city_index, product, "m_price")
			if price <= 0.0:
				price = 1.0
			cost += float(item["qty"]) * price / float(recipe["yield"])
		if available and (best.is_empty() or cost < best_cost):
			best_cost = cost
			best = recipe
	return best


## Filtros EXATOS do laço antigo (sinal de mercado, template conhecido, receita
## disponível, margem > 0) e custo por capacidade. O corte por custo base é
## necessário, nunca suficiente: o insumo só encarece — filtrar por ele não
## muda quem ganha.
func _append_candidate(out: Array, city_index: int, city: Dictionary, resource: String,
		context: Dictionary) -> void:
	var product := int(context.get("product", -1))
	if product < 0 or product >= _light_product_count:
		return
	# R2/Q-014: lavouras industriais (algodão/madeira) não têm mercado próprio
	# formado — a escassez que vale é a DEMANDA DERIVADA da cadeia a jusante
	# (têxtil/celulose) já instalada na cidade, líquida da capacidade do próprio
	# insumo. Sem cadeia a demanda é 0 e a lavoura não abre (nasceria com o preço
	# no piso e faliria em cascata).
	var derived_gap := 0.0
	if Templates.LAND_COMPETING_RESOURCES.has(resource):
		derived_gap = Templates.derived_input_demand(city, product) \
			- Templates.existing_capacity(city, product)
		if derived_gap <= 0.0:
			return
	var price := _light_market(city_index, product, "m_price")
	if price <= 0.0:
		return
	var shortage := _light_market(city_index, product, "m_shortage")
	var sold := _light_market(city_index, product, "m_sold")
	var desired := _light_market(city_index, product, "m_desired")
	if derived_gap > shortage:
		shortage = derived_gap
	# §15/D-060: a escassez efetiva desconta a capacidade futura em obras — a
	# lacuna já atendida por uma obra em voo (local ou global) não dispara outro
	# candidato (cascata de minas/fazendas). R3/Q-014: na CESTA ALIMENTAR o
	# pending GLOBAL não desconta — obra em outra cidade não garante entrega aqui
	# (frete/preço/porto); a obra LOCAL continua descontando.
	shortage = maxf(0.0, shortage - _pending_capacity(city_index, product))
	if product >= 0 and product < _pending_global.size() \
			and not Templates.FOOD_RESOURCES.has(resource):
		shortage = maxf(0.0, shortage - float(_pending_global[product]))
	var is_food := bool(context.get("is_food", false))
	if is_food:
		# Comida: só onde há escassez local (nunca em excesso).
		if shortage <= 0.0:
			return
	elif shortage <= 0.0 and sold <= 0.0 and desired <= 0.0:
		return
	var base_unit := float(context.get("base_unit", 0.0))
	if price <= base_unit:
		return
	# §15/D-060: lavouras industriais (algodão/madeira) competem por terra com a
	# cesta e só são viáveis em terra boa — aptidão marginal produz fração da
	# capacidade e a fazenda fale quando o preço normaliza.
	if Templates.LAND_COMPETING_RESOURCES.has(resource):
		var suitability: Array = city.get("suitability", [])
		if product >= suitability.size() or float(suitability[product]) < 0.50:
			return
	var recipe: Dictionary = _light_best_recipe(resource, city_index)
	if bool(context.get("has_recipes", false)) and recipe.is_empty():
		return
	var capital_per_capacity := float(
		context.get("capital_per_capacity", 2000.0))
	var unit_cost := base_unit
	var recipe_id := ""
	if not recipe.is_empty():
		capital_per_capacity = float(recipe.get("capital_per_capacity", 2000.0))
		recipe_id = String(recipe.get("id", ""))
		var inputs: Array = recipe.get("inputs", [])
		for item: Dictionary in inputs:
			unit_cost += float(item.get("qty", 0.0)) \
				/ float(recipe.get("yield", 1.0)) \
				* _light_market(city_index, int(item.get("product", -1)), "m_price")
	else:
		var input_product := int(context.get("input_product", -1))
		if input_product >= 0:
			unit_cost += float(context.get("input_ratio", 0.0)) \
				* _light_market(city_index, input_product, "m_price")
	# §15/D-060: aptidão < 1.0 reduz a produção (capacidade × aptidão) — o custo
	# por tonelada produzida sobe para custo ÷ aptidão (mesmo gate de viabilidade
	# do privado/governo). Sem isso o grupo abre algodão/madeira que produzem
	# fração da capacidade e falem.
	if not bool(context.get("has_recipes", false)) \
			and not Templates.MINERAL_RESOURCES.has(resource):
		var suitability: Array = city.get("suitability", [])
		if product >= 0 and product < suitability.size():
			var apt := float(suitability[product])
			if apt > 0.0 and apt < 1.0:
				unit_cost = unit_cost / apt
	if capital_per_capacity <= 0.0:
		return
	var margin := price - unit_cost
	if margin <= 0.0:
		return
	out.append({
		"city": city,
		"resource": resource,
		"product": product,
		"margin": margin,
		"capital_per_capacity": capital_per_capacity,
		"recipe": recipe_id,
		"is_food": is_food,
		"price": price,
		"shortage": shortage,
		"desired": desired,
	})


## Risco/retorno do grupo para um candidato da base (o que sobrou de
## `_evaluate` depois de tirar o que não depende do grupo).
func _score_candidate(group: Dictionary, entry: Dictionary) -> Dictionary:
	var cash := float(group.get("cash", 0.0))
	var capital_per_capacity := float(entry.get("capital_per_capacity", 0.0))
	if capital_per_capacity <= 0.0:
		return {}
	var city: Dictionary = entry["city"]
	var city_nation := String(city.get("nation", ""))
	var origin := String(group.get("origin_nation", ""))
	var group_sector := String(group.get("sector", ""))
	var resource := String(entry.get("resource", ""))
	var foreign := city_nation != origin
	var margin := float(entry.get("margin", 0.0))
	var shortage := float(entry.get("shortage", 0.0))
	var desired := float(entry.get("desired", 0.0))
	if foreign:
		# IDE só quando o mercado local justifica e a abertura permite.
		if shortage <= 0.0 and desired < 1.0:
			return {}
		var policy := _policy(city_nation, group_sector)
		var mode := int(policy.get("mode", 0))
		if mode == 3:
			return {}
		if mode == 1 and not _is_incumbent(group, city_nation, resource):
			return {}
	# Legado: o porte GDScript ainda usa o 0,5 do caixa; o jogo (C++) usa o
	# forecast. Mantido só para as ferramentas que instanciam este script.
	var capacity := clampf(cash * CASH_SHARE / capital_per_capacity, 0.5,
		MAX_CAPACITY)
	var cost := capacity * capital_per_capacity
	if cost > cash:
		capacity = cash / capital_per_capacity
		cost = capacity * capital_per_capacity
	if capacity < 0.5:
		return {}
	# Rampa de operação (fábrica nova começa em 25%, como no núcleo).
	var ramp := 1.0 if bool(entry.get("is_food", false)) else 0.5
	var profit_day := margin * capacity * ramp
	if profit_day <= 0.0:
		return {}
	var risk := 1.0
	var partner := ""
	var local_share := 0.0
	if foreign:
		var policy := _policy(city_nation, group_sector)
		var mode := int(policy.get("mode", 0))
		var tariff := _tariff(city_nation, origin, resource)
		# IDE evita a tarifa: quanto maior a tarifa, mais o investimento local
		# compensa (o risco de mercado cresce, mas o prêmio também).
		risk *= 1.0 + maxf(0.0, 0.4 - tariff) * 2.0
		risk *= 1.0 + (1.0 - _confidence(city_nation)) * 2.0
		if mode == 2:
			local_share = float(policy.get("min_local", 0.3))
			if local_share <= 0.0:
				local_share = 0.3
			partner = _partner_for(group, city_nation, cost * local_share)
			if partner.is_empty():
				return {}
			risk *= 1.0 + local_share
	var score := profit_day / maxf(cost, 1.0) / risk
	return {
		"city": city,
		"resource": resource,
		"product": int(entry.get("product", -1)),
		"recipe": String(entry.get("recipe", "")),
		"capacity": capacity,
		"cost": cost,
		"margin": margin,
		"score": score,
		"foreign": foreign,
		"partner": partner,
		"local_share": local_share,
	}


## Executa a melhor oportunidade do grupo (chamada do núcleo real).
func _execute(group: Dictionary, opportunity: Dictionary) -> void:
	var city: Dictionary = opportunity["city"]
	var capacity := float(opportunity["capacity"])
	var partner := String(opportunity.get("partner", ""))
	var building_id := "GRP-%s-%d" % [
		String(group.get("id", "")).replace("-", ""), _day]
	var result: Dictionary = _sim.group_invest(
		String(group["id"]), String(city["id"]), building_id,
		String(opportunity["resource"]), int(opportunity["product"]), capacity,
		String(opportunity.get("recipe", "")), partner)
	var created := bool(result.get("created", false))
	var decision := {
		"day": _day,
		"agent": "group",
		"group": String(group.get("id", "")),
		"city": String(city.get("id", "")),
		"action": "build_group" if created else "skip",
		"resource": String(opportunity["resource"]),
		"cost": float(result.get("cost", 0.0)),
		"capacity": capacity,
		"foreign": bool(opportunity.get("foreign", false)),
		"score": float(opportunity["score"]),
		"reason": "retorno %.4f/dia" % float(opportunity["score"]) if created
			else String(result.get("error", "")),
	}
	_decisions.append(decision)
	if created:
		# Mantém o caixa local em dia para a próxima decisão do mesmo grupo.
		for index in _groups.size():
			if String(_groups[index].get("id", "")) == String(group.get("id", "")):
				_groups[index]["cash"] = float(_groups[index].get("cash", 0.0)) \
					- float(result.get("cost", 0.0))
				break


# --- Sinais do dia -----------------------------------------------------------

func _load_signals(status: Dictionary) -> void:
	if _sim == null:
		_signals = {}
	elif _sim.has_method("get_investment_signals"):
		_signals = _sim.get_investment_signals()
	elif _sim.has_method("get_investment_status"):
		_signals = _sim.get_investment_status()
	else:
		_signals = {}
	_policy_cache.clear()
	_tariff_cache.clear()
	_light = status.get("light", {})
	_light_product_count = int(_light.get("product_count", 0))
	# §15/D-060: capacidade futura em obras por produto (soma em todas as
	# cidades), para não abrir candidato concorrente à mesma lacuna em voo.
	_measure_pending_global()


func _policy(nation: String, sector: String) -> Dictionary:
	var key := nation + "|" + sector
	if _policy_cache.has(key):
		return _policy_cache[key]
	var policies: Array = _signals.get("fdi_policies", [])
	var found: Dictionary = {"mode": 0, "min_local": 0.0}
	for entry: Dictionary in policies:
		if String(entry.get("nation", "")) == nation \
				and String(entry.get("sector", "")) == sector:
			found = entry
			break
	_policy_cache[key] = found
	return found


func _confidence(nation: String) -> float:
	var nations: Dictionary = _signals.get("nations", {})
	var row: Dictionary = nations.get(nation, {})
	return float(row.get("confidence", 1.0))


func _tariff(nation: String, origin: String, resource: String) -> float:
	var key := nation + "|" + origin + "|" + resource
	if _tariff_cache.has(key):
		return float(_tariff_cache[key])
	var value := float(_sim.get_tariff(nation, origin, resource))
	_tariff_cache[key] = value
	return value


## Incumbente no setor do anfitrião: índice construído uma vez por dia a partir
## do status, em vez de varrer cidade × construção a cada candidato.
func _is_incumbent(group: Dictionary, nation: String, resource: String) -> bool:
	if not _incumbents_ready:
		_incumbents_ready = true
		var cities: Array = _status.get("cities", [])
		var light: Dictionary = _light
		var i_city: PackedInt32Array = light.get("i_city", PackedInt32Array())
		var i_resource: PackedStringArray = light.get("i_resource", PackedStringArray())
		var i_group: PackedStringArray = light.get("i_owner_group", PackedStringArray())
		for index in i_city.size():
			var city_index := int(i_city[index])
			var city_nation := ""
			if city_index >= 0 and city_index < cities.size():
				city_nation = String((cities[city_index] as Dictionary).get("nation", ""))
			var building_resource := String(i_resource[index])
			var owner_group := String(i_group[index])
			var key := city_nation + "|" + building_resource
			if not _incumbents.has(key):
				_incumbents[key] = {}
			(_incumbents[key] as Dictionary)[owner_group] = true
	var key := nation + "|" + resource
	var owners: Dictionary = _incumbents.get(key, {})
	return owners.has(String(group.get("id", "")))


func _partner_for(group: Dictionary, nation: String, required: float) -> String:
	for candidate: Dictionary in _groups:
		if String(candidate.get("id", "")) == String(group.get("id", "")):
			continue
		if String(candidate.get("origin_nation", "")) != nation:
			continue
		if bool(candidate.get("state_owned", false)):
			continue
		if float(candidate.get("cash", 0.0)) >= required:
			return String(candidate.get("id", ""))
	return ""


func _compute_food_secure(status: Dictionary) -> bool:
	var light: Dictionary = status.get("light", {})
	var satisfaction: PackedFloat64Array = light.get(
		"p_satisfaction", PackedFloat64Array())
	var total := 0.0
	var count := 0
	for value in satisfaction:
		total += float(value)
		count += 1
	if count == 0:
		return true
	return total / float(count) >= FOOD_SECURE_SATISFACTION


## Mapa setor → [recursos] a partir de goods.json (uma leitura por instância).
func _ensure_sector_map() -> void:
	if _sector_ready:
		return
	_sector_ready = true
	_sector_of.clear()
	var file := FileAccess.open("res://data/definitions/economy/goods.json",
		FileAccess.READ)
	if file == null:
		return
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(data) != TYPE_ARRAY:
		return
	for entry: Variant in data:
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		var item: Dictionary = entry
		var sector := String(item.get("sector", ""))
		var id := String(item.get("id", ""))
		if sector.is_empty() or id.is_empty():
			continue
		if not _sector_of.has(sector):
			_sector_of[sector] = []
		(_sector_of[sector] as Array).append(id)
