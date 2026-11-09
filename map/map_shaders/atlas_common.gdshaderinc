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
//   B = divisa admin-1     (raw*255-128)/8 * texel  (só na região)
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
float ink_line(float dist, float px, float width_px) {
	float d_px = dist / px;
	return 1.0 - smoothstep(width_px * 0.5 - 0.6, width_px * 0.5 + 0.6, d_px);
}
