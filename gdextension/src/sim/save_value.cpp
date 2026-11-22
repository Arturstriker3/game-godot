// Implementação do tipo de valor JSON puro-C++ (save_value.h): containers
// ordenados, conversões e serialização. O nlohmann fica confinado AQUI — nem o
// header nem os outros TUs do núcleo enxergam a biblioteca.
#include "save_value.h"

#include "../../third_party/nlohmann/json.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace ogs {
namespace save {

namespace {

using Json = nlohmann::ordered_json;

// NIL compartilhado devolvido pelas leituras fora dos limites/chave ausente.
const Value& nil_value() {
	static const Value nil;
	return nil;
}

// Slot descartável do `operator[](int)` com índice negativo: limpo a cada
// acesso, para uma escrita indevida não contaminar leituras futuras.
Value& discarded_slot() {
	static Value slot;
	slot = Value();
	return slot;
}

// Token numérico sem ponto e sem expoente é INTEIRO. Se ele chega ao nlohmann
// como float, o strtoll estourou o int64 (ver lexer: "fall back to floats") —
// o save não aceita inteiro fora do int64, e não inventamos um FLOAT.
bool looks_like_integer_token(const std::string& raw) {
	return raw.find('.') == std::string::npos
		&& raw.find('e') == std::string::npos
		&& raw.find('E') == std::string::npos;
}

} // namespace

// ---------------------------------------------------------------------------
// String
// ---------------------------------------------------------------------------

// Só STRING tem texto; os demais tipos viram string vazia (ausência explícita).
String::String(const Value& value) : std::string(static_cast<std::string>(value)) {}

// ---------------------------------------------------------------------------
// Value — construção
// ---------------------------------------------------------------------------

Value::Value(bool value) : type_(BOOL), bool_value_(value) {}

Value::Value(int value) : type_(INT), int_value_(static_cast<long long>(value)) {}

Value::Value(long long value) : type_(INT), int_value_(value) {}

Value::Value(double value) : type_(FLOAT), float_value_(value) {}

Value::Value(const char* value)
	: type_(STRING), string_value_(value != nullptr ? value : "") {}

Value::Value(const std::string& value) : type_(STRING), string_value_(value) {}

Value::Value(const String& value)
	: type_(STRING), string_value_(static_cast<const std::string&>(value)) {}

Value::Value(std::initializer_list<Value> items) : type_(ARRAY) {
	array_items_.assign(items.begin(), items.end());
}

// ---------------------------------------------------------------------------
// Value — acesso a objeto
// ---------------------------------------------------------------------------

bool Value::has(const std::string& key) const {
	if (type_ != DICTIONARY) {
		return false;
	}
	for (const auto& entry : object_entries_) {
		if (entry.first == key) {
			return true;
		}
	}
	return false;
}

const Value& Value::get(const std::string& key, const Value& fallback) const {
	if (type_ == DICTIONARY) {
		for (const auto& entry : object_entries_) {
			if (entry.first == key) {
				return entry.second;
			}
		}
	}
	return fallback;
}

Value& Value::operator[](const std::string& key) {
	if (type_ != DICTIONARY) {
		reset_to(DICTIONARY);
	}
	for (auto& entry : object_entries_) {
		if (entry.first == key) {
			return entry.second;
		}
	}
	// Chave nova entra no FIM: a ordem de inserção é o contrato do save.
	object_entries_.emplace_back(key, Value());
	return object_entries_.back().second;
}

const Value& Value::operator[](const std::string& key) const {
	if (type_ == DICTIONARY) {
		for (const auto& entry : object_entries_) {
			if (entry.first == key) {
				return entry.second;
			}
		}
	}
	return nil_value();
}

// ---------------------------------------------------------------------------
// Value — acesso a array
// ---------------------------------------------------------------------------

Value& Value::operator[](int index) {
	if (type_ != ARRAY) {
		reset_to(ARRAY);
	}
	if (index < 0) {
		return discarded_slot();
	}
	if (index >= static_cast<int>(array_items_.size())) {
		array_items_.resize(static_cast<std::size_t>(index) + 1);
	}
	return array_items_[static_cast<std::size_t>(index)];
}

const Value& Value::operator[](int index) const {
	if (type_ != ARRAY || index < 0
			|| index >= static_cast<int>(array_items_.size())) {
		return nil_value();
	}
	return array_items_[static_cast<std::size_t>(index)];
}

void Value::append(const Value& value) {
	if (type_ != ARRAY) {
		reset_to(ARRAY);
	}
	array_items_.push_back(value);
}

void Value::resize(int size) {
	if (type_ != ARRAY) {
		reset_to(ARRAY);
	}
	if (size < 0) {
		size = 0;
	}
	array_items_.resize(static_cast<std::size_t>(size));
}

// ---------------------------------------------------------------------------
// Value — tamanho e vazio
// ---------------------------------------------------------------------------

int Value::size() const {
	switch (type_) {
	case STRING:
		return static_cast<int>(string_value_.size());
	case ARRAY:
		return static_cast<int>(array_items_.size());
	case DICTIONARY:
		return static_cast<int>(object_entries_.size());
	default:
		return 0;
	}
}

bool Value::is_empty() const {
	switch (type_) {
	case NIL:
		return true;
	case STRING:
	case ARRAY:
	case DICTIONARY:
		return size() == 0;
	default:
		// Escalar (bool/int/float) não é "vazio": existe e vale algo.
		return false;
	}
}

// ---------------------------------------------------------------------------
// Value — conversões explícitas
// ---------------------------------------------------------------------------

Value::operator int() const {
	switch (type_) {
	case BOOL:
		return bool_value_ ? 1 : 0;
	case INT:
		return static_cast<int>(int_value_);
	case FLOAT:
		return static_cast<int>(float_value_);
	default:
		return 0;
	}
}

Value::operator long long() const {
	switch (type_) {
	case BOOL:
		return bool_value_ ? 1 : 0;
	case INT:
		return int_value_;
	case FLOAT:
		return static_cast<long long>(float_value_);
	default:
		return 0;
	}
}

Value::operator double() const {
	switch (type_) {
	case BOOL:
		return bool_value_ ? 1.0 : 0.0;
	case INT:
		return static_cast<double>(int_value_);
	case FLOAT:
		return float_value_;
	default:
		return 0.0;
	}
}

Value::operator bool() const {
	switch (type_) {
	case NIL:
		return false;
	case BOOL:
		return bool_value_;
	case INT:
		return int_value_ != 0;
	case FLOAT:
		return float_value_ != 0.0;
	case STRING:
		return !string_value_.empty();
	case ARRAY:
		return !array_items_.empty();
	case DICTIONARY:
		return !object_entries_.empty();
	}
	return false;
}

Value::operator String() const {
	return type_ == STRING ? String(string_value_) : String();
}

Value::operator std::string() const {
	return type_ == STRING ? string_value_ : std::string();
}

// ---------------------------------------------------------------------------
// Value — igualdade
// ---------------------------------------------------------------------------

bool Value::operator==(const Value& other) const {
	if (type_ != other.type_) {
		return false;
	}
	switch (type_) {
	case NIL:
		return true;
	case BOOL:
		return bool_value_ == other.bool_value_;
	case INT:
		return int_value_ == other.int_value_;
	case FLOAT:
		return float_value_ == other.float_value_;
	case STRING:
		return string_value_ == other.string_value_;
	case ARRAY:
		return array_items_ == other.array_items_;
	case DICTIONARY:
		return object_entries_ == other.object_entries_;
	}
	return false;
}

bool Value::operator==(const char* other) const {
	return type_ == STRING
		&& string_value_ == (other != nullptr ? other : "");
}

// ---------------------------------------------------------------------------
// Value — interno
// ---------------------------------------------------------------------------

void Value::reset_to(Type type) {
	bool_value_ = false;
	int_value_ = 0;
	float_value_ = 0.0;
	string_value_.clear();
	array_items_.clear();
	object_entries_.clear();
	type_ = type;
}

namespace detail {

// Value -> ordered_json. A ordem do objeto é copiada como está (é ela que sai
// no dump). Valores não finitos (NaN/Inf) não têm representação em JSON: saem
// como `null` — o contrato do save exige finitos (safe_nonneg/safe_unit).
struct JsonSerializer {
	static Json to_json(const Value& node) {
		switch (node.type_) {
		case Value::NIL:
			return Json(nullptr);
		case Value::BOOL:
			return Json(node.bool_value_);
		case Value::INT:
			return Json(node.int_value_);
		case Value::FLOAT:
			return Json(node.float_value_);
		case Value::STRING:
			return Json(node.string_value_);
		case Value::ARRAY: {
			Json out = Json::array();
			for (const Value& item : node.array_items_) {
				out.push_back(to_json(item));
			}
			return out;
		}
		case Value::DICTIONARY: {
			Json out = Json::object();
			for (const auto& entry : node.object_entries_) {
				out[entry.first] = to_json(entry.second);
			}
			return out;
		}
		}
		return Json(nullptr);
	}
};

// Construtor SAX: monta o Value direto do texto, o que dá acesso ao token
// numérico bruto (o DOM do nlohmann converte inteiro estourado em float sem
// avisar). Pilha de quadros, sem ponteiros para dentro de Value: qualquer
// realocação de std::vector deixaria o ponteiro pendurado.
class JsonSaxBuilder final : public nlohmann::json_sax<Json> {
public:
	bool null() override { return push(Value()); }

