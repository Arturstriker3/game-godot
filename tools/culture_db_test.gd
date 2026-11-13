extends SceneTree

## Contrato de leitura das culturas: catálogo (nacional + sem estado),
## composição nacional, mistura por cidade, papéis (principal/aceita), "outras"
## no fim e nomes nos três idiomas. O teste não presume ids: descobre tudo pela
## API. Fonte e "estimativa" não são mais expostas.
##
##   godot --headless --path . --script res://tools/culture_db_test.gd

const CultureDbScript := preload("res://core/culture/culture_db.gd")

var _failures: Array[String] = []


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	print("=== culture_db_test ===")
	CultureDbScript.load_data(true)

	var cultures := CultureDbScript.culture_count()
	var nations: Array = CultureDbScript.nation_tags()
	var cities: Array = CultureDbScript.city_ids()
	print("catálogo=%d nações=%d cidades=%d" % [cultures, nations.size(),
			cities.size()])
	if cultures < 100:
		_fail("catálogo cultural incompleto: %d" % cultures)
	if nations.size() < 100:
		_fail("composição nacional incompleta: %d" % nations.size())
	if cities.size() < 500:
		_fail("mistura por cidade incompleta: %d" % cities.size())

	_check_nations(nations)
	_check_cities(cities)
	_check_catalog(nations, cities)
	_check_locales(nations)
	_finish()


func _check_nations(tags: Array) -> void:
	var with_primary := 0
	var with_accepted := 0
	var others_last := 0
	var others_present := 0
	for tag: Variant in tags:
		var key := String(tag)
		var mix: Array = CultureDbScript.nation_mix(key)
		if mix.is_empty():
			_fail("nação %s sem composição" % key)
			continue
		var total := 0.0
		var previous := 101.0
		var seen_other := false
		var primary_seen := false
		for item: Variant in mix:
			var entry: Dictionary = item
			var percent := float(entry.get("percent", 0.0))
			var id := String(entry.get("id", ""))
			result_ok(not String(entry.get("name", "")).is_empty(),
					"nome vazio em %s/%s" % [key, id])
			if id == CultureDbScript.OTHER_ID:
				seen_other = true
				others_present += 1
			else:
				if seen_other:
					_fail("%s: cultura %s depois de 'outras'" % [key, id])
				if percent > previous + 0.001:
					_fail("%s: composição fora de ordem (%s %.1f > %.1f)"
							% [key, id, percent, previous])
				previous = percent
			if float(entry.get("share", -1.0)) < 0.0 \
					or float(entry.get("share", 2.0)) > 1.0:
				_fail("%s: share fora de 0..1 em %s" % [key, id])
			if String(entry.get("role", "")) == "primary":
				primary_seen = true
			total += percent
		if absf(total - 100.0) > 2.0:
			_fail("%s: composição soma %.1f%%" % [key, total])
		if seen_other:
			others_last += 1
		if primary_seen:
			with_primary += 1
		var primary: Dictionary = CultureDbScript.primary(key)
		if primary.is_empty():
			_fail("%s: primary() vazio" % key)
		elif String(primary.get("id", "")).is_empty():
			_fail("%s: primary() sem id" % key)
		if not CultureDbScript.accepted(key).is_empty():
			with_accepted += 1
		var national_id := CultureDbScript.national_culture(key)
		if national_id.is_empty():
			_fail("%s: sem cultura nacional" % key)
		else:
			var national := CultureDbScript.culture(national_id)
			if national.is_empty():
				_fail("%s: cultura nacional %s fora do catálogo" % [key, national_id])
			elif String(national.get("nation", "")) != key:
				_fail("%s: cultura nacional sem nation=%s" % [key, key])
			elif String(primary.get("id", "")) != national_id:
				_fail("%s: primary() %s != nacional %s"
						% [key, String(primary.get("id", "")), national_id])

	print("principal na composição=%d/%d aceitas=%d/%d outras-por-último=%d/%d"
			% [with_primary, tags.size(), with_accepted, tags.size(),
			others_last, others_present])
	if with_primary < int(tags.size() * 0.9):
		_fail("só %d nações têm a principal na composição" % with_primary)

	# Fora do arquivo: vazio, sem inventar.
	if not CultureDbScript.nation_mix("ZZZ").is_empty():
		_fail("nação inexistente devolveu composição")
	if not CultureDbScript.primary("ZZZ").is_empty():
		_fail("nação inexistente devolveu principal")
	if CultureDbScript.has_nation("ZZZ"):
		_fail("has_nation aceitou tag inexistente")


func _check_cities(ids: Array) -> void:
	var measured := 0
	var max_entries := 0
	for city_id: Variant in ids:
		var key := String(city_id)
		var mix: Array = CultureDbScript.city_mix(key)
		if mix.is_empty():
			_fail("cidade %s sem mistura" % key)
			continue
		measured += 1
		max_entries = maxi(max_entries, mix.size())
		if mix.size() > 6:
			_fail("%s: mistura com %d entradas (máx. 5 + outras)"
					% [key, mix.size()])
		var total := 0.0
		var previous := 101.0
		var seen_other := false
		for item: Variant in mix:
			var entry: Dictionary = item
			var percent := float(entry.get("percent", 0.0))
			var id := String(entry.get("id", ""))
			result_ok(not String(entry.get("name", "")).is_empty(),
					"nome vazio em %s/%s" % [key, id])
			if id == CultureDbScript.OTHER_ID:
				seen_other = true
			else:
				if seen_other:
					_fail("%s: cultura %s depois de 'outras'" % [key, id])
				if percent > previous + 0.001:
					_fail("%s: mistura fora de ordem" % key)
				previous = percent
			total += percent
		if absf(total - 100.0) > 2.0:
			_fail("%s: mistura soma %.1f%%" % [key, total])

	print("cidades medidas=%d máx. entradas=%d" % [measured, max_entries])
	if measured != ids.size():
		_fail("cidades lidas=%d mas com mistura=%d" % [ids.size(), measured])
	if not CultureDbScript.city_mix("ZZZ").is_empty():
		_fail("cidade inexistente devolveu mistura")
	if CultureDbScript.has_city("ZZZ"):
		_fail("has_city aceitou id inexistente")


