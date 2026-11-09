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

// repeat igual ao do mundo: o Godot exige samplers idênticos em dyn_block().
uniform sampler2D region_sdf : filter_linear_mipmap, repeat_enable;
uniform sampler2D region_color : source_color, filter_linear_mipmap, repeat_disable;
uniform sampler2D world_sdf : filter_linear_mipmap, repeat_enable;
uniform sampler2D world_color : source_color, filter_linear_mipmap, repeat_enable;

// Território dinâmico: id da província por pixel e dono por id.
uniform sampler2D region_province : filter_nearest, repeat_enable;
uniform sampler2D world_province : filter_nearest, repeat_enable;
uniform sampler2D province_owner : source_color, filter_nearest, repeat_disable;
uniform int owner_columns = 64;
uniform bool dynamic_territory = false;

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

// Bloco de território dinâmico de UMA camada (mesmas regras do atlas político).
void dyn_block(sampler2D sdf_tex, sampler2D prov_tex, vec2 uv,
		out float out_differ, out vec3 out_nation) {
	vec2 e = 1.0 / vec2(textureSize(sdf_tex, 0));
	float b_tx = (texture(sdf_tex, uv).b * 255.0 - 128.0) / 8.0;
	float b_l = (texture(sdf_tex, uv - vec2(e.x, 0.0)).b * 255.0 - 128.0) / 8.0;
	float b_r = (texture(sdf_tex, uv + vec2(e.x, 0.0)).b * 255.0 - 128.0) / 8.0;
	float b_d = (texture(sdf_tex, uv - vec2(0.0, e.y)).b * 255.0 - 128.0) / 8.0;
	float b_u = (texture(sdf_tex, uv + vec2(0.0, e.y)).b * 255.0 - 128.0) / 8.0;
	vec2 g = vec2(b_r - b_l, b_u - b_d) * 0.5;
	float gl = length(g);
	vec2 dir = (gl > 1e-4) ? -sign(b_tx) * g / gl : vec2(0.0);
	float reliable = step(abs(gl - 1.0), 0.5);
	vec2 here_uv = (abs(b_tx) < 1.5 && reliable > 0.5) ? uv - dir * e * 1.5 : uv;
	vec2 other_uv = uv + dir * e * (abs(b_tx) + 1.0);
	float id_here = atlas_province_id(prov_tex, here_uv);
	float id_other = atlas_province_id(prov_tex, other_uv);
	vec4 owner_here = atlas_owner(province_owner, owner_columns, id_here);
	vec4 owner_other = atlas_owner(province_owner, owner_columns, id_other);
	float differ = (round(owner_here.a * 255.0) != round(owner_other.a * 255.0)) ? 1.0 : 0.0;
	out_differ = differ * reliable;
	out_nation = owner_here.rgb;
}

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
	float province_r = atlas_province_signed(r_sdf.b, region_texel);
	float province_w = atlas_province_signed(w_sdf.b, world_texel);

	float coast = mix(coast_w, coast_r, region_w);
	float border_signed = mix(border_w, border_r, region_w);
	float province_signed = mix(province_w, province_r, region_w);
	float border = abs(border_signed);
	vec3 nation = mix(w_col, r_col, region_w);

	// Gradiente medido no campo COM sinal (liso na linha). Um gradiente muito
	// maior que o pixel indica o salto de sinal do eixo medial: não é linha.
	float px_coast = pixel_along(coast, px_floor);
	float px_border = pixel_along(border_signed, px_floor);
	float px_province = pixel_along(province_signed, px_floor);
	float border_real = step(px_border, px_max * 2.5);
	float province_real = step(px_province, px_max * 2.5);

	// --- Território dinâmico (região e mundo) -------------------------------
	float region_differ = 0.0;
	float world_differ = 0.0;
	vec3 region_dyn_nation = nation;
	vec3 world_dyn_nation = nation;
	if (dynamic_territory) {
		dyn_block(region_sdf, region_province, ruv, region_differ, region_dyn_nation);
		dyn_block(world_sdf, world_province, UV, world_differ, world_dyn_nation);
	}
	float differ = (region_w > 0.5) ? region_differ : world_differ;
	vec3 dyn_nation = (region_w > 0.5) ? region_dyn_nation : world_dyn_nation;

	// Cor da nação passa a ser a cor do dono.
	if (dynamic_territory) {
		nation = dyn_nation;
	}

	// Terra (máscara suave da costa) para limitar preenchimento e fronteira.
	float land = smoothstep(-0.5, 0.5, coast / px_coast);

	// Fronteira de país em tinta, descartando o salto do eixo medial. Com
	// território dinâmico, a fronteira é a divisa entre donos (região e mundo).
	float eff_border = border;
	float eff_border_px = px_border;
	float eff_border_real = border_real;
	if (dynamic_territory) {
		float region_border = (region_differ > 0.5) ? abs(province_r) : 1e3;
		float world_border = (world_differ > 0.5) ? abs(province_w) : 1e3;
		float dyn_border = (region_w > 0.5) ? region_border : world_border;
		eff_border = dyn_border;
		eff_border_px = px_province;
		eff_border_real = province_real;
	}

	float country_px = max(country_width_px, country_width_world / eff_border_px);
	float country_line = ink_line(eff_border, eff_border_px, country_px) * land * eff_border_real;

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
