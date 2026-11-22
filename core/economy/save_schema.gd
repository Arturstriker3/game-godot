extends RefCounted

## Versão atual do schema de save — leitura somente.
##
## O número mora no núcleo C++ (`ogs::kSaveVersion`, sim_constants.h) e sobe a
## cada bloco novo do save (v37 hoje, D-120). Testes NÃO devem fixar o literal:
## comparar com [method current] mantém a asserção forte (o save gravado tem de
## sair exatamente na versão do núcleo) sem apodrecer quando o schema sobe.
##
## Uso:
##   const SaveSchema := preload("res://core/economy/save_schema.gd")
##   _check(int(save.get("version", 0)) == SaveSchema.current(), ...)


## Versão do schema publicada por `EconomySim.get_save_version()`.
static func current() -> int:
	var probe := EconomySim.new()
	if probe == null:
		return 0
	return int(probe.get_save_version())
