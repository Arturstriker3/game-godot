shader_type spatial;

// Mapa tático: converte o relevo Natural Earth em uma carta militar impressa
// (terra oliva, mar azul-petróleo, gelo pálido), com graticule e correção de tom.

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
uniform float grid_opacity : hint_range(0.0, 0.5) = 0.07;
uniform float shelf_radius : hint_range(0.0, 0.02) = 0.0032;
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

	// Gelo: claro e dessaturado.
	float saturation = max(max(src.r, src.g), src.b) - min(min(src.r, src.g), src.b);
	float ice = smoothstep(0.78, 0.92, lum) * (1.0 - smoothstep(0.06, 0.18, saturation));

	float land = max(landness(src), ice);

	// Costa: média de terra em quatro direções, para dar profundidade ao mar.
	vec2 offset = vec2(shelf_radius, shelf_radius * 1.5);
	float around = landness(texture(map_texture, uv + vec2(offset.x, 0.0)).rgb)
			+ landness(texture(map_texture, uv - vec2(offset.x, 0.0)).rgb)
			+ landness(texture(map_texture, uv + vec2(0.0, offset.y)).rgb)
			+ landness(texture(map_texture, uv - vec2(0.0, offset.y)).rgb);
	around *= 0.25;

	vec3 water = mix(ocean_deep.rgb, ocean_mid.rgb, smoothstep(0.0, 0.6, around));
	water = mix(water, ocean_shelf.rgb, smoothstep(0.4, 0.95, around));

	// Relevo aproximado pela própria luminância, sem derivadas.
	vec3 ground = mix(land_low.rgb, land_mid.rgb, smoothstep(0.28, 0.62, lum));
	ground = mix(ground, land_high.rgb, smoothstep(0.58, 0.86, lum));

	vec3 color = mix(water, ground, land);
	color = mix(color, ice_color.rgb, ice * ice_blend);

	// Graticule fino e anti-aliased.
	vec2 coord = uv / grid_step;
	vec2 distance_to_line = abs(fract(coord - 0.5) - 0.5) / max(fwidth(coord), vec2(1e-5));
	float line = 1.0 - min(min(distance_to_line.x, distance_to_line.y), 1.0);
	color = mix(color, grid_color.rgb, line * grid_opacity);

	ALBEDO = color;
}
