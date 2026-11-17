extends RefCounted

## Configuração do modo TURBO do relógio econômico.
##
## FERRAMENTA DE TESTE/AGENTE, não é feature do jogador: nada em `ui/`,
## `core/ui/` ou `map/` lê estes campos. Só a LINHA DE COMANDO
## (`--turbo...`) ou o AMBIENTE (`OGS_TURBO...`) ativam o modo — sem eles, o
## jogo roda exatamente como hoje (`enabled = false`, lote de 1 dia).
##
## Contrato da CLI (recebida em `_init`, normalmente `OS.get_cmdline_user_args()`
## — todos os args depois de `--`, nunca `OS.get_cmdline_args()`):
##   --turbo                     liga o turbo (1 dia/tarefa, publica a cada dia)
##   --turbo-days-per-frame=N    liga; N dias por tarefa da thread (N >= 1)
##   --turbo-render-every=N      liga; publica (sinais) a cada N dias (N >= 1)
##   --bench-days=N              mede parede até o dia N (N >= 1; 0 desliga).
##                               NÃO liga o turbo: serve para medir o modo normal.
##   --bench-speed=N             força a velocidade 0..3 (0 = pausa) no bench
##
## Os argumentos vêm por PARÂMETRO (não são lidos do `OS` aqui) para o teste
## poder chamar `TurboConfigScript.new(["--turbo", ...])` sem depender da linha
## de comando do processo. Só o ambiente é lido do `OS`.
##
## Ambiente equivalente: OGS_TURBO (1/true/yes/on, sem diferenciar caixa),
## OGS_TURBO_DAYS_PER_FRAME, OGS_TURBO_RENDER_EVERY, OGS_BENCH_DAYS,
## OGS_BENCH_SPEED. A linha de comando vence quando as duas fontes aparecem.
##
## `render_every` acompanha `days_per_frame` quando não é dado explicitamente e
## nunca fica menor que ele: a menor granularidade de publicação é o lote.

var enabled: bool = false
var days_per_frame: int = 4
var render_every: int = 4
var bench_days: int = 0
var bench_speed: int = -1

## `--turbo-render-every=` / OGS_TURBO_RENDER_EVERY apareceu? Sem isso,
## `render_every` segue `days_per_frame`.
var _render_every_given := false


func _init(args: PackedStringArray = PackedStringArray()) -> void:
	# Ambiente primeiro: a linha de comando sobrescreve.
	_apply_env("OGS_TURBO", "")
	_apply_env("OGS_TURBO_DAYS_PER_FRAME", "days_per_frame")
	_apply_env("OGS_TURBO_RENDER_EVERY", "render_every")
	_apply_env("OGS_BENCH_DAYS", "bench_days")
	_apply_env("OGS_BENCH_SPEED", "bench_speed")
	for arg: String in args:
		_apply_arg(arg)
	if not _render_every_given:
		render_every = days_per_frame
	render_every = maxi(render_every, days_per_frame)


## Lê um argumento de usuário (`--flag` ou `--flag=N`). Desconhecido é ignorado.
func _apply_arg(arg: String) -> void:
	if arg == "--turbo":
		enabled = true
		return
	if arg.begins_with("--turbo-days-per-frame="):
		# A presença da flag liga o turbo; valor inválido mantém o padrão.
		enabled = true
		days_per_frame = maxi(1, _parse_int(arg.get_slice("=", 1), days_per_frame))
		return
	if arg.begins_with("--turbo-render-every="):
		enabled = true
		_render_every_given = true
		render_every = maxi(1, _parse_int(arg.get_slice("=", 1), render_every))
		return
	if arg.begins_with("--bench-days="):
		bench_days = maxi(0, _parse_int(arg.get_slice("=", 1), bench_days))
		return
	if arg.begins_with("--bench-speed="):
		bench_speed = clampi(_parse_int(arg.get_slice("=", 1), bench_speed), 0, 3)


## Lê uma variável de ambiente. `target` vazio = só a chave booleana OGS_TURBO.
func _apply_env(name: String, target: String) -> void:
	var raw := OS.get_environment(name).strip_edges()
	if raw.is_empty():
		return
	match target:
		"":
			if raw.to_lower() in ["1", "true", "yes", "on"]:
				enabled = true
		"days_per_frame":
			# Mesma regra da CLI: a chave presente liga o turbo.
			enabled = true
			days_per_frame = maxi(1, _parse_int(raw, days_per_frame))
		"render_every":
			enabled = true
			_render_every_given = true
			render_every = maxi(1, _parse_int(raw, render_every))
		"bench_days":
			bench_days = maxi(0, _parse_int(raw, bench_days))
		"bench_speed":
			bench_speed = clampi(_parse_int(raw, bench_speed), 0, 3)


## Converte o texto em int; devolve `fallback` se não for um inteiro.
func _parse_int(text: String, fallback: int) -> int:
	var trimmed := text.strip_edges()
	if not trimmed.is_valid_int():
		return fallback
	return int(trimmed)


## Resumo de uma linha para o log de início (não vai para a UI).
func describe() -> String:
	return "days_per_frame=%d render_every=%d bench_days=%d bench_speed=%d" % [
		days_per_frame, render_every, bench_days, bench_speed]
