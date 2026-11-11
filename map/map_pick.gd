extends RefCounted
class_name MapPick

## Regras ÚNICAS de picking no mapa: cidade, porto/construção e jazida usam o
## MESMO raio mínimo em pixels de TELA. Acima do mínimo, o raio cresce com o
## tamanho desenhado do ícone (footprint projetado pela câmera), então o hover
## acompanha o desenho em qualquer zoom.
##
## Não use valores por tipo (26 px para cidade, 20 px para porto etc.): todo
## picking passa por `pick_radius_px()`.

## Piso de detecção em pixels de tela — o raio do hover da cidade aprovado.
const MIN_RADIUS_PX := 26.0
## A jazida é desenhada como um quad de lado MARKER_SIZE (raio visível =
## metade); o pick aplica 1,35× de folga sobre esse raio, como no anel de hover.
const DEPOSIT_RADIUS_FACTOR := 1.35


## Raio cru do ícone em pixels de tela (sem o piso), para câmera válida.
## Devolve 0 se o ponto estiver atrás da câmera.
static func icon_radius_px(camera: Camera3D, world: Vector3,
		world_radius: float) -> float:
	if camera == null or camera.is_position_behind(world):
		return 0.0
	var right := camera.global_transform.basis.x
	var center := camera.unproject_position(world)
	var edge := camera.unproject_position(world + right * maxf(world_radius, 0.001))
	return center.distance_to(edge)


## Raio de detecção em pixels de tela: o piso comum ou o ícone projetado
## (vezes `factor`, quando o tipo desenha com folga sobre o raio visível).
static func pick_radius_px(camera: Camera3D, world: Vector3, world_radius: float,
		factor: float = 1.0) -> float:
	return maxf(MIN_RADIUS_PX, icon_radius_px(camera, world, world_radius) * factor)
