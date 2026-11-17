#pragma once
// Tipo de valor JSON puro-C++ para o save/load da simulação.
//
// Substitui String/Array/Dictionary/Variant do Godot DENTRO da biblioteca de
// simulação, para que `edge/save_load.cpp` compile sem godot-cpp (o objetivo
// do S1/perf: núcleo testável e serializável sem o engine). O contrato é o que
// o save real já usa hoje:
//   * o OBJETO preserva a ORDEM DE INSERÇÃO das chaves — o hash do save do jogo
//     depende dela (por isso o dump passa por nlohmann::ordered_json);
//   * tipos explícitos (NIL/BOOL/INT/FLOAT/STRING/ARRAY/DICTIONARY) para que
//     `int(..)`, `double(..)`, `bool(..)` e `get_type()` se comportem como o
//     Variant nos pontos que o save usa;
//   * nenhum acesso lança: chave/índice ausente devolve NIL.
//
// STL puro neste header. O nlohmann (vendorizado em
// gdextension/third_party/nlohmann) aparece só em save_value.cpp.

#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace ogs {
namespace save {

class Value;

// Serialização (implementadas em save_value.cpp para o nlohmann não vazar
// para os outros TUs). `dump_json` termina com quebra de linha.
std::string dump_json(const Value& value, int indent = 2);
bool parse_json_text(const std::string& text, Value& out, std::string& error);
bool parse_json_file(const std::string& path, Value& out, std::string& error);

namespace detail {
// Acessores internos da serialização (save_value.cpp). Declarados aqui só para
// a amizade: o dump precisa da ordem interna dos objetos e o parse precisa
// marcar contêiner vazio com o tipo certo (a API pública não expõe isso, e não
// deve — o save não usa).
struct JsonSerializer;
class JsonSaxBuilder;
} // namespace detail

// Visão dos bytes UTF-8 de um String: equivalente ao `String.utf8()` do Godot
// no único uso que o save faz (`.get_data()` + std::string).
struct Utf8View {
	const std::string& text;

	const char* get_data() const { return text.c_str(); }
};

// String de bytes UTF-8. Deriva de std::string de propósito: herda os
// operadores e serve direto como chave de objeto.
class String : public std::string {
public:
	String() = default;
	String(const char* text) : std::string(text != nullptr ? text : "") {}
	String(const std::string& text) : std::string(text) {}

	// Converte um Value. Só STRING tem texto; os outros tipos viram string
	// vazia (ausência explícita, nunca um número inventado em texto).
	// Defino depois de Value, em save_value.cpp.
	String(const Value& value);

	// Leitura dos bytes como UTF-8 — mesmo contrato do String::utf8 do Godot
	// (usado pelo save real para não corromper acentos no round-trip).
	static String utf8(const char* text) { return String(text); }

	// Bytes UTF-8 do próprio texto.
	Utf8View utf8() const { return Utf8View{*this}; }

	bool is_empty() const { return empty(); }

	// O String já É um std::string (herança pública), então todo uso de
	// `std::string` já o aceita; a conversão explícita fica no contrato da API
	// (nega o aviso -Wclass-conversion, que é justamente por ser classe-base).
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wclass-conversion"
#endif
	explicit operator std::string() const {
		return std::string(static_cast<const std::string&>(*this));
	}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
};

// Valor JSON com tipo explícito e ordem de inserção preservada no objeto.
class Value {
public:
	enum Type { NIL, BOOL, INT, FLOAT, STRING, ARRAY, DICTIONARY };

	Value() = default;

	Value(bool value);
	Value(int value);
	Value(long long value);
	Value(double value);
	Value(const char* value);
	Value(const std::string& value);
	Value(const String& value);
	// Literal de array: `Array{a, b, c}` (o to_dict usa direto).
	Value(std::initializer_list<Value> items);

