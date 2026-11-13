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
// (~4,9 km/texel), misturadas na borda da região. As divisas admin-1 valem
// nas duas camadas (canal B) e usam a mesma mistura, sem costura na caixa.
//
// Composição: terreno (luminância do satélite) × cor da nação, faixa de cor
// junto à fronteira, divisas de província, água interior (rios e lagos),
// mar com batimetria de costa e linhas de nível, linha de costa e fronteiras
// em tinta. Fronteiras disputadas usam tracejado de tela; a nação apontada
// por `highlight_nation` ganha preenchimento mais claro e contorno em latão.

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
uniform int highlight_owner = -1;
uniform float dynamic_band_width = 0.3;
// Destaque de NAÇÃO (hover do nome/bandeira, API TerritoryMap): o território
// inteiro do dono clareia `highlight_fill` e a divisa com os vizinhos (e a
// costa) ganha um contorno de latão de `highlight_border_px`.
uniform int highlight_nation = -1;
uniform vec3 highlight_color : source_color = vec3(0.788, 0.635, 0.290);
uniform float highlight_border_px = 2.0;
uniform float highlight_fill : hint_range(0.0, 0.5) = 0.08;

// Janela da região em graus (lon0, lat0, lon1, lat1) — igual ao gerador.
uniform vec4 region_bounds = vec4(-25.0, 33.0, 45.0, 72.0);
// Tamanho de um texel em unidades do plano (plano = 1000 × 500 unidades).
uniform float region_texel = 0.047471;
uniform float world_texel = 0.122070;  // 1000 / 8192 (mundo 8192 px, ~4,9 km/texel)

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

// Água interior (Natural Earth 10m: rios e lagos). `lake_bias` é o viés de
// interior gravado no canal A do SDF (LAKE_BIAS em tools/build_atlas_layers.py).
uniform float lake_bias = 4.0;
uniform vec3 river_color : source_color = vec3(0.137, 0.282, 0.373);
uniform float river_fade_near = 0.045;
uniform float river_fade_far = 0.130;
uniform float river_big_scale = 2.2;
// Largura MÍNIMA do rio em pixels de tela: o traço rasterizado é de 1–2 texels
// e some quando o texel fica menor que o pixel; este piso mantém o rio legível
// no zoom continental. Perto, o traço do raster é mais largo e domina (o rio
// engrossa ao aproximar).
uniform float river_min_width_px = 1.2;
uniform float lake_fade_near = 0.14;
uniform float lake_fade_far = 0.30;

