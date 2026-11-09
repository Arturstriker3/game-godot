shader_type spatial;

// Fundo de satélite: mantém as cores reais do mosaico World Imagery, aplica
// gradação leve (contraste/saturação/brilho) e destaca a plataforma costeira,
// sem competir com a camada geopolítica desenhada por cima.

render_mode unshaded, cull_disabled;

uniform sampler2D map_texture : source_color, filter_linear_mipmap_anisotropic, repeat_enable;

uniform float saturation : hint_range(0.0, 2.0) = 1.06;
uniform float contrast : hint_range(0.5, 2.0) = 1.04;
uniform float brightness : hint_range(0.5, 1.5) = 1.0;

uniform float shelf_strength : hint_range(0.0, 1.0) = 0.30;
uniform vec2 shelf_offset = vec2(0.006, 0.009);

float landness(vec3 color) {
	float dominance = max(color.r, color.g) - color.b;
	return smoothstep(-0.02, 0.070, dominance);
}

void fragment() {
	vec3 src = texture(map_texture, UV).rgb;
	float lum = dot(src, vec3(0.299, 0.587, 0.114));

	vec3 color = mix(vec3(lum), src, saturation);
	color = (color - 0.5) * contrast + 0.5;
	color *= brightness;

	// Água rasa (perto da costa) ganha um leve tom de plataforma; o mar aberto
	// fica escuro para não competir com fronteiras e preenchimentos.
	float around = 0.25 * (
		landness(texture(map_texture, UV + vec2(shelf_offset.x, 0.0)).rgb)
		+ landness(texture(map_texture, UV - vec2(shelf_offset.x, 0.0)).rgb)
		+ landness(texture(map_texture, UV + vec2(0.0, shelf_offset.y)).rgb)
		+ landness(texture(map_texture, UV - vec2(0.0, shelf_offset.y)).rgb));
	float water = 1.0 - landness(src);
	float shelf = smoothstep(0.02, 0.55, around);
	color += vec3(0.012, 0.028, 0.036) * water * shelf * shelf_strength;

	ALBEDO = clamp(color, 0.0, 1.0);
}
