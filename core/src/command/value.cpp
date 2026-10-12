#include "fem/core/command/value.h"

#include "fem/core/command/error.h"

#include <string>

namespace fem::command {

std::string_view to_string(Value::Kind kind) noexcept {
    switch (kind) {
        case Value::Kind::none:
            return "none";
        case Value::Kind::boolean:
            return "boolean";
        case Value::Kind::integer:
            return "integer";
        case Value::Kind::number:
            return "number";
        case Value::Kind::string:
            return "string";
        case Value::Kind::path:
            return "path";
        case Value::Kind::list:
            return "list";
        case Value::Kind::dict:
            return "dict";
    }
    return "unknown";
}

void Value::throw_out_of_range() {
    throw CommandError(CommandErrc::invalid_value, "integer is out of the int64 range");
}

void Value::throw_kind(Kind expected) const {
    throw CommandError(CommandErrc::invalid_value, "expected " + std::string(to_string(expected)) +
                                                       ", got " + std::string(to_string(kind())));
}

bool Value::as_boolean() const {
    if (const bool* value = std::get_if<bool>(&data_)) {
        return *value;
    }
    throw_kind(Kind::boolean);
}

std::int64_t Value::as_integer() const {
    if (const std::int64_t* value = std::get_if<std::int64_t>(&data_)) {
        return *value;
    }
    throw_kind(Kind::integer);
}

double Value::as_number() const {
    if (const double* value = std::get_if<double>(&data_)) {
        return *value;
    }
    if (const std::int64_t* value = std::get_if<std::int64_t>(&data_)) {
        return static_cast<double>(*value);
    }
    throw_kind(Kind::number);
}

const std::string& Value::as_string() const {
    if (const std::string* value = std::get_if<std::string>(&data_)) {
        return *value;
    }
    throw_kind(Kind::string);
}

const std::filesystem::path& Value::as_path() const {
    if (const std::filesystem::path* value = std::get_if<std::filesystem::path>(&data_)) {
        return *value;
    }
    throw_kind(Kind::path);
}

const Value::List& Value::as_list() const {
    if (const List* value = std::get_if<List>(&data_)) {
        return *value;
    }
    throw_kind(Kind::list);
}

Value::List& Value::as_list() {
    if (List* value = std::get_if<List>(&data_)) {
        return *value;
    }
    throw_kind(Kind::list);
}

const Value::Dict& Value::as_dict() const {
    if (const Dict* value = std::get_if<Dict>(&data_)) {
        return *value;
    }
    throw_kind(Kind::dict);
}

Value::Dict& Value::as_dict() {
    if (Dict* value = std::get_if<Dict>(&data_)) {
        return *value;
    }
    throw_kind(Kind::dict);
}

bool operator==(const Value& a, const Value& b) {
    return a.data_ == b.data_;
}

}  // namespace fem::command