// Fronteiras disputadas: tracejado em ESPAÇO DE TELA (6 px de traço, 4 de
// vão), mesma tinta e mesma espessura da fronteira comum.
uniform float dash_on_px = 6.0;
uniform float dash_period_px = 10.0;

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
// de id de província, e devolve o dono do lado certo da divisa, se os dois
// lados têm donos diferentes e se este pixel tem dono válido. Igual para a
// região e para o mundo.
// `out_nation_fill` = pixel pertence à nação destacada; `out_nation_edge` =
// pixel está na divisa entre a nação destacada e outro dono (contorno).
void dyn_block(sampler2D sdf_tex, sampler2D prov_tex, vec2 uv,
		out float out_differ, out vec3 out_nation, out float out_highlight,
		out float out_no_owner, out float out_nation_fill,
		out float out_nation_edge) {
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
	float index_here = round(owner_here.a * 255.0);
	float index_other = round(owner_other.a * 255.0);
	// Sem id de província ou sem dono publicado = pixel sem território. O
	// fragmento usa isto para descartar a máscara de terra: uma ilha removida
	// do atlas nunca mais pinta preto, mesmo com raster/importação defasada.
	float has_here = (id_here > 0.5 && index_here > 0.0) ? 1.0 : 0.0;
	float has_other = (id_other > 0.5 && index_other > 0.0) ? 1.0 : 0.0;
	float differ = (has_here > 0.5 && has_other > 0.5
			&& index_here != index_other) ? 1.0 : 0.0;
	out_differ = differ * reliable;
	out_nation = (has_here > 0.5) ? owner_here.rgb : vec3(0.0);
	out_highlight = (has_here > 0.5
			&& index_here == float(highlight_owner)) ? 1.0 : 0.0;
	out_no_owner = 1.0 - has_here;
	// Destaque de nação: preenchimento em todo o território do dono e contorno
	// onde exatamente um dos lados da divisa é dele (o lado do mar fica com a
	// tinta da costa, reforçada no fragmento).
	// `highlight_nation < 0` = sem destaque: nem preenchimento nem contorno.
	float highlight_on = (highlight_nation >= 0) ? 1.0 : 0.0;
	float hi_here = (highlight_on > 0.5 && has_here > 0.5
			&& index_here == float(highlight_nation)) ? 1.0 : 0.0;
	float hi_other = (highlight_on > 0.5 && has_other > 0.5
			&& index_other == float(highlight_nation)) ? 1.0 : 0.0;
	out_nation_fill = hi_here;
	// Contorno onde EXATAMENTE um dos lados é da nação destacada (a divisa com
	// os vizinhos); o lado do mar é resolvido no fragmento, pela costa.
	out_nation_edge = differ * reliable * abs(hi_here - hi_other);
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

	vec4 w_sdf = texture(world_sdf, UV);
	vec3 w_col = texture(world_color, UV).rgb;
	vec4 r_sdf = texture(region_sdf, ruv);
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
	float region_highlight = 0.0;
	float world_highlight = 0.0;
	float region_no_owner = 0.0;
	float world_no_owner = 0.0;
	float region_nation_fill = 0.0;
	float world_nation_fill = 0.0;
	float region_nation_edge = 0.0;
	float world_nation_edge = 0.0;
	vec3 region_dyn_nation = nation;
	vec3 world_dyn_nation = nation;
	if (dynamic_territory) {
		dyn_block(region_sdf, region_province, ruv, region_differ, region_dyn_nation,
				region_highlight, region_no_owner, region_nation_fill,
				region_nation_edge);
		dyn_block(world_sdf, world_province, UV, world_differ, world_dyn_nation,
				world_highlight, world_no_owner, world_nation_fill,
				world_nation_edge);
	}
	vec3 dyn_nation = (region_w > 0.5) ? region_dyn_nation : world_dyn_nation;
	float highlight = (region_w > 0.5) ? region_highlight : world_highlight;
	float nation_fill = (region_w > 0.5) ? region_nation_fill : world_nation_fill;
	float nation_edge = (region_w > 0.5) ? region_nation_edge : world_nation_edge;
	// Pixel sem província/dono válido não é terra: vira mar (era o preto das
	// ilhas removidas do Egeu quando o raster ainda trazia o id antigo).
	float no_owner = dynamic_territory
			? ((region_w > 0.5) ? region_no_owner : world_no_owner) : 0.0;
	float has_territory = 1.0 - no_owner;

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

	// Divisas de província: finas, somem ao afastar a câmera. Valem no mundo
	// inteiro: dentro da caixa da Europa vêm da camada regional (alta
	// resolução) e fora dela do SDF mundial (4,9 km/texel). A distância é a
	// mesma mistura de `region_w` usada no resto do shader, então a borda da
	// caixa não tem costura; só o halo muda — fora da região ele é mais largo
	// para esconder a quantização do texel mundial, que no zoom de país chega
	// a ~1,5 texel por pixel de tela.
	// O fade por zoom some no nível continental: a divisa aparece com o país
	// inteiro na tela (px ≈ 0,09) e some antes do LOD longe (px ≈ 0,19).
	// Com território dinâmico, só onde os donos são iguais (senão duplica a
	// fronteira grossa).
	float province_fade = 1.0 - smoothstep(0.055, 0.20, px);
	float province_soft = mix(1.0, 0.6, region_w);
	float province_line = ink_line_soft(abs(province_signed), px_province,
			province_width_px, province_soft)
		* province_opacity * province_fade * province_real;
	if (dynamic_territory) {
		float same_owner = mix(1.0 - world_differ, 1.0 - region_differ, region_w);
		province_line *= same_owner;
	} else {
		// Sem dono publicado não há como distinguir divisa de fronteira
		// nacional: fora da região não desenha (evita duplicar a grossa).
		province_line *= region_w;
	}
	land = mix(land, land * 0.55, province_line);

	// Destaque do território da nação selecionada (seleção de cidade): clareia
	// o preenchimento e aquece um pouco o tom, sem tocar no mar nem nas divisas.
	if (dynamic_territory && highlight > 0.5) {
		land = mix(land, land * 1.12 + vec3(0.035, 0.030, 0.018), 0.55);
	}
	// Destaque de NAÇÃO (hover do nome/bandeira): clareamento leve e uniforme
	// do território inteiro; o contorno de latão entra na composição final.
	land *= 1.0 + highlight_fill * nation_fill;

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
	float land_mask = smoothstep(-0.5, 0.5, coast / px_coast) * has_territory;
	vec3 color = mix(sea, land, land_mask);

	float coast_line = ink_line(abs(coast), px_coast, coast_width_px) * has_territory;
	color = mix(color, coast_ink, coast_line * 0.85);

	// --- Água interior (rios e lagos, canal A do SDF) ------------------------
	// O campo é normalizado em texels da camada em uso: `wtpx` = texels por
	// pixel de tela (a medida de espessura que o resto do shader usa).
	float texel = mix(world_texel, region_texel, region_w);
	float water_tx = mix(atlas_water_signed(w_sdf.a, world_texel) / world_texel,
			atlas_water_signed(r_sdf.a, region_texel) / region_texel, region_w);
	float wtpx = max(px / texel, 1e-4);
	// Lâmina rasa = rio; o interior do lago começa em `lake_bias` texels.
	float shallow = 1.0 - smoothstep(lake_bias * 0.35, lake_bias * 0.7, water_tx);
	float lake_fill = smoothstep(-0.6 * wtpx, 0.6 * wtpx, water_tx - lake_bias)
		* (1.0 - smoothstep(lake_fade_near, lake_fade_far, px));
	// Rio: lâmina rasterizada (1-2 texels) preenchida com a cor da água; longe
	// o traço fica sub-pixel e some, mas os rios largos (scalerank 0-1) ficam
	// ~2x mais tempo no zoom afastado.
	float big_river = smoothstep(0.7, 1.3, water_tx);
	float river_vis = mix(
		1.0 - smoothstep(river_fade_near, river_fade_far, px),
		1.0 - smoothstep(river_fade_far, river_fade_far * river_big_scale, px),
		big_river);
	float river_band = smoothstep(-0.5 * river_min_width_px * wtpx,
			0.5 * river_min_width_px * wtpx, water_tx) * shallow;
	float river_amount = river_band * river_vis * land_mask;
	vec3 lake_color = mix(sea_shallow, sea_deep, 0.35);
	color = mix(color, lake_color, lake_fill);
	// Rio: cor da água plena no núcleo (a banda já dá o anti-aliasing da
	// margem), escurecendo o traço o suficiente para ler sobre o papel claro.
	color = mix(color, river_color, river_amount);
	// Margem: mesma tinta da costa, como o mar, mas fina e translúcida no rio —
	// grosso demais ela cobriria a lâmina de 1 px e o traço leria cinza em vez
	// de água. Só aparece quando a lâmina é maior que um pixel (`edge_gate`).
	float edge_gate = 1.0 - smoothstep(0.8, 1.6, wtpx);
	float water_edge = ink_line(abs(water_tx), wtpx, coast_width_px * 0.55)
		* max(lake_fill, river_amount) * land_mask * edge_gate;
	color = mix(color, coast_ink, water_edge * 0.6);
	// Fronteira entre nações NÃO atravessa lago (o raster de província é de
	// terra; sem isto a linha cortaria Vitória, Baikal, os Grandes Lagos).
	float country_px = max(country_width_px, country_width_world / eff_border_px);
	float country_line = ink_line(eff_border, eff_border_px, country_px)
		* land_mask * eff_border_real * (1.0 - lake_fill);

	// --- Fronteiras disputadas: tracejado de tela ----------------------------
	// O canal B do raster de província marca a divisa entre os pares de
	// DISPUTED_BORDERS (tools/world_nation_curation.py). Ali a linha cheia dá
	// lugar a 6 px de traço / 4 px de vão, mesma tinta e mesma espessura.
	float disputed = abs(mix(
			atlas_disputed_signed(texture(world_province, UV).b, world_texel),
			atlas_disputed_signed(texture(region_province, ruv).b, region_texel),
			region_w));
	float disputed_gate = 1.0 - smoothstep(0.6 * texel, 1.8 * texel, disputed);
	float dash = step(mod(FRAGCOORD.x + FRAGCOORD.y, dash_period_px), dash_on_px);
	float solid = mix(1.0, dash, disputed_gate);
	color = mix(color, ink, country_line * solid * 0.92);

	// Contorno de latão do destaque de nação: divisa com os outros donos e,
	// no lado do mar, a própria costa. Sem destaque (`highlight_nation < 0`) o
	// latão é zero por construção — nenhum resquício dourado na fronteira.
	float highlight_on = (highlight_nation >= 0) ? 1.0 : 0.0;
	float brass = ink_line(eff_border, eff_border_px, highlight_border_px)
		* nation_edge * eff_border_real * land_mask;
	brass = max(brass, ink_line(abs(coast), px_coast, highlight_border_px)
			* nation_fill);
	color = mix(color, highlight_color,
			clamp(brass, 0.0, 1.0) * 0.9 * highlight_on);

	// --- Vinheta polar -------------------------------------------------------
	float pole = min(UV.y, 1.0 - UV.y);
	color = mix(vignette_color, color, smoothstep(0.004, vignette_start, pole));

	ALBEDO = clamp(color, 0.0, 1.0);
}
