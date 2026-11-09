extends Node

## Efeitos sonoros de interface (Kenney Interface Sounds, CC0).
## Registrado como autoload `UiSfx`: toca uma variação aleatória por tipo de evento.
## Os streams são pré-carregados para evitar I/O no momento do clique.
## Eventos contínuos (rolagem, arraste de slider) usam `play_throttled`, que limita
## a repetição por chave para não saturar as vozes nem virar metralhadora.

enum Sound {
	CLICK,
	SELECT,
	TOGGLE,
	BACK,
	OPEN,
	CLOSE,
	CONFIRM,
	ERROR,
	SCROLL,
	TICK,
	SWITCH,
	QUESTION,
	MAXIMIZE,
	MINIMIZE,
}

const PATHS := {
	Sound.CLICK: [
		"res://assets/audio/interface/click_001.wav",
		"res://assets/audio/interface/click_002.wav",
		"res://assets/audio/interface/click_003.wav",
		"res://assets/audio/interface/click_004.wav",
		"res://assets/audio/interface/click_005.wav",
	],
	Sound.SELECT: [
		"res://assets/audio/interface/select_001.wav",
		"res://assets/audio/interface/select_002.wav",
		"res://assets/audio/interface/select_003.wav",
		"res://assets/audio/interface/select_004.wav",
		"res://assets/audio/interface/select_005.wav",
		"res://assets/audio/interface/select_006.wav",
		"res://assets/audio/interface/select_007.wav",
		"res://assets/audio/interface/select_008.wav",
	],
	Sound.TOGGLE: [
		"res://assets/audio/interface/toggle_001.wav",
		"res://assets/audio/interface/toggle_002.wav",
		"res://assets/audio/interface/toggle_003.wav",
		"res://assets/audio/interface/toggle_004.wav",
	],
	Sound.BACK: [
		"res://assets/audio/interface/back_001.wav",
		"res://assets/audio/interface/back_002.wav",
		"res://assets/audio/interface/back_003.wav",
		"res://assets/audio/interface/back_004.wav",
	],
	Sound.OPEN: [
		"res://assets/audio/interface/open_001.wav",
		"res://assets/audio/interface/open_002.wav",
		"res://assets/audio/interface/open_003.wav",
		"res://assets/audio/interface/open_004.wav",
	],
	Sound.CLOSE: [
		"res://assets/audio/interface/close_001.wav",
		"res://assets/audio/interface/close_002.wav",
		"res://assets/audio/interface/close_003.wav",
		"res://assets/audio/interface/close_004.wav",
	],
	Sound.CONFIRM: [
		"res://assets/audio/interface/confirmation_001.wav",
		"res://assets/audio/interface/confirmation_002.wav",
		"res://assets/audio/interface/confirmation_003.wav",
		"res://assets/audio/interface/confirmation_004.wav",
	],
	Sound.ERROR: [
		"res://assets/audio/interface/error_001.wav",
		"res://assets/audio/interface/error_002.wav",
		"res://assets/audio/interface/error_003.wav",
		"res://assets/audio/interface/error_004.wav",
		"res://assets/audio/interface/error_005.wav",
		"res://assets/audio/interface/error_006.wav",
		"res://assets/audio/interface/error_007.wav",
		"res://assets/audio/interface/error_008.wav",
	],
	Sound.SCROLL: [
		"res://assets/audio/interface/scroll_001.wav",
		"res://assets/audio/interface/scroll_002.wav",
		"res://assets/audio/interface/scroll_003.wav",
		"res://assets/audio/interface/scroll_004.wav",
		"res://assets/audio/interface/scroll_005.wav",
	],
	Sound.TICK: [
		"res://assets/audio/interface/tick_001.wav",
		"res://assets/audio/interface/tick_002.wav",
		"res://assets/audio/interface/tick_004.wav",
	],
	Sound.SWITCH: [
		"res://assets/audio/interface/switch_001.wav",
		"res://assets/audio/interface/switch_002.wav",
		"res://assets/audio/interface/switch_003.wav",
		"res://assets/audio/interface/switch_004.wav",
		"res://assets/audio/interface/switch_005.wav",
		"res://assets/audio/interface/switch_006.wav",
		"res://assets/audio/interface/switch_007.wav",
	],
	Sound.QUESTION: [
		"res://assets/audio/interface/question_001.wav",
		"res://assets/audio/interface/question_002.wav",
		"res://assets/audio/interface/question_003.wav",
		"res://assets/audio/interface/question_004.wav",
	],
	Sound.MAXIMIZE: [
		"res://assets/audio/interface/maximize_001.wav",
		"res://assets/audio/interface/maximize_002.wav",
		"res://assets/audio/interface/maximize_003.wav",
		"res://assets/audio/interface/maximize_004.wav",
		"res://assets/audio/interface/maximize_005.wav",
		"res://assets/audio/interface/maximize_006.wav",
		"res://assets/audio/interface/maximize_007.wav",
		"res://assets/audio/interface/maximize_008.wav",
		"res://assets/audio/interface/maximize_009.wav",
	],
	Sound.MINIMIZE: [
		"res://assets/audio/interface/minimize_001.wav",
		"res://assets/audio/interface/minimize_002.wav",
		"res://assets/audio/interface/minimize_003.wav",
		"res://assets/audio/interface/minimize_004.wav",
		"res://assets/audio/interface/minimize_005.wav",
		"res://assets/audio/interface/minimize_006.wav",
		"res://assets/audio/interface/minimize_007.wav",
		"res://assets/audio/interface/minimize_008.wav",
		"res://assets/audio/interface/minimize_009.wav",
	],
}

