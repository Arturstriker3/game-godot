shader_type spatial;

// Mapa político "atlas" em uma única passada opaca.
//
// Fonte: campos de distância gerados por tools/build_atlas_layers.py a partir
// dos vetores do Natural Earth (costa, fronteira de país e divisa admin-1).
// Como o shader recebe DISTÂNCIAS e não máscaras, as linhas são reconstruídas
// com espessura em pixels de tela e anti-aliasing em qualquer zoom — sem a
// escadinha das máscaras ampliadas.
//
// Duas resoluções: uma região Europa (~1,9 km/texel) e o mundo inteiro
// (~9,8 km/texel), misturadas na borda da região.
//
// Composição: terreno (luminância do satélite) × cor da nação, faixa de cor
// junto à fronteira, divisas de província, mar com batimetria de costa e
// linhas de nível, linha de costa e fronteiras em tinta.

render_mode unshaded, cull_disabled;

#include "res://map/map_shaders/atlas_common.gdshaderinc"

// repeat igual ao do mundo: o Godot exige samplers idênticos em dyn_block().
uniform sampler2D region_sdf : filter_linear_mipmap, repeat_enable;
uniform sampler2D region_color : source_color, filter_linear_mipmap, repeat_disable;
uniform sampler2D world_sdf : filter_linear_mipmap, repeat_enable;
uniform sampler2D world_color : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D terrain_texture : source_color, filter_linear_mipmap_anisotropic, repeat_enable;

// Território dinâmico: id da província por pixel e dono por id (RGB8, sem
// mipmap/compressão; a = índice do dono). Válido na região E no mundo inteiro.
uniform sampler2D region_province : filter_nearest, repeat_enable;
uniform sampler2D world_province : filter_nearest, repeat_enable;
uniform sampler2D province_owner : source_color, filter_nearest, repeat_disable;
uniform int owner_columns = 64;
uniform bool dynamic_territory = false;
uniform float dynamic_band_width = 0.3;

// Janela da região em graus (lon0, lat0, lon1, lat1) — igual ao gerador.
uniform vec4 region_bounds = vec4(-25.0, 33.0, 45.0, 72.0);
// Tamanho de um texel em unidades do plano (plano = 1000 × 500 unidades).
uniform float region_texel = 0.047471;
uniform float world_texel = 0.244141;

// Mar (a faixa rasa precisa caber no alcance da costa na região: 3,0 u)
uniform vec3 sea_deep : source_color = vec3(0.110, 0.204, 0.239);
uniform vec3 sea_shallow : source_color = vec3(0.170, 0.300, 0.330);
uniform vec3 sea_contour : source_color = vec3(0.330, 0.470, 0.490);
uniform float shallow_width = 1.6;

// Terra
uniform float terrain_strength : hint_range(0.0, 2.0) = 1.0;
uniform float nation_saturation : hint_range(0.0, 1.5) = 0.85;
uniform float border_band_width = 0.9;
uniform float border_band_strength : hint_range(0.0, 1.0) = 0.30;

// Tinta
uniform vec3 ink : source_color = vec3(0.090, 0.098, 0.105);
uniform vec3 coast_ink : source_color = vec3(0.075, 0.120, 0.135);
uniform float country_width_px = 1.8;
uniform float country_width_world = 0.052;
uniform float province_width_px = 0.85;
uniform float province_opacity : hint_range(0.0, 1.0) = 0.40;
uniform float coast_width_px = 1.15;

// Vinheta polar: funde a borda do mundo com a cor de fundo do ambiente
// (main_game.tscn), então o horizonte não mostra a quina da prancha.
uniform float vignette_start : hint_range(0.0, 0.5) = 0.09;
uniform vec3 vignette_color : source_color = vec3(0.0392, 0.0549, 0.0588);

const vec3 LUMA = vec3(0.2126, 0.7152, 0.0722);

