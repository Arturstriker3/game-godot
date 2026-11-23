extends RefCounted

## Caminhos dos artefatos temporários de teste (regra do projeto: nada de
## arquivo grande no C:).
##
## `OPENGS_TMP` escolhe a raiz; sem ela, `G:/opengs_tmp`. Se o disco não
## existir ou não aceitar escrita, cai para o temp do SO — o mesmo destino de
## antes, sem mudar o resultado de nenhum teste.
##
## Uso pelas ferramentas que geram JSON/CSV grande:
##
##     const TmpPaths := preload("res://tools/tmp_paths.gd")
##     var path := TmpPaths.path("ai_scale_bench.json")  # G:/opengs_tmp/...
##     var dir := TmpPaths.dir("world_sweep")            # cria e devolve

const DEFAULT_ROOT := "G:/opengs_tmp"

static var _root_cache := ""


## Raiz dos temporários: `OPENGS_TMP` -> `G:/opengs_tmp` -> temp do SO.
## Garante que a pasta existe e aceita escrita.
static func root() -> String:
	if not _root_cache.is_empty():
		return _root_cache
	var candidate := OS.get_environment("OPENGS_TMP").strip_edges()
	if candidate.is_empty():
		candidate = DEFAULT_ROOT
	candidate = candidate.replace("\\", "/").trim_suffix("/")
	if not candidate.is_empty() and DirAccess.make_dir_recursive_absolute(candidate) == OK \
			and _is_writable(candidate):
		_root_cache = candidate
		return _root_cache
	var fallback := OS.get_temp_dir().replace("\\", "/").trim_suffix("/")
	DirAccess.make_dir_recursive_absolute(fallback)
	_root_cache = fallback
	return _root_cache


## Arquivo dentro da raiz.
static func path(file_name: String) -> String:
	return root().path_join(file_name)


## Subpasta dentro da raiz (criada se faltar).
static func dir(name: String) -> String:
	var full := root().path_join(name)
	DirAccess.make_dir_recursive_absolute(full)
	return full


static func _is_writable(directory: String) -> bool:
	var probe := directory.path_join(".write_probe")
	var file := FileAccess.open(probe, FileAccess.WRITE)
	if file == null:
		return false
	file.store_string("ok")
	file.close()
	DirAccess.remove_absolute(probe)
	return true
