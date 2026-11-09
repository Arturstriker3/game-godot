shader_type spatial;

// Camada geopolítica: preenchimento das nações, fronteiras e linha de costa.
// Uma única passada transparente sobre a base (satélite ou atlas chapado).
// A borda é alargada por amostragem em cruz e suavizada por smoothstep para
// não ficar serrilhada; a costa vem da transição terra/água do nav_mask.

render_mode unshaded, cull_disabled;

uniform sampler2D fill_texture : source_color, filter_linear_mipmap, repeat_disable;
uniform sampler2D border_texture : filter_linear_mipmap, repeat_disable;
uniform sampler2D nav_mask : filter_linear_mipmap, repeat_disable;

uniform vec4 border_color : source_color = vec4(0.070, 0.085, 0.090, 1.0);
uniform vec4 coast_color : source_color = vec4(0.10, 0.16, 0.16, 1.0);
uniform float border_opacity : hint_range(0.0, 1.0) = 0.45;
uniform float fill_opacity : hint_range(0.0, 1.0) = 0.11;
uniform vec3 player_tint : source_color = vec3(1.0, 1.0, 1.0);

// Modo atlas: opacidade alta para o preenchimento cobrir a base parchment.
uniform bool atlas_mode = false;
uniform float atlas_fill_opacity : hint_range(0.0, 1.0) = 0.88;
uniform float atlas_border_opacity : hint_range(0.0, 1.0) = 0.95;

// Tamanho de um texel de cada textura (passado pelo script).
uniform vec2 border_texel = vec2(0.0002, 0.0004);
uniform vec2 nav_texel = vec2(0.0004, 0.0008);

void fragment() {
	vec4 fill = texture(fill_texture, UV);

	// Fronteira: amostra em cruz para engrossar + smoothstep (anti-aliasing).
	float b = texture(border_texture, UV).r;
	b = max(b, texture(border_texture, UV + vec2(border_texel.x, 0.0)).r);
	b = max(b, texture(border_texture, UV - vec2(border_texel.x, 0.0)).r);
	b = max(b, texture(border_texture, UV + vec2(0.0, border_texel.y)).r);
	b = max(b, texture(border_texture, UV - vec2(0.0, border_texel.y)).r);
	float border_mask = smoothstep(0.10, 0.50, b);

	// Costa: transição terra/água na máscara de navegação.
	float nav = texture(nav_mask, UV).r;
	float nav_a = texture(nav_mask, UV + vec2(nav_texel.x, 0.0)).r;
	float nav_b = texture(nav_mask, UV - vec2(nav_texel.x, 0.0)).r;
	float nav_c = texture(nav_mask, UV + vec2(0.0, nav_texel.y)).r;
	float nav_d = texture(nav_mask, UV - vec2(0.0, nav_texel.y)).r;
	float nav_lo = min(nav, min(min(nav_a, nav_b), min(nav_c, nav_d)));
	float nav_hi = max(nav, max(max(nav_a, nav_b), max(nav_c, nav_d)));
	float coast_mask = smoothstep(0.06, 0.28, nav_hi - nav_lo);

	float eff_fill_opacity = atlas_mode ? atlas_fill_opacity : fill_opacity;
	float eff_border_opacity = atlas_mode ? atlas_border_opacity : border_opacity;

	float fill_alpha = fill.a * eff_fill_opacity;
	float border_alpha = border_mask * eff_border_opacity;
	float coast_alpha = coast_mask * (atlas_mode ? 0.9 : 0.45);

	// Composição "over": preenchimento -> fronteira -> costa.
	float alpha = fill_alpha;
	vec3 color = fill.rgb * player_tint;

	alpha = border_alpha + alpha * (1.0 - border_alpha);
	color = (border_color.rgb * border_alpha + color * (1.0 - border_alpha)) / max(alpha, 0.0001);

	alpha = coast_alpha + alpha * (1.0 - coast_alpha);
	color = (coast_color.rgb * coast_alpha + color * (1.0 - coast_alpha)) / max(alpha, 0.0001);

	if (alpha <= 0.0001) {
		discard;
	}

	ALBEDO = color;
	ALPHA = alpha;
}