float hash(vec2 p) {
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float value_noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = hash(i);
	float b = hash(i + vec2(1.0, 0.0));
	float c = hash(i + vec2(0.0, 1.0));
	float d = hash(i + vec2(1.0, 1.0));
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Bloco de território dinâmico de UMA camada: lê o canal B do SDF e o raster
// de id de província, e devolve o dono do lado certo da divisa e se os dois
// lados têm donos diferentes. Igual para a região e para o mundo.
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
	// O campo só é uma distância de verdade onde o gradiente ≈ 1 texel/texel;
	// no eixo medial e nas junções triplas ele salta e a direção não vale.
	float reliable = step(abs(gl - 1.0), 0.5);
	// Cor: perto da divisa, a província 1,5 texel para longe da linha (o
	// sinal do campo é sub-texel, então o preenchimento segue a linha suave
	// e não o degrau do id); no resto, a do próprio pixel.
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
	// Pegada média do pixel (só para decidir fades por zoom) e a menor pegada
	// possível (piso para a medida ao longo do gradiente).
	vec2 dx = dFdx(plane);
	vec2 dy = dFdy(plane);
	float px = max(sqrt(0.5 * (dot(dx, dx) + dot(dy, dy))), 1e-5);
	float px_floor = max(min(length(dx), length(dy)), 1e-5);

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
	// Fronteiras têm sinal (ver gerador): a linha está no cruzamento por zero.
	float border_signed = mix(border_w, border_r, region_w);
	// Divisa admin-1: agora existe nas duas camadas (região e mundo).
	float province_signed = mix(province_w, province_r, region_w);
	float border = abs(border_signed);
	vec3 nation = mix(w_col, r_col, region_w);

	// Gradiente medido no campo COM sinal (liso na linha). Um gradiente muito
	// maior que o pixel indica o salto de sinal do eixo medial: não é linha.
	// dFdx/dFdy e pixel_along são chamados SEMPRE (fora de qualquer branch) e o
	// resultado é selecionado depois, para não quebrar os derivativos.
	float px_max = max(length(dx), length(dy));
	float px_coast = pixel_along(coast, px_floor);
	float px_border = pixel_along(border_signed, px_floor);
	float px_province = pixel_along(province_signed, px_floor);
	float border_real = step(px_border, px_max * 2.5);
	float province_real = step(px_province, px_max * 2.5);

	// --- Território dinâmico (região e mundo) -------------------------------
	// O bloco é calculado DUAS vezes (uma por camada). As amostras dependem só
	// de `dynamic_territory` (uniforme) e nunca quebram os derivativos; a
	// seleção por region_w acontece depois.
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

	// Cor da nação passa a ser a cor do dono (substitui region/world color).
	if (dynamic_territory) {
		nation = dyn_nation;
	}
	nation = mix(vec3(dot(nation, LUMA)), nation, nation_saturation);

	// --- Terra ---------------------------------------------------------------
	// O satélite tem ~4,9 km/texel: perto da câmera ele só dá manchas. Usa-se
	// o tom regional (relevo, florestas, desertos) e o detalhe fino vem de um
	// grão de papel procedural, que não borra no zoom.
	vec3 sat = texture(terrain_texture, UV, 1.0).rgb;
	vec3 sat_blur = texture(terrain_texture, UV, 4.5).rgb;
	float lum = sqrt(dot(sat, LUMA));
	float lum_blur = sqrt(dot(sat_blur, LUMA));
	float shade = 0.95 + (lum - lum_blur) * 1.05 * terrain_strength
		+ (lum_blur - 0.45) * 0.20 * terrain_strength;
	shade = clamp(shade, 0.78, 1.08);

	float fibers = value_noise(plane * vec2(60.0, 14.0)) * 0.5 + value_noise(plane * 140.0) * 0.5;
	float blotch = value_noise(plane * 2.2);
	shade *= 0.985 + (fibers - 0.5) * 0.035 + (blotch - 0.5) * 0.05;

	vec3 land = nation * shade;

	// Fronteira de país dinâmica: a divisa entre donos diferentes (distância
	// |b| da camada em uso); onde os donos são iguais não há fronteira. Fora
	// da região a fronteira passa a ser a dinâmica do mundo (não o canal G).
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

	// Faixa de cor junto à fronteira (assinatura de carta de grand strategy).
	// Com território dinâmico a faixa precisa caber dentro de uma província
	// (a distância vem da divisa de província mais próxima).
	float band_width = (dynamic_territory && region_w > 0.5) ? dynamic_band_width : border_band_width;
	float band = 1.0 - smoothstep(0.0, band_width, eff_border);
	land *= mix(1.0, 1.0 - border_band_strength, band * band);
	// Leve escurecimento na orla costeira para a terra "assentar".
	land *= mix(1.0, 0.90, 1.0 - smoothstep(0.0, 0.35, coast));

	// Divisas de província: finas, somem ao afastar a câmera. Só na região —
	// fora dela a divisa admin-1 fica poluída em 9,8 km/texel e não é desenhada.
	// Com território dinâmico, só onde os donos são iguais (senão duplica a
	// fronteira grossa).
	float province_fade = 1.0 - smoothstep(0.05, 0.13, px);
	float province_line_dist = mix(1e3, abs(province_r), region_w);
	float province_line = ink_line(province_line_dist, px_province, province_width_px)
		* province_opacity * province_fade * province_real;
	if (dynamic_territory) {
		province_line *= 1.0 - differ;
	}
	land = mix(land, land * 0.55, province_line);

	// --- Mar -----------------------------------------------------------------
	float depth = max(-coast, 0.0);
	vec3 sea = mix(sea_shallow, sea_deep, smoothstep(0.0, shallow_width, depth));
	// Linhas de nível paralelas à costa, como nas cartas náuticas.
	float contour_fade = 1.0 - smoothstep(0.06, 0.16, px);
	float contours = 0.0;
	contours = max(contours, ink_line(abs(depth - 0.40), px_coast, 1.0) * 0.20);
	contours = max(contours, ink_line(abs(depth - 1.00), px_coast, 1.0) * 0.11);
	contours *= contour_fade * step(0.001, depth);
	sea = mix(sea, sea_contour, contours);
	sea *= 0.985 + value_noise(plane * 6.0) * 0.03;

	// --- Composição ----------------------------------------------------------
	float land_mask = smoothstep(-0.5, 0.5, coast / px_coast);
	vec3 color = mix(sea, land, land_mask);

	float coast_line = ink_line(abs(coast), px_coast, coast_width_px);
	color = mix(color, coast_ink, coast_line * 0.85);

	float country_px = max(country_width_px, country_width_world / eff_border_px);
	float country_line = ink_line(eff_border, eff_border_px, country_px) * land_mask * eff_border_real;
	color = mix(color, ink, country_line * 0.92);

	// --- Vinheta polar -------------------------------------------------------
	float pole = min(UV.y, 1.0 - UV.y);
	color = mix(vignette_color, color, smoothstep(0.004, vignette_start, pole));

	ALBEDO = clamp(color, 0.0, 1.0);
}
