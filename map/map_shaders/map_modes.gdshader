shader_type spatial;

// Camada de calor dos modos de mapa. Recebe cidades já normalizadas (UV e valor
// 0..1) e desenha um campo suave por cidade — sem geometria por cidade e sem
// reconstruir nada ao trocar de modo. A base geográfica permanece intacta.

render_mode unshaded, blend_mix, depth_draw_never, cull_disabled;

uniform vec2 city_uv[16];
uniform float city_value[16];
uniform int city_count = 0;

uniform vec3 low_color : source_color = vec3(0.16, 0.24, 0.28);
uniform vec3 high_color : source_color = vec3(0.79, 0.64, 0.29);
uniform float heat_radius : hint_range(0.02, 0.25) = 0.055;
uniform float opacity : hint_range(0.0, 1.0) = 0.45;

void fragment() {
	float heat = 0.0;
	for (int index = 0; index < 16; index++) {
		if (index >= city_count) {
			break;
		}
		// UV é 2:1; corrige o eixo X para o halo ficar circular no mundo.
		vec2 delta = vec2((UV.x - city_uv[index].x) * 2.0, UV.y - city_uv[index].y);
		float weight = exp(-dot(delta, delta) / (heat_radius * heat_radius));
		heat = max(heat, weight * city_value[index]);
	}

	heat = clamp(heat, 0.0, 1.0);
	if (heat <= 0.004) {
		discard;
	}

	ALBEDO = mix(low_color, high_color, heat);
	ALPHA = heat * opacity;
}
