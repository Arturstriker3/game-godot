shader_type spatial;

// Base chapada do modo atlas: água teal lisa, terra parchment. As cores das
// nações e as fronteiras são aplicadas pela camada political_overlay por cima.
// Substitui o satélite no modo político para dar a leitura de atlas histórico.

render_mode unshaded, cull_disabled;

uniform sampler2D nav_mask : filter_linear_mipmap, repeat_disable;
uniform vec3 water_color : source_color = vec3(0.180, 0.357, 0.361);  // teal #2E5B5C
uniform vec3 land_color : source_color = vec3(0.910, 0.875, 0.780);   // parchment #E8DFC7

void fragment() {
	float nav = texture(nav_mask, UV).r;
	// nav_mask: 0 = água, 128 = costa, 255 = terra
	float land = smoothstep(0.45, 0.55, nav);
	ALBEDO = mix(water_color, land_color, land);
}
