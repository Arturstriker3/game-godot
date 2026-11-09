shader_type spatial;

// Camada geopolítica: preenchimento das nações e fronteiras reais por cima do
// terreno de satélite. Uma única passada transparente, sem reconstruir o mapa.

render_mode unshaded, cull_disabled;

uniform sampler2D fill_texture : source_color, filter_linear_mipmap, repeat_disable;
uniform sampler2D border_texture : filter_linear_mipmap, repeat_disable;

uniform vec4 border_color : source_color = vec4(0.070, 0.085, 0.090, 1.0);
uniform float border_opacity : hint_range(0.0, 1.0) = 0.45;
uniform float fill_opacity : hint_range(0.0, 1.0) = 0.11;
uniform vec3 player_tint : source_color = vec3(1.0, 1.0, 1.0);

void fragment() {
	vec4 fill = texture(fill_texture, UV);
	float border = texture(border_texture, UV).r;

	// Preenchimento da nação por baixo, fronteira por cima (composição "over").
	float fill_alpha = fill.a * fill_opacity;
	float border_alpha = border * border_opacity;
	float alpha = border_alpha + fill_alpha * (1.0 - border_alpha);
	if (alpha <= 0.0001) {
		discard;
	}

	vec3 fill_color = fill.rgb * player_tint;
	vec3 color = (border_color.rgb * border_alpha
		+ fill_color * fill_alpha * (1.0 - border_alpha)) / alpha;

	ALBEDO = color;
	ALPHA = alpha;
}