	bool boolean(bool val) override { return push(Value(val)); }

	bool number_integer(number_integer_t val) override {
		return push(Value(static_cast<long long>(val)));
	}

	bool number_unsigned(number_unsigned_t val) override {
		if (val > static_cast<number_unsigned_t>(
				std::numeric_limits<long long>::max())) {
			return fail("inteiro fora do int64");
		}
		return push(Value(static_cast<long long>(val)));
	}

	bool number_float(number_float_t val, const string_t& raw) override {
		if (!std::isfinite(val)) {
			return fail("número não finito (NaN/Inf)");
		}
		if (looks_like_integer_token(raw)) {
			return fail("inteiro fora do int64: " + raw);
		}
		return push(Value(static_cast<double>(val)));
	}

	bool string(string_t& val) override { return push(Value(std::string(val))); }

	bool binary(binary_t&) override {
		return fail("dado binário não faz parte do save JSON");
	}

	bool start_object(std::size_t) override {
		Frame frame;
		frame.is_object = true;
		// Objeto vazio precisa SAIR como objeto ({}), não como null: marca o
		// tipo já na abertura.
		frame.container.reset_to(Value::DICTIONARY);
		stack_.push_back(std::move(frame));
		return true;
	}

	bool key(string_t& val) override {
		if (stack_.empty() || !stack_.back().is_object) {
			return fail("chave fora de um objeto");
		}
		stack_.back().key = val;
		return true;
	}