	// Contêiner VAZIO do tipo certo. `Dictionary d;` é NIL (tipo indefinido),
	// e um contêiner que fica vazio precisa sair como `{}`/`[]` no JSON — é o
	// que o Dictionary/Array do Godot fazem. O serializador usa estes dois.
	static Value make_object() {
		Value value;
		value.reset_to(DICTIONARY);
		return value;
	}
	static Value make_array() {
		Value value;
		value.reset_to(ARRAY);
		return value;
	}

	Value(const Value&) = default;
	Value(Value&&) noexcept = default;
	Value& operator=(const Value&) = default;
	Value& operator=(Value&&) noexcept = default;
	~Value() = default;

	Type get_type() const { return type_; }

	// Objeto: presença de chave (não recria nem injeta nada).
	bool has(const std::string& key) const;
	// Objeto: valor da chave ou `fallback` se ela não existir.
	const Value& get(const std::string& key, const Value& fallback) const;
	// Objeto: garante a chave no FIM da ordem e devolve o slot para escrita.
	Value& operator[](const std::string& key);
	// Objeto: leitura sem escrita; chave ausente (ou valor não-objeto) devolve
	// um NIL estático — nunca lança.
	const Value& operator[](const std::string& key) const;
	// Objeto: pares (chave, valor) na ORDEM DE INSERÇÃO, só para leitura.
	// Existe para a ponte de valor do adaptador Godot (edge/value_bridge.cpp)
	// converter object -> Dictionary sem perder a ordem (o hash do save depende
	// dela). Não altera o Value: inserção continua passando por operator[].
	const std::vector<std::pair<std::string, Value>>& entries() const {
		return object_entries_;
	}

	// Array: cresce até `index + 1` com NIL quando precisa.
	Value& operator[](int index);
	// Array: leitura; índice fora dos limites devolve um NIL estático.
	const Value& operator[](int index) const;

	void append(const Value& value);
	// Array: ajusta o tamanho (os novos slots são NIL). Escalares viram array
	// vazio antes de crescer — `Array a; a.resize(n)` é o padrão do save.
	void resize(int size);

	// Array/objeto/string; 0 nos escalares.
	int size() const;
	// NIL e contêiner/string vazios são "vazios"; escalares não são.
	bool is_empty() const;

	// Conversões explícitas. NIL -> 0 / 0.0 / false / "". INT<->FLOAT são
	// aceitos; STRING/ARRAY/DICTIONARY não viram número (0) — o save nunca
	// converte esses tipos, e inventar um valor esconderia o erro.
	explicit operator int() const;
	// Inteiro de 64 bits, sem truncar em 2^31: a ponte de valor do adaptador
	// Godot (edge/value_bridge.cpp) converte Variant::INT <-> Value::INT com a
	// mesma largura do Variant.
	explicit operator long long() const;
	explicit operator double() const;
	explicit operator bool() const;
	explicit operator String() const;
	explicit operator std::string() const;

	// Igualdade por tipo + conteúdo (o round-trip tem de bater tipo a tipo).
	bool operator==(const Value& other) const;
	// Atalho para os usos tipo `state == "private"`.
	bool operator==(const char* other) const;

private:
	friend struct detail::JsonSerializer;
	friend class detail::JsonSaxBuilder;

	// Troca o tipo limpando o payload anterior (valor padrão do novo tipo).
	// Chamado só quando o tipo muda, para não perder a ordem do objeto.
	void reset_to(Type type);

	Type type_ = NIL;
	bool bool_value_ = false;
	long long int_value_ = 0;
	double float_value_ = 0.0;
	std::string string_value_;
	std::vector<Value> array_items_;
	// Par (chave, valor) em ordem de inserção: é o que preserva o hash.
	std::vector<std::pair<std::string, Value>> object_entries_;
};

// Os nomes do Godot que o save/load já usa.
using Array = Value;
using Dictionary = Value;
using Variant = Value;

} // namespace save
} // namespace ogs
