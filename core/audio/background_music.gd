class_name BackgroundMusic
extends AudioStreamPlayer

## Trilha de fundo: toca as faixas em sequência e recomeça do início.
## Registrado como autoload `Music`.

const TRACKS: Array[String] = [
	"res://assets/music/march_of_the_fallen.ogg",
	"res://assets/music/the_wealth_of_nations.ogg",
	"res://assets/music/the_global_trade.ogg",
	"res://assets/music/l_appel_du_general.ogg",
	"res://assets/music/factory_dawn.ogg",
	"res://assets/music/solemn_leitmotif.ogg",
	"res://assets/music/the_fallen_heroes.ogg",
]

const VOLUME_DB := -10.0

var _track_index := -1


func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	volume_db = VOLUME_DB
	finished.connect(_play_next)

	if DisplayServer.get_name() == "headless":
		return
	_play_next()


func _exit_tree() -> void:
	stop()
	stream = null


func _play_next() -> void:
	if TRACKS.is_empty():
		return

	_track_index = (_track_index + 1) % TRACKS.size()
	stream = load(TRACKS[_track_index])
	if stream == null:
		return
	play()
