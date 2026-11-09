shader_type spatial;

// Camada geopolítica transparente sobre a base (satélite nos modos de calor).
//
// Lê os mesmos campos de distância do modo político (map/map_data/atlas/) em
// vez dos rasters antigos (`country_borders.png`/`nation_fill.png`/`nav_mask`),
// então fronteiras e costa ficam nítidas em qualquer zoom — sem a escadinha
// das máscaras ampliadas. Sem divisas de província: só preenchimento da nação,
// fronteira de país e linha de costa.
//
// Composição "over" em alfa pré-multiplicado (ALBEDO = premult/alpha), o que
// evita o halo branco onde o preenchimento é fraco.

render_mode unshaded, cull_disabled;

#include "res://map/map_shaders/atlas_common.gdshaderinc"

uniform sampler2D region_sdf : filter_linear_mipmap, repeat_disable;
uniform sampler2D region_color : source_color, filter_linear_mipmap, repeat_disable;
uniform sampler2D world_sdf : filter_linear_mipmap, repeat_enable;
uniform sampler2D world_color : source_color, filter_linear_mipmap, repeat_enable;

// Janela da região em graus e tamanho de texel (iguais ao gerador e ao atlas).
uniform vec4 region_bounds = vec4(-25.0, 33.0, 45.0, 72.0);
uniform float region_texel = 0.047471;
uniform float world_texel = 0.244141;

uniform vec4 border_color : source_color = vec4(0.070, 0.085, 0.090, 1.0);
uniform vec4 coast_color : source_color = vec4(0.10, 0.16, 0.16, 1.0);
uniform float border_opacity : hint_range(0.0, 1.0) = 0.45;
uniform float fill_opacity : hint_range(0.0, 1.0) = 0.11;

// Largura das linhas em pixels de tela (mesma regra do atlas político).
uniform float country_width_px = 1.8;
uniform float country_width_world = 0.052;
uniform float coast_width_px = 1.15;

// Opacidade da linha de costa (equivalente ao modo de calor do overlay antigo).
const float COAST_OPACITY = 0.45;

void fragment() {
	vec2 plane = UV * vec2(1000.0, 500.0);
	vec2 dx = dFdx(plane);
	vec2 dy = dFdy(plane);
	float px_floor = max(min(length(dx), length(dy)), 1e-5);
	float px_max = max(length(dx), length(dy));

	// --- Amostragem: região em alta resolução sobre o mundo -----------------
	vec2 ruv = atlas_region_uv(UV, region_bounds);
	float region_w = atlas_region_weight(ruv);

	vec3 w_sdf = texture(world_sdf, UV).rgb;
	vec3 w_col = texture(world_color, UV).rgb;
	vec3 r_sdf = texture(region_sdf, ruv).rgb;
	vec3 r_col = texture(region_color, ruv).rgb;

	float coast_w = atlas_coast_distance(w_sdf.r, world_texel);
	float border_w = atlas_border_signed(w_sdf.g, world_texel);
	float coast_r = atlas_coast_distance(r_sdf.r, region_texel);
	float border_r = atlas_border_signed(r_sdf.g, region_texel);

	float coast = mix(coast_w, coast_r, region_w);
	float border_signed = mix(border_w, border_r, region_w);
	float border = abs(border_signed);
	vec3 nation = mix(w_col, r_col, region_w);

	// Gradiente medido no campo COM sinal (liso na linha). Um gradiente muito
	// maior que o pixel indica o salto de sinal do eixo medial: não é linha.
	float px_coast = pixel_along(coast, px_floor);
	float px_border = pixel_along(border_signed, px_floor);
	float border_real = step(px_border, px_max * 2.5);

	// Terra (máscara suave da costa) para limitar preenchimento e fronteira.
	float land = smoothstep(-0.5, 0.5, coast / px_coast);

	// Fronteira de país em tinta, descartando o salto do eixo medial.
	float country_px = max(country_width_px, country_width_world / px_border);
	float country_line = ink_line(border, px_border, country_px) * land * border_real;

	// Linha de costa fina.
	float coast_line = ink_line(abs(coast), px_coast, coast_width_px);

	// --- Composição "over" em alfa pré-multiplicado -------------------------
	float fill_alpha = land * fill_opacity;
	vec3 premult = nation * fill_alpha;
	float alpha = fill_alpha;

	float border_alpha = country_line * border_opacity;
	premult = border_color.rgb * border_alpha + premult * (1.0 - border_alpha);
	alpha = border_alpha + alpha * (1.0 - border_alpha);

	float coast_alpha = coast_line * COAST_OPACITY;
	premult = coast_color.rgb * coast_alpha + premult * (1.0 - coast_alpha);
	alpha = coast_alpha + alpha * (1.0 - coast_alpha);

	if (alpha <= 0.0001) {
		discard;
	}

	ALBEDO = premult / alpha;
	ALPHA = alpha;
}
