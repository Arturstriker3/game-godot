shader_type canvas_item;

// Vinheta sutil de tela: escurece só os cantos/bordas na cor de fundo do
// ambiente, para o mapa "assentar". Fica abaixo do HUD (CanvasLayer 40).

uniform float strength : hint_range(0.0, 1.0) = 0.30;
uniform float inner : hint_range(0.0, 1.5) = 0.62;
uniform float outer : hint_range(0.5, 2.0) = 1.45;
uniform vec3 tint : source_color = vec3(0.0392, 0.0549, 0.0588);

void fragment() {
	vec2 p = UV * 2.0 - 1.0;
	// Elipse um pouco mais larga que alta (tela 16:9).
	float d = length(p * vec2(0.86, 1.0));
	COLOR = vec4(tint, smoothstep(inner, outer, d) * strength);
}
