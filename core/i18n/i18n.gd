extends Node

## Autoload `I18n`: tradução em tempo de execução para pt-BR, inglês e espanhol.
##
## As telas nunca escrevem texto fixo: pedem `I18n.t("chave")` e escutam
## `locale_changed` para se reconstruírem quando o idioma muda.

signal locale_changed(locale: String)

const DEFAULT_LOCALE := "pt_BR"
const SUPPORTED_LOCALES: Array[String] = ["pt_BR", "en", "es"]

const SHORT_LABELS := {"pt_BR": "PT", "en": "EN", "es": "ES"}
const I18N_PATH := "res://data/definitions/i18n/i18n.json"
const SETTINGS_PATH := "user://settings.cfg"

var STRINGS: Dictionary = {}

var locale: String = DEFAULT_LOCALE


func _ready() -> void:
	_load_strings()
	var saved := _load_saved_locale()
	if not saved.is_empty():
		set_locale(saved)


## Lê as traduções do JSON (gerado de data/definitions/i18n/i18n.csv).
func _load_strings() -> void:
	var file := FileAccess.open(I18N_PATH, FileAccess.READ)
	if file == null:
		push_error("I18n: não abriu %s" % I18N_PATH)
		return
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(data) == TYPE_DICTIONARY:
		STRINGS = data



## Traduz uma chave; aceita parâmetros no formato `{nome}`.
func t(key: String, params: Dictionary = {}) -> String:
	var table: Dictionary = STRINGS.get(locale, {})
	var fallback: Dictionary = STRINGS[DEFAULT_LOCALE]
	var text: String = table.get(key, fallback.get(key, key))
	if params.is_empty():
		return text
	return text.format(params)


## Troca o idioma ativo e avisa as telas.
func set_locale(code: String) -> void:
	if not SUPPORTED_LOCALES.has(code) or code == locale:
		return
	locale = code
	locale_changed.emit(locale)


## Próximo idioma do ciclo PT → EN → ES → PT.
func next_locale() -> String:
	var index: int = SUPPORTED_LOCALES.find(locale)
	return SUPPORTED_LOCALES[(index + 1) % SUPPORTED_LOCALES.size()]


## Grava o idioma escolhido em user://settings.cfg ([general] locale), para
## persistir entre sessões.
func save_locale(code: String) -> void:
	if not SUPPORTED_LOCALES.has(code):
		return
	var config := ConfigFile.new()
	config.set_value("general", "locale", code)
	var err := config.save(SETTINGS_PATH)
	if err != OK:
		push_warning("I18n: falhou ao gravar %s (erro %d)" % [SETTINGS_PATH, err])


## Lê o idioma salvo em disco; vazio se ainda não existir.
func _load_saved_locale() -> String:
	var config := ConfigFile.new()
	if config.load(SETTINGS_PATH) != OK:
		return ""
	return str(config.get_value("general", "locale", ""))


func short_label() -> String:
	return SHORT_LABELS.get(locale, "PT")


## Nome localizado de uma nação a partir da tag (GBR, FRA...).
func nation_name(tag: String) -> String:
	return t("nation.%s" % tag)


## Nome localizado de uma cidade a partir do id (GBR-LDN...).
func city_name(city_id: String, fallback: String) -> String:
	var key := "city.name.%s" % city_id
	var value := t(key)
	return fallback if value == key else value


## Nome localizado de um porto a partir do id (GBR-DOV...).
func port_name(port_id: String, fallback: String) -> String:
	var key := "port.name.%s" % port_id
	var value := t(key)
	return fallback if value == key else value