	bool end_object() override { return close_container(true); }

	bool start_array(std::size_t) override {
		Frame frame;
		frame.is_object = false;
		frame.container.reset_to(Value::ARRAY);
		stack_.push_back(std::move(frame));
		return true;
	}

	bool end_array() override { return close_container(false); }

	bool parse_error(std::size_t position, const std::string& last_token,
			const nlohmann::detail::exception& ex) override {
		std::ostringstream message;
		message << ex.what() << " (byte " << position << ", token '"
			<< last_token << "')";
		return fail(message.str());
	}

	const std::string& error() const { return error_; }

	Value take_root() { return std::move(root_); }

private:
	struct Frame {
		Value container;
		std::string key;
		bool is_object = false;
	};

	bool push(Value&& value) {
		if (stack_.empty()) {
			root_ = std::move(value);
			return true;
		}
		Frame& parent = stack_.back();
		if (parent.is_object) {
			// operator[] insere a chave no fim (ordem do arquivo preservada).
			parent.container[parent.key] = std::move(value);
			parent.key.clear();
		} else {
			parent.container.append(value);
		}
		return true;
	}

	bool close_container(bool is_object) {
		if (stack_.empty() || stack_.back().is_object != is_object) {
			return fail("estrutura JSON inconsistente");
		}
		Frame frame = std::move(stack_.back());
		stack_.pop_back();
		return push(std::move(frame.container));
	}

	bool fail(std::string message) {
		if (error_.empty()) {
			error_ = std::move(message);
		}
		return false;
	}

	Value root_;
	std::vector<Frame> stack_;
	std::string error_;
};

} // namespace detail

// ---------------------------------------------------------------------------
// Serialização
// ---------------------------------------------------------------------------

std::string dump_json(const Value& value, int indent) {
	const Json root = detail::JsonSerializer::to_json(value);
	// UTF-8 inválido vira U+FFFD em vez de exceção: o save nunca falha por um
	// byte corrompido, e a representação continua determinística.
	const std::string text = root.dump(indent < 0 ? -1 : indent, ' ', false,
		Json::error_handler_t::replace);
	return text + "\n";
}

bool parse_json_text(const std::string& text, Value& out, std::string& error) {
	error.clear();
	detail::JsonSaxBuilder builder;
	if (!Json::sax_parse(text.begin(), text.end(), &builder)) {
		error = builder.error();
		if (error.empty()) {
			error = "JSON inválido";
		}
		return false;
	}
	out = builder.take_root();
	return true;
}

bool parse_json_file(const std::string& path, Value& out, std::string& error) {
	error.clear();
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		error = "não foi possível abrir o arquivo: " + path;
		return false;
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	if (file.bad()) {
		error = "falha de leitura do arquivo: " + path;
		return false;
	}
	return parse_json_text(buffer.str(), out, error);
}

} // namespace save
} // namespace ogs
