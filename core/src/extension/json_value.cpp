#include "json_value.h"

#include "fem/core/extension/error.h"

#include "files.h"

#include <cstdint>
#include <limits>
#include <string>

namespace fem::extension::detail {

nlohmann::json to_json(const command::Value& value) {
    using Kind = command::Value::Kind;
    switch (value.kind()) {
        case Kind::none:
            return nullptr;
        case Kind::boolean:
            return value.as_boolean();
        case Kind::integer:
            return value.as_integer();
        case Kind::number:
            return value.as_number();
        case Kind::string:
            return value.as_string();
        case Kind::path:
            return display(value.as_path());
        case Kind::list: {
            nlohmann::json list = nlohmann::json::array();
            for (const command::Value& item : value.as_list()) {
                list.push_back(to_json(item));
            }
            return list;
        }
        case Kind::dict: {
            nlohmann::json dict = nlohmann::json::object();
            for (const auto& [key, item] : value.as_dict()) {
                dict[key] = to_json(item);
            }
            return dict;
        }
    }
    return nullptr;
}

command::Value from_json(const nlohmann::json& json) {
    switch (json.type()) {
        case nlohmann::json::value_t::null:
            return {};
        case nlohmann::json::value_t::boolean:
            return json.get<bool>();
        case nlohmann::json::value_t::number_integer:
            return json.get<std::int64_t>();
        case nlohmann::json::value_t::number_unsigned: {
            const auto value = json.get<std::uint64_t>();
            if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                throw ExtensionError(ExtensionErrc::command_failed,
                                     "the integer " + std::to_string(value) + " is too large");
            }
            return static_cast<std::int64_t>(value);
        }
        case nlohmann::json::value_t::number_float:
            return json.get<double>();
        case nlohmann::json::value_t::string:
            return json.get<std::string>();
        case nlohmann::json::value_t::array: {
            command::Value::List list;
            for (const nlohmann::json& item : json) {
                list.push_back(from_json(item));
            }
            return list;
        }
        case nlohmann::json::value_t::object: {
            command::Value::Dict dict;
            for (const auto& [key, item] : json.items()) {
                dict.emplace(key, from_json(item));
            }
            return dict;
        }
        case nlohmann::json::value_t::binary:
        case nlohmann::json::value_t::discarded:
            break;
    }
    throw ExtensionError(ExtensionErrc::command_failed, "a value that JSON text cannot hold");
}

context::ContextValue to_context_value(const nlohmann::json& json) {
    switch (json.type()) {
        case nlohmann::json::value_t::null:
            return {};
        case nlohmann::json::value_t::boolean:
            return json.get<bool>();
        case nlohmann::json::value_t::number_integer:
        case nlohmann::json::value_t::number_unsigned:
        case nlohmann::json::value_t::number_float:
            return json.get<double>();
        case nlohmann::json::value_t::string:
            return json.get<std::string>();
        case nlohmann::json::value_t::array: {
            context::ContextValue::List list;
            for (const nlohmann::json& item : json) {
                if (!item.is_string()) {
                    break;
                }
                list.push_back(item.get<std::string>());
            }
            if (list.size() == json.size()) {
                return list;
            }
            break;
        }
        default:
            break;
    }
    throw ExtensionError(ExtensionErrc::invalid_state,
                         "a context key holds true, false, a number, text, an array of text or "
                         "null, not " +
                             json.dump());
}

nlohmann::json to_json(const context::ContextValue& value) {
    using Kind = context::ContextValue::Kind;
    switch (value.kind()) {
        case Kind::none:
            return nullptr;
        case Kind::boolean:
            return value.as_boolean();
        case Kind::number:
            return value.as_number();
        case Kind::string:
            return value.as_string();
        case Kind::list:
            return value.as_list();
    }
    return nullptr;
}

}  // namespace fem::extension::detail
