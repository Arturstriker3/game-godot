shader_type spatial;

// Base cartográfica branca: terra em marfim, mar branco, relevo suave e apenas
// o contorno dos continentes. Sem fronteiras de país ou província — a ideia é
// ser o fundo neutro para os mapas de densidade e de recursos.
//
// A costa vem dos mesmos campos de distância do Natural Earth usados pelo
// atlas político (tools/build_atlas_layers.py), então a linha é reconstruída
// com espessura em pixels de tela e anti-aliasing em qualquer zoom.

render_mode unshaded, cull_disabled;

#include "res://map/map_shaders/atlas_common.gdshaderinc"

uniform sampler2D region_sdf : filter_linear_mipmap, repeat_enable;
uniform sampler2D world_sdf : filter_linear_mipmap, repeat_enable;
uniform sampler2D relief_texture : filter_linear_mipmap_anisotropic, repeat_enable;

uniform vec4 region_bounds = vec4(-25.0, 33.0, 45.0, 72.0);
uniform float region_texel = 0.047471;
uniform float world_texel = 0.122070;

uniform vec3 paper_color : source_color = vec3(0.949, 0.929, 0.878);
uniform vec3 sea_color : source_color = vec3(1.0, 1.0, 1.0);
uniform vec3 coast_ink : source_color = vec3(0.235, 0.216, 0.184);
uniform float coast_width_px = 1.25;

uniform float relief_strength : hint_range(0.0, 1.0) = 0.16;
uniform float grain_strength : hint_range(0.0, 0.1) = 0.018;

uniform float vignette_start : hint_range(0.0, 0.5) = 0.08;
uniform float vignette_strength : hint_range(0.0, 1.0) = 0.35;
uniform vec3 vignette_color : source_color = vec3(0.0392, 0.0549, 0.0588);

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

float hash(vec2 p) {
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float value_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = hash(i);
	float b = hash(i + vec2(1.0, 0.0));
	float c = hash(i + vec2(0.0, 1.0));
	float d = hash(i + vec2(1.0, 1.0));
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void fragment() {
	// Unidades do plano (1000 × 500) para medir linhas em pixels de tela.
	vec2 plane = UV * vec2(1000.0, 500.0);
	vec2 dx = dFdx(plane);
	vec2 dy = dFdy(plane);
	float px_floor = max(min(length(dx), length(dy)), 1e-5);

	// Costa: região (Europa) em alta resolução sobre o mundo, com mistura suave.
	vec2 ruv = atlas_region_uv(UV, region_bounds);
	float region_w = atlas_region_weight(ruv);
	float coast_w = atlas_coast_distance(texture(world_sdf, UV).r, world_texel);
	float coast_r = atlas_coast_distance(texture(region_sdf, ruv).r, region_texel);
	float coast = mix(coast_w, coast_r, region_w);

	// Terra/mar com transição sub-pixel (a linha de costa cobre a emenda).
	float land = smoothstep(-0.02, 0.02, coast);

	// Relevo: hillshade relativo da normal (placeholder plano hoje; quando
	// houver DEM real, o mapa ganha sombra sem mudar mais nada).
	vec2 normal_xy = texture(relief_texture, UV).xy * 2.0 - 1.0;
	vec3 normal = vec3(normal_xy, sqrt(max(0.0, 1.0 - dot(normal_xy, normal_xy))));
	vec3 light_dir = normalize(vec3(-0.55, 0.55, 0.62));
	float relief = (dot(normal, light_dir) - light_dir.z) * relief_strength;

	// Grão de papel: alta frequência fina + mancha de baixa frequência.
	float fine = value_noise(plane * 95.0) - 0.5;
	float blotch = value_noise(plane * 3.2) - 0.5;
	vec3 land_col = paper_color * (1.0 + relief + fine * grain_strength + blotch * 0.018);

	// Mar branco com variação quase imperceptível (evita parecer plástico).
	float sea_var = value_noise(plane * 26.0) - 0.5;
	vec3 sea_col = sea_color * (1.0 + sea_var * 0.006);

	vec3 color = mix(sea_col, land_col, land);

	// Contorno dos continentes, anti-aliasado e com espessura fixa em pixels.
	// abs() porque o campo é assinado: sem isso o oceano inteiro viraria tinta.
	float px_coast = pixel_along(coast, px_floor);
	float coast_line = ink_line(abs(coast), px_coast, coast_width_px);
	color = mix(color, coast_ink, coast_line);

	// Vinheta polar: funde a borda do mundo com o fundo do ambiente.
	float dist_edge = min(UV.y, 1.0 - UV.y);
	float vignette = smoothstep(0.0, vignette_start, dist_edge);
	color = mix(vignette_color, color, mix(1.0 - vignette_strength, 1.0, vignette));

	ALBEDO = clamp(color, 0.0, 1.0);
}
