shader_type spatial;

// Mapa tático: converte o relevo Natural Earth em uma carta militar impressa
// (terra oliva, mar azul-petróleo, gelo pálido), com graticule e grão de papel.

render_mode unshaded, cull_disabled;

uniform sampler2D map_texture : source_color, filter_linear_mipmap_anisotropic, repeat_enable;

uniform vec4 ocean_deep : source_color = vec4(0.043, 0.098, 0.118, 1.0);
uniform vec4 ocean_mid : source_color = vec4(0.071, 0.157, 0.180, 1.0);
uniform vec4 ocean_shelf : source_color = vec4(0.118, 0.235, 0.259, 1.0);
uniform vec4 land_low : source_color = vec4(0.290, 0.302, 0.216, 1.0);
uniform vec4 land_mid : source_color = vec4(0.396, 0.384, 0.267, 1.0);
uniform vec4 land_high : source_color = vec4(0.545, 0.498, 0.353, 1.0);
uniform vec4 ice_color : source_color = vec4(0.729, 0.741, 0.706, 1.0);
uniform vec4 grid_color : source_color = vec4(0.784, 0.725, 0.486, 1.0);

uniform vec2 grid_step = vec2(0.0416667, 0.0833333);
uniform float grid_opacity : hint_range(0.0, 0.5) = 0.085;
uniform float relief_strength : hint_range(0.0, 3.0) = 1.15;
uniform float grain_strength : hint_range(0.0, 0.2) = 0.045;
uniform float ice_blend : hint_range(0.0, 1.0) = 0.9;

float luminance(vec3 c) {
	return dot(c, vec3(0.299, 0.587, 0.114));
}

// Terra quando o verde/vermelho domina o azul; oceano quando o azul domina.
float landness(vec3 c) {
	float dominance = max(c.r, c.g) - c.b;
	return smoothstep(-0.015, 0.085, dominance);
}

void fragment() {
	vec2 uv = UV;
	vec3 src = texture(map_texture, uv).rgb;

	float lum = luminance(src);
	float saturation = max(max(src.r, src.g), src.b) - min(min(src.r, src.g), src.b);
	float ice = smoothstep(0.78, 0.92, lum) * (1.0 - smoothstep(0.06, 0.18, saturation));

	float land = landness(src);
	land = max(land, ice);

	// Relevo: gradiente da luminância em quatro direções.
	vec2 texel = vec2(0.0011, 0.0017);
	float lum_r = luminance(texture(map_texture, uv + vec2(texel.x, 0.0)).rgb);
	float lum_l = luminance(texture(map_texture, uv - vec2(texel.x, 0.0)).rgb);
	float lum_u = luminance(texture(map_texture, uv + vec2(0.0, texel.y)).rgb);
	float lum_d = luminance(texture(map_texture, uv - vec2(0.0, texel.y)).rgb);
	float relief = clamp(0.5 + (lum_r - lum_l + lum_u - lum_d) * relief_strength * 5.0, 0.0, 1.0);

	// Profundidade: média de terra nos arredores aproxima a distância à costa.
	float around = 0.0;
	for (int i = 0; i < 8; i++) {
		float angle = float(i) * 0.7853982;
		vec2 offset = vec2(cos(angle), sin(angle)) * vec2(0.0022, 0.0033);
		around += landness(texture(map_texture, uv + offset).rgb);
	}
	around /= 8.0;

	vec3 water = mix(ocean_deep, ocean_mid, smoothstep(0.0, 0.65, around));
	water = mix(water, ocean_shelf, smoothstep(0.45, 0.95, around));

	vec3 ground = mix(land_low, land_high, relief);
	ground = mix(ground, land_mid, smoothstep(0.18, 0.82, lum) * 0.40);

	vec3 color = mix(water, ground, land);
	color = mix(color, ice_color, ice * ice_blend);

	// Mar recebe um leve relevo para não ficar chapado.
	color += (relief - 0.5) * 0.055 * (1.0 - land);

	// Graticule fino e anti-aliased.
	vec2 coord = uv / grid_step;
	vec2 distance_to_line = abs(fract(coord - 0.5) - 0.5) / max(fwidth(coord), vec2(1e-5));
	float line = 1.0 - min(min(distance_to_line.x, distance_to_line.y), 1.0);
	color = mix(color, grid_color.rgb, line * grid_opacity);

	// Grão de impressão, fixo na superfície do mapa.
	float grain = fract(sin(dot(uv * vec2(4096.0, 2048.0), vec2(12.9898, 78.233))) * 43758.5453);
	color += (grain - 0.5) * grain_strength;

	ALBEDO = color;
}
