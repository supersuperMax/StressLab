#include "fem/core/context/value.h"

#include "fem/core/context/error.h"

#include <string>

namespace fem::context {

bool ContextValue::as_boolean() const {
    if (const bool* value = std::get_if<bool>(&data_)) {
        return *value;
    }
    throw_kind(Kind::boolean);
}

double ContextValue::as_number() const {
    if (const double* value = std::get_if<double>(&data_)) {
        return *value;
    }
    throw_kind(Kind::number);
}

const std::string& ContextValue::as_string() const {
    if (const std::string* value = std::get_if<std::string>(&data_)) {
        return *value;
    }
    throw_kind(Kind::string);
}

const ContextValue::List& ContextValue::as_list() const {
    if (const List* value = std::get_if<List>(&data_)) {
        return *value;
    }
    throw_kind(Kind::list);
}

bool ContextValue::is_truthy() const noexcept {
    switch (kind()) {
        case Kind::none:
            return false;
        case Kind::boolean:
            return std::get<bool>(data_);
        case Kind::number:
            return std::get<double>(data_) != 0.0;
        case Kind::string:
            return !std::get<std::string>(data_).empty();
        case Kind::list:
            return !std::get<List>(data_).empty();
    }
    return false;
}

void ContextValue::throw_kind(Kind expected) const {
    throw ContextError(ContextErrc::invalid_value, "expected a value of kind " +
                                                       std::string(to_string(expected)) + ", got " +
                                                       std::string(to_string(kind())));
}

std::string_view to_string(ContextValue::Kind kind) noexcept {
    switch (kind) {
        case ContextValue::Kind::none:
            return "none";
        case ContextValue::Kind::boolean:
            return "boolean";
        case ContextValue::Kind::number:
            return "number";
        case ContextValue::Kind::string:
            return "string";
        case ContextValue::Kind::list:
            return "list";
    }
    return "unknown";
}

}  // namespace fem::context
