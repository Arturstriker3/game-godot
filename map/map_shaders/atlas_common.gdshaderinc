// Funções comuns do atlas político: decodificação dos campos de distância
// assinados, mistura região/mundo, medida de pixel ao longo do gradiente e
// linhas de tinta anti-aliasadas.
//
// Usado por:
//   - atlas_political.gdshader      (passada opaca do modo político)
//   - political_overlay.gdshader    (camada transparente dos modos de calor)
//
// As texturas têm canais RGB8 codificados como distâncias assinadas:
//   R = costa       (raw*255-128)/2 * texel   (>0 é terra)
//   G = fronteira de país  (raw*255-128)/8 * texel  (linha no zero)
//   B = divisa admin-1     (raw*255-128)/8 * texel  (linha no zero; região E mundo)
//   A = água interior      (raw*255-128)/8 * texel  (rio/lago no zero;
//       dentro de lago o valor leva +LAKE_BIAS texels)
// O canal B do raster de província (não do SDF) é a distância às fronteiras
// disputadas, na mesma codificação de G.
// `texel` é o tamanho de um texel em unidades do plano (plano = 1000×500).

// Decodifica a distância assinada à costa (canal R do SDF).
float atlas_coast_distance(float raw, float texel) {
	return (raw * 255.0 - 128.0) / 2.0 * texel;
}

// Decodifica a distância ASSINADA à fronteira de país (canal G do SDF).
// A linha está no cruzamento por zero.
float atlas_border_signed(float raw, float texel) {
	return (raw * 255.0 - 128.0) / 8.0 * texel;
}

// Decodifica a distância ASSINADA à divisa admin-1 (canal B do SDF).
float atlas_province_signed(float raw, float texel) {
	return (raw * 255.0 - 128.0) / 8.0 * texel;
}

// Decodifica a distância ASSINADA à água interior (canal A do SDF): costa de
// rio/lago no cruzamento por zero. Dentro de um lago o gerador soma `LAKE_BIAS`
// texels, então `valor - LAKE_BIAS` é a distância à margem do lago e o valor
// bruto separa lago (>= LAKE_BIAS) de rio (linha fina).
float atlas_water_signed(float raw, float texel) {
	return (raw * 255.0 - 128.0) / 8.0 * texel;
}

// Decodifica a distância ASSINADA às fronteiras disputadas (canal B do raster
// de província; mesma codificação do canal G do SDF).
float atlas_disputed_signed(float raw, float texel) {
	return (raw * 255.0 - 128.0) / 8.0 * texel;
}

// UV da região (Europa) dentro da textura regional, a partir do UV global
// equirretangular. `region_bounds` = (lon0, lat0, lon1, lat1) em graus.
vec2 atlas_region_uv(vec2 uv, vec4 region_bounds) {
	float lon = uv.x * 360.0 - 180.0;
	float lat = 90.0 - uv.y * 180.0;
	return vec2(
		(lon - region_bounds.x) / (region_bounds.z - region_bounds.x),
		(region_bounds.w - lat) / (region_bounds.w - region_bounds.y));
}

// Peso de mistura região/mundo (0 = mundo, 1 = região) na borda da região.
float atlas_region_weight(vec2 ruv) {
	vec2 edge = min(ruv, 1.0 - ruv);
	return smoothstep(0.0, 0.04, min(edge.x, edge.y * 1.8));
}

// Unidades do plano por pixel de tela NA DIREÇÃO do gradiente do campo. Com a
// câmera inclinada o pixel cobre mais chão na vertical do que na horizontal;
// medir ao longo do gradiente deixa linhas de qualquer orientação com a mesma
// espessura. `floor_px` evita furos onde o campo sem sinal tem o "vinco" (d≈0).
float pixel_along(float field, float floor_px) {
	return max(length(vec2(dFdx(field), dFdy(field))), floor_px);
}

// Linha anti-aliasada: `dist` e `px` em unidades do plano, largura em pixels.
// `soft_px` é a meia-largura da transição em pixels de tela (0,6 no padrão do
// atlas): valores maiores esfumam a linha e escondem a quantização de campos
// de distância de baixa resolução, sem mudar a largura nominal.
float ink_line_soft(float dist, float px, float width_px, float soft_px) {
	float d_px = dist / px;
	return 1.0 - smoothstep(width_px * 0.5 - soft_px,
			width_px * 0.5 + soft_px, d_px);
}

float ink_line(float dist, float px, float width_px) {
	return ink_line_soft(dist, px, width_px, 0.6);
}

// Decodifica o id da província no raster `region_province`: id = R + G*256.
// Amostra com `filter_nearest` para não misturar ids de províncias vizinhas.
float atlas_province_id(sampler2D region_province, vec2 uv) {
	vec3 c = texture(region_province, uv).rgb;
	return round(c.r * 255.0 + c.g * 255.0 * 256.0);
}

// Busca a cor e o índice do dono da província `id` na textura de donos.
// rgb = cor do dono, a * 255 = índice do dono. Texel (id % cols, id / cols).
// Id ausente/fora da textura devolve alfa 0 ("sem dono"). Sem esta guarda o
// id 0 caía no texel (0,0) — que nunca é escrito — e a ilha removida (id que
// saiu de provinces.json) pintava preto em vez de mar.
vec4 atlas_owner(sampler2D province_owner, int owner_columns, float id) {
	int iid = int(round(id));
	ivec2 texels = textureSize(province_owner, 0);
	if (iid <= 0 || iid >= texels.x * texels.y) {
		return vec4(0.0);
	}
	ivec2 px = ivec2(iid % owner_columns, iid / owner_columns);
	return texelFetch(province_owner, px, 0);
}
