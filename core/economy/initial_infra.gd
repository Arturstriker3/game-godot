extends RefCounted

## B5 (D-089) — NÍVEL INICIAL de infraestrutura do estado a partir do dado real.
##
## Fonte: `data/definitions/scenario/state_infrastructure_2026.json` (World Bank
## WDI/LPI, coletado pela tarefa de dados D-007; regra e limitações em
## `docs/data/state_infrastructure.md`). O arquivo tem uma entrada por estado de
## `state_regions.json` (4.226 ids) com o valor BRUTO por dimensão e a regra
## `% -> nível 0..3` declarada em `conversion_rule`.
##
## Este helper APLICA a regra (em vez de confiar no campo pré-calculado
## `infra_levels`, que o teste confere contra este resultado), devolvendo
## `{state_id: {road, power, water}}` para o núcleo (`set_region_infra`).
##
## Regra (docs/data/state_infrastructure.md §3) — os cortes NÃO são duplicados
## aqui: vêm de `conversion_rule.<tipo>.bands`, que é a fonte única do arquivo.
## - energia: `electricity_access_rural_pct`, fallback `electricity_access_pct`;
## - água: `water_basic_rural_pct`, fallback `water_basic_pct`;
## - estrada: `road_quality_index` (LPI 1..5);
## - rural primeiro porque o nível mede o PIOR caso da região (o interior é o
##   gargalo; a média nacional esconderia a falta de acesso);
## - estado sem observação → `conversion_rule.no_data_level` (0 = ausência
##   explícita, não cobertura zero) — a mesma regra do validador do dado.
##
## Granularidade: o arquivo é NACIONAL (`granularity: "nation"`) — o valor do
## país é propagado a cada estado pela própria fonte; nada é rateado/estimado
## aqui, nem quando a fonte separa urbano e rural (o nível usa o rural).

const STATE_INFRA_PATH := "res://data/definitions/scenario/state_infrastructure_2026.json"

const TYPE_ROAD := "road"
const TYPE_POWER := "power"
const TYPE_WATER := "water"

## Cache do arquivo (3,3 MB): o boot faz uma leitura só, e o teste/ferramentas
## reaproveitam a mesma instância de classe.
static var _document: Dictionary = {}
static var _levels: Dictionary = {}
static var _loaded := false


## Carrega e indexa o arquivo uma vez. Ausente/ inválido deixa tudo vazio — o
## núcleo mantém todas as regiões em 0, como antes da B5 ler dado real.
static func _load() -> void:
	if _loaded:
		return
	_loaded = true
	if not FileAccess.file_exists(STATE_INFRA_PATH):
		push_warning("InitialInfra: arquivo ausente " + STATE_INFRA_PATH)
		return
	var parsed: Variant = JSON.parse_string(
			FileAccess.get_file_as_string(STATE_INFRA_PATH))
	if typeof(parsed) != TYPE_DICTIONARY:
		push_warning("InitialInfra: JSON invalido " + STATE_INFRA_PATH)
		return
	_document = parsed
	var rule := conversion_rule()
	var states: Dictionary = _document.get("states", {})
	if states.is_empty() or rule.is_empty():
		return
	for state_id: Variant in states:
		var entry: Variant = states[state_id]
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		_levels[String(state_id)] = _levels_for_entry(entry, rule)


## Níveis por estado: `{state_id: {road, power, water}}` (cada um
## 0..infra_max_level), mais a proveniência de cada dimensão (`primary`,
## `fallback` ou `none`) e o `data_status` declarado. O núcleo lê só
## `road`/`power`/`water`; o resto é evidência para teste/auditoria.
static func levels_by_state() -> Dictionary:
	_load()
	return _levels


## Bloco `conversion_rule` do arquivo (cortes e campos por tipo). Vazio quando o
## arquivo não abriu — o chamador então não inventa corte nenhum.
static func conversion_rule() -> Dictionary:
	_load()
	return _document.get("conversion_rule", {})


## Nível de uma dimensão a partir do valor bruto e das faixas do arquivo.
## `metric` = "pct" (corte em `min_pct`) ou "index" (corte em `min_index`).
## Sem valor numérico finito → `no_data` (ausência explícita).
static func level_from_bands(value: Variant, bands: Array, metric: String,
		no_data: int) -> int:
	if not (value is int or value is float):
		return no_data
	var number := float(value)
	if not is_finite(number):
		return no_data
	var key := "min_index" if metric == "index" else "min_pct"
	var level := no_data
	for band: Variant in bands:
		if typeof(band) != TYPE_DICTIONARY:
			continue
		var row: Dictionary = band
		if not row.has(key):
			continue
		# Faixas em ordem crescente: a última que o valor alcança vence.
		if number >= float(row[key]):
			level = int(row.get("level", no_data))
	var max_level := int(conversion_rule().get("infra_max_level", 3))
	return clampi(level, 0, max_level)


## Níveis de uma entrada de estado. `data_status` no_data curto-circuita para a
## regra de ausência (não se deriva nível de percentual nulo).
static func _levels_for_entry(entry: Dictionary, rule: Dictionary) -> Dictionary:
	var no_data := int(rule.get("no_data_level", 0))
	var out := {
		"data_status": String(entry.get("data_status", "")),
	}
	for kind in [TYPE_ROAD, TYPE_POWER, TYPE_WATER]:
		var type_rule: Dictionary = rule.get(kind, {})
		var metric := "index" if kind == TYPE_ROAD else "pct"
		var picked := _pick_value(entry, type_rule)
		out[kind] = level_from_bands(picked["value"],
				type_rule.get("bands", []), metric, no_data)
		out[kind + "_source"] = String(picked["source"])
	return out


## Valor bruto da dimensão: campo principal (rural, no dado de acesso) e, se ele
## não for numérico, o fallback declarado no arquivo (total). Nenhum dos dois =
## ausência. Estrada não tem fallback (campo único do LPI).
static func _pick_value(entry: Dictionary, type_rule: Dictionary) -> Dictionary:
	for source in ["field", "fallback"]:
		var key := String(type_rule.get(source, ""))
		if key == "":
			continue
		var value: Variant = entry.get(key, null)
		if value is int or value is float:
			return {"value": value, "source": source}
	return {"value": null, "source": "none"}
