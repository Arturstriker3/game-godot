#include "value_bridge.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <cstdint>
#include <string>

namespace ogs {
namespace edge {
namespace {

using save::Value;

// Bytes UTF-8 de um String do Godot. NUNCA Latin-1: os nomes do mundo têm
// acento e o round-trip save/load não pode corrompê-los.
std::string utf8_bytes(const godot::String& text) {
	const godot::CharString bytes = text.utf8();
	const char* data = bytes.get_data();
	return data != nullptr ? std::string(data) : std::string();
}

// String do Godot a partir de bytes UTF-8 (o construtor const char* lê Latin-1).
godot::String godot_text(const std::string& text) {
	return godot::String::utf8(text.c_str());
}

godot::Variant value_to_godot(const Value& value) {
	switch (value.get_type()) {
	case Value::BOOL:
		return godot::Variant(static_cast<bool>(value));
	case Value::INT:
		return godot::Variant(static_cast<int64_t>(
			static_cast<long long>(value)));
	case Value::FLOAT:
		return godot::Variant(static_cast<double>(value));
	case Value::STRING: {
		const std::string text = static_cast<std::string>(value);
		return godot::Variant(godot_text(text));
	}
	case Value::ARRAY: {
		godot::Array items;
		const int count = value.size();
		for (int index = 0; index < count; ++index) {
			items.push_back(value_to_godot(value[index]));
		}
		return godot::Variant(items);
	}
	case Value::DICTIONARY: {
		godot::Dictionary object;
		// Pares na ordem de inserção do Value: o Dictionary do Godot preserva
		// a ordem em que as chaves entram, e o hash do save depende dela.
		for (const auto& entry : value.entries()) {
			object[godot::Variant(godot_text(entry.first))] =
				value_to_godot(entry.second);
		}
		return godot::Variant(object);
	}
	case Value::NIL:
	default:
		// NIL e qualquer tipo fora do contrato: ausência explícita.
		return godot::Variant();
	}
}

Value godot_to_value(const godot::Variant& value) {
	switch (value.get_type()) {
	case godot::Variant::BOOL:
		return Value(static_cast<bool>(value));
	case godot::Variant::INT:
		return Value(static_cast<long long>(static_cast<int64_t>(value)));
	case godot::Variant::FLOAT:
		return Value(static_cast<double>(value));
	case godot::Variant::STRING:
		return Value(save::String(
			utf8_bytes(static_cast<godot::String>(value))));
	case godot::Variant::STRING_NAME:
		return Value(save::String(utf8_bytes(
			godot::String(static_cast<godot::StringName>(value)))));
	case godot::Variant::ARRAY: {
		const godot::Array items = static_cast<godot::Array>(value);
		Value out = Value::make_array();
		const int64_t count = items.size();
		for (int64_t index = 0; index < count; ++index) {
			out.append(godot_to_value(items[index]));
		}
		return out;
	}
	case godot::Variant::DICTIONARY: {
		const godot::Dictionary object = static_cast<godot::Dictionary>(value);
		Value out = Value::make_object();
		// `keys()` devolve na ordem de inserção do Dictionary; o save é
		// serializado exatamente nessa ordem.
		const godot::Array keys = object.keys();
		const int64_t count = keys.size();
		for (int64_t index = 0; index < count; ++index) {
			const godot::Variant key = keys[index];
			std::string key_text;
			if (key.get_type() == godot::Variant::STRING_NAME) {
				key_text = utf8_bytes(
					godot::String(static_cast<godot::StringName>(key)));
			} else {
				// O save só tem chaves STRING; um Variant de chave de outro
				// tipo é convertido pelo próprio Godot (mesmo texto do
				// GDScript), sem descartar a entrada silenciosamente.
				key_text = utf8_bytes(static_cast<godot::String>(key));
			}
			out[key_text] = godot_to_value(object[key]);
		}
		return out;
	}
	case godot::Variant::NIL:
	default:
		return Value();
	}
}

} // namespace

godot::Variant to_godot(const save::Value& value) {
	return value_to_godot(value);
}

save::Value from_godot(const godot::Variant& value) {
	return godot_to_value(value);
}

} // namespace edge
} // namespace ogs