func _check_catalog(tags: Array, cities: Array) -> void:
	var national := 0
	var stateless := 0
	for id: Variant in CultureDbScript.culture_ids():
		var entry: Dictionary = CultureDbScript.culture(String(id))
		if entry.is_empty():
			_fail("catalogou id vazio na leitura: %s" % id)
			continue
		if entry.has("parent"):
			_fail("%s: campo 'parent' ainda exposto" % id)
		var tag := String(entry.get("nation", ""))
		var home: Array = entry.get("home_region", [])
		if tag.is_empty():
			stateless += 1
			if home.is_empty():
				_fail("%s: povo sem estado sem home_region" % id)
		else:
			national += 1
			if not home.is_empty():
				_fail("%s: cultura nacional com home_region" % id)
			if CultureDbScript.national_culture(tag) != String(id):
				_fail("%s: nation=%s não aponta de volta" % [id, tag])
	print("catálogo nacional=%d sem estado=%d" % [national, stateless])
	if national != tags.size():
		_fail("%d culturas nacionais para %d nações" % [national, tags.size()])
	if stateless <= 0:
		_fail("catálogo sem povos sem estado")

	var complete := 0
	var colored := 0
	var total := 0
	for tag: Variant in tags:
		for item: Variant in CultureDbScript.nation_mix(String(tag)):
			var id := String((item as Dictionary).get("id", ""))
			if id == CultureDbScript.OTHER_ID:
				continue
			var entry: Dictionary = CultureDbScript.culture(id)
			if entry.is_empty():
				_fail("cultura %s fora do catálogo" % id)
				continue
			total += 1
			var names := 0
			for locale in ["pt_BR", "en", "es"]:
				if not CultureDbScript.culture_name(id, locale).is_empty():
					names += 1
			if names == 3:
				complete += 1
			if not String(entry.get("color", "")).is_empty() \
					and CultureDbScript.culture_color(id) \
					!= CultureDbScript.INK_FALLBACK:
				colored += 1
			# Família é campo do catálogo; ausência é permitida, invenção não.
			if entry.has("family") and String(entry.get("family", "")).is_empty():
				_fail("%s: família vazia no catálogo" % id)

	print("culturas citadas=%d com 3 nomes=%d com cor=%d" % [total, complete,
			colored])
	if total <= 0:
		_fail("nenhuma cultura citada nas composições")
	elif complete < int(total * 0.95):
		_fail("só %d de %d culturas têm nome pt/en/es" % [complete, total])
	if colored < int(total * 0.95):
		_fail("só %d de %d culturas têm cor" % [colored, total])

	# Id desconhecido: legível e sem chave de i18n crua.
	var unknown := CultureDbScript.culture_name("cultura_inexistente")
	if unknown.is_empty() or unknown.contains("pop."):
		_fail("id desconhecido não virou rótulo legível: '%s'" % unknown)
	if not CultureDbScript.culture("cultura_inexistente").is_empty():
		_fail("culture() inventou entrada para id desconhecido")

	# A lista de cidades do serviço cobre o cenário vivo (fonte única: o CSV).
	var covered := 0
	for city_id: Variant in cities:
		if not CultureDbScript.city_mix(String(city_id)).is_empty():
			covered += 1
	if covered != cities.size():
		_fail("cidades lidas=%d mas com mistura=%d" % [cities.size(), covered])


## Trocar o idioma não pode esvaziar nem repetir o rótulo agregado.
func _check_locales(tags: Array) -> void:
	var sample := ""
	for tag: Variant in tags:
		var primary: Dictionary = CultureDbScript.primary(String(tag))
		if not primary.is_empty():
			sample = String(primary.get("id", ""))
			break
	if sample.is_empty():
		_fail("nenhuma principal para amostrar idioma")
		return
	var pt := CultureDbScript.culture_name(sample, "pt_BR")
	var en := CultureDbScript.culture_name(sample, "en")
	var es := CultureDbScript.culture_name(sample, "es")
	print("amostra %s: pt=%s en=%s es=%s" % [sample, pt, en, es])
	if pt.is_empty() or en.is_empty() or es.is_empty():
		_fail("nome cultural vazio em algum idioma")
	var others_pt := CultureDbScript.culture_name(CultureDbScript.OTHER_ID, "pt_BR")
	var others_en := CultureDbScript.culture_name(CultureDbScript.OTHER_ID, "en")
	var others_es := CultureDbScript.culture_name(CultureDbScript.OTHER_ID, "es")
	print("outras: pt=%s en=%s es=%s" % [others_pt, others_en, others_es])
	if others_pt == others_en and others_pt == others_es:
		_fail("'outras' não é traduzido nos três idiomas")
	for value in [others_pt, others_en, others_es]:
		if String(value).begins_with("pop."):
			_fail("chave de i18n vazou em 'outras'")


func result_ok(condition: bool, message: String) -> void:
	if not condition:
		_fail(message)


func _fail(message: String) -> void:
	if not _failures.has(message):
		_failures.append(message)


func _finish() -> void:
	if _failures.is_empty():
		print("PASS: catálogo nacional (1 por nação) e povos sem estado lidos por "
				+ "coluna; papéis e ordem conferidos")
		quit(0)
		return
	for failure in _failures:
		push_error("[culture_db_test] " + failure)
	quit(1)
