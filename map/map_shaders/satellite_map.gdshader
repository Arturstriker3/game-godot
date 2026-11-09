shader_type spatial;

// Base geográfica do mapa-múndi: satélite do World Imagery com gradação
// cartográfica. Oceano é regradado por profundidade (luminância) e plataforma
// costeira; o relevo vem do normal map do mapa antigo, aplicado como hillshade
// de luz fixa noroeste. Tudo em unshaded para manter leitura constante em
// qualquer ângulo de câmera, sem brilho especular nem aparência de plástico.
//
// Reaproveita map_data/normal.png (5000×2500, gerado a partir do height.png)
// para não amostrar a altura quatro vezes por pixel.

render_mode unshaded, cull_disabled;

uniform sampler2D map_texture : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D relief_texture : filter_linear_mipmap_anisotropic, repeat_enable;

// Terra
uniform float land_saturation : hint_range(0.0, 2.0) = 0.95;
uniform float land_contrast : hint_range(0.5, 2.0) = 1.02;
uniform float land_brightness : hint_range(0.5, 1.5) = 1.09;

// Oceano (valores em espaço linear: o tonemapper converte para sRGB)
uniform vec3 ocean_deep : source_color = vec3(0.057, 0.110, 0.141);
uniform vec3 ocean_mid : source_color = vec3(0.084, 0.160, 0.198);
uniform vec3 ocean_shallow : source_color = vec3(0.137, 0.262, 0.289);
uniform float ocean_detail : hint_range(0.0, 1.0) = 0.50;
uniform float ocean_saturation : hint_range(0.0, 1.5) = 0.90;
uniform float ocean_depth_bias : hint_range(0.0, 0.5) = 0.20;

// Relevo
uniform float relief_strength : hint_range(0.0, 3.0) = 2.2;
uniform float relief_ambient : hint_range(0.0, 1.0) = 0.62;
uniform vec3 relief_light_dir = vec3(-0.55, 0.55, 0.62);

// Plataforma costeira
uniform vec2 shelf_offset = vec2(0.006, 0.009);
uniform float shelf_strength : hint_range(0.0, 1.0) = 1.0;

// Vinheta de latitude: escurece os polos para esconder a borda do mundo.
uniform float vignette_start : hint_range(0.0, 0.5) = 0.15;
uniform float vignette_strength : hint_range(0.0, 1.0) = 0.5;
uniform vec3 vignette_color : source_color = vec3(0.04, 0.06, 0.06);

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

float landness(vec3 color) {
	float dominance = max(color.r, color.g) - color.b;
	return smoothstep(-0.02, 0.055, dominance);
}

void fragment() {
	vec3 src = texture(map_texture, UV).rgb;
	float lum = dot(src, LUMA);
	float land = landness(src);

	// Terra: gradação conservadora; sem sharpen agressivo para não gerar halos.
	vec3 earth = mix(vec3(lum), src, land_saturation);
	earth = (earth - 0.5) * land_contrast + 0.5;
	earth *= land_brightness;

	// Relevo: normal reconstruída (funciona com BC5/normal map comprimido) e
	// hillshade relativo — terreno plano fica exatamente em 1.0, encostas
	// variam em torno disso, e o piso de ambiente evita sombras pretas.
	vec2 normal_xy = texture(relief_texture, UV).xy * 2.0 - 1.0;
	vec3 normal = vec3(normal_xy, sqrt(max(0.0, 1.0 - dot(normal_xy, normal_xy))));
	vec3 light_dir = normalize(relief_light_dir);
	float relief = (dot(normal, light_dir) - light_dir.z) * relief_strength;
	earth *= mix(1.0, clamp(1.0 + relief, relief_ambient, 1.35), land);

	// Plataforma: média da vizinhança ainda "terra" para achar a costa.
	float around = 0.25 * (
		landness(texture(map_texture, UV + vec2(shelf_offset.x, 0.0)).rgb)
		+ landness(texture(map_texture, UV - vec2(shelf_offset.x, 0.0)).rgb)
		+ landness(texture(map_texture, UV + vec2(0.0, shelf_offset.y)).rgb)
		+ landness(texture(map_texture, UV - vec2(0.0, shelf_offset.y)).rgb));
	float shelf = smoothstep(0.0, 0.65, around);

	// Oceano: profundidade pela luminância do satélite (escuro = fundo) e
	// transições suaves entre raso, médio e profundo.
	float depth = clamp((ocean_depth_bias - lum) * 5.0, 0.0, 1.0);
	vec3 ocean = mix(ocean_shallow, ocean_mid, smoothstep(0.0, 0.50, depth));
	ocean = mix(ocean, ocean_deep, smoothstep(0.45, 1.0, depth));
	ocean = mix(ocean, ocean_shallow * 1.15, shelf * shelf_strength * 0.55);

	// Mantém a batimetria do satélite como variação de brilho, sem reintroduzir
	// o ciano: o detalhe é multiplicativo e monocromático.
	ocean *= 1.0 + (lum - 0.12) * 1.6 * ocean_detail;
	ocean = mix(vec3(dot(ocean, LUMA)), ocean, ocean_saturation);

	vec3 color = mix(ocean, earth, land);

	// Dither só na água: quebra o banding dos gradientes de 8 bits do JPEG.
	float noise = fract(sin(dot(UV * 2048.0, vec2(12.9898, 78.233))) * 43758.5453);
	color += (noise - 0.5) / 255.0 * (1.0 - land);

	// Vinheta de latitude: escurece perto de UV.y = 0 e UV.y = 1
	float dist_from_edge = min(UV.y, 1.0 - UV.y);
	float vignette = smoothstep(0.0, vignette_start, dist_from_edge);
	color = mix(vignette_color, color, mix(1.0 - vignette_strength, 1.0, vignette));

	ALBEDO = clamp(color, 0.0, 1.0);
}
