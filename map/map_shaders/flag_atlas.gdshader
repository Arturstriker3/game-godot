// Bandeiras do mapa num atlas único, desenhadas por MultiMesh (1 draw call).
// `INSTANCE_CUSTOM` escolhe a célula do atlas: xy = escala UV, zw = offset UV.
// `COLOR` (MultiMesh.use_colors) carrega o alfa do fade de nível.
shader_type spatial;
render_mode unshaded, cull_disabled, depth_test_disabled, blend_mix;

uniform sampler2D flags_atlas : source_color, filter_linear, repeat_disable;

varying vec2 atlas_uv;
varying float instance_alpha;

void vertex() {
	// Billboard: a base do quad acompanha a câmera, preservando a escala da
	// instância (MODEL_MATRIX já inclui o transform do MultiMesh).
	mat4 billboard = mat4(
		vec4(INV_VIEW_MATRIX[0].xyz * length(MODEL_MATRIX[0].xyz), 0.0),
		vec4(INV_VIEW_MATRIX[1].xyz * length(MODEL_MATRIX[1].xyz), 0.0),
		vec4(INV_VIEW_MATRIX[2].xyz * length(MODEL_MATRIX[2].xyz), 0.0),
		MODEL_MATRIX[3]);
	MODELVIEW_MATRIX = VIEW_MATRIX * billboard;
	atlas_uv = UV * INSTANCE_CUSTOM.xy + INSTANCE_CUSTOM.zw;
	instance_alpha = COLOR.a;
}

void fragment() {
	vec4 tex = texture(flags_atlas, atlas_uv);
	ALBEDO = tex.rgb;
	ALPHA = tex.a * instance_alpha;
}