## Tema ativo da interface. "command_post" = sintetizado (seco/mecânico, WWII);
## "kenney" = pacote CC0 original (mais suave). Trocar aqui muda tudo.
const THEME := "command_post"
const COMMAND_POST_DIR := "res://assets/audio/interface_command_post/"
const COMMAND_POST_VARIANTS := 3
const COMMAND_POST_NAMES := {
	Sound.CLICK: "click",
	Sound.SELECT: "select",
	Sound.TOGGLE: "toggle",
	Sound.BACK: "back",
	Sound.OPEN: "open",
	Sound.CLOSE: "close",
	Sound.CONFIRM: "confirmation",
	Sound.ERROR: "error",
	Sound.SCROLL: "scroll",
	Sound.TICK: "tick",
	Sound.SWITCH: "switch",
	Sound.QUESTION: "question",
	Sound.MAXIMIZE: "maximize",
	Sound.MINIMIZE: "minimize",
}

const VOICES := 8
const VOLUME_DB := -8.0
const PITCH_JITTER := 0.04

## Sons contínuos ficam abaixo do clique para não competir com a ação principal.
const VOLUME_OFFSETS := {
	Sound.SCROLL: -7.0,
	Sound.TICK: -6.0,
	Sound.QUESTION: -3.0,
}

const MAX_THROTTLE_KEYS := 64

var _streams: Dictionary = {}
var _voices: Array[AudioStreamPlayer] = []
var _cursor := 0
var _last_played: Dictionary = {}


func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	if DisplayServer.get_name() == "headless":
		return
	_streams = _load_streams()
	_create_voices()


## Toca um efeito de interface; sem efeito em execução headless.
## @param sound - Tipo de som a reproduzir.
func play(sound: Sound) -> void:
	if _voices.is_empty():
		return
	var stream := _pick(sound)
	if stream == null:
		return

	var voice := _next_voice()
	voice.stream = stream
	voice.volume_db = VOLUME_DB + float(VOLUME_OFFSETS.get(sound, 0.0))
	voice.pitch_scale = 1.0 + randf_range(-PITCH_JITTER, PITCH_JITTER)
	voice.play()


## Toca um efeito contínuo no máximo uma vez a cada `interval_ms` para a mesma chave.
## Usado por rolagem, arraste de slider e outros eventos que chegam a cada frame.
## @param sound - Tipo de som a reproduzir.
## @param key - Identificador estável do controle que originou o som.
## @param interval_ms - Intervalo mínimo entre repetições, em milissegundos.
func play_throttled(sound: Sound, key: String, interval_ms: int) -> void:
	var now := Time.get_ticks_msec()
	if now - int(_last_played.get(key, -interval_ms)) < interval_ms:
		return
	if _last_played.size() >= MAX_THROTTLE_KEYS:
		_last_played.clear()
	_last_played[key] = now
	play(sound)


func _pick(sound: Sound) -> AudioStream:
	var variants: Array = _streams.get(sound, [])
	if variants.is_empty():
		return null
	return variants[randi() % variants.size()]


func _load_streams() -> Dictionary:
	var loaded := {}
	var paths := _theme_paths()
	for sound: Sound in paths:
		var variants: Array[AudioStream] = []
		for path: String in paths[sound]:
			var stream := load(path) as AudioStream
			if stream != null:
				variants.append(stream)
		loaded[sound] = variants
	return loaded


## Caminhos do tema ativo ("command_post" sintetizado ou o pacote Kenney).
func _theme_paths() -> Dictionary:
	if THEME != "command_post":
		return PATHS
	var result := {}
	for sound: Sound in COMMAND_POST_NAMES:
		var base: String = COMMAND_POST_NAMES[sound]
		var list: Array[String] = []
		for i in COMMAND_POST_VARIANTS:
			list.append("%s%s_%03d.wav" % [COMMAND_POST_DIR, base, i + 1])
		result[sound] = list
	return result


func _create_voices() -> void:
	for index in VOICES:
		var voice := AudioStreamPlayer.new()
		voice.name = "Voice%d" % index
		voice.volume_db = VOLUME_DB
		add_child(voice)
		_voices.append(voice)


## Reaproveita um canal livre; se todos estiverem tocando, recicla em rodízio.
func _next_voice() -> AudioStreamPlayer:
	for voice in _voices:
		if not voice.playing:
			return voice

	var recycled := _voices[_cursor]
	_cursor = (_cursor + 1) % _voices.size()
	return recycled
