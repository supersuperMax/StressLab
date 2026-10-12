#include "fem/core/command/command.h"

#include "fem/core/command/error.h"

#include <string>

namespace fem::command {

namespace {

[[noreturn]] void throw_argument(std::string_view name, const std::string& problem) {
    throw CommandError(CommandErrc::invalid_arguments,
                       "argument '" + std::string(name) + "': " + problem);
}

/// Reads an argument with `read`, reporting a value of another kind as an argument error.
template <class Read>
decltype(auto) read_argument(const Arguments& args, std::string_view name, Read read) {
    const Value& value = args.at(name);
    try {
        return read(value);
    } catch (const CommandError& error) {
        throw_argument(name, error.what());
    }
}

model::ObjectId to_object_id(const Value& value) {
    const std::int64_t id = value.as_integer();
    if (id < 1) {
        throw CommandError(CommandErrc::invalid_value, "object ids start at 1");
    }
    return model::ObjectId(static_cast<std::uint64_t>(id));
}

}  // namespace

std::string_view to_string(ArgType type) noexcept {
    switch (type) {
        case ArgType::boolean:
            return "boolean";
        case ArgType::integer:
            return "integer";
        case ArgType::number:
            return "number";
        case ArgType::string:
            return "string";
        case ArgType::path:
            return "path";
        case ArgType::object:
            return "object";
        case ArgType::list:
            return "list";
        case ArgType::dict:
            return "dict";
        case ArgType::any:
            return "any";
    }
    return "unknown";
}

const ArgSpec* CommandSpec::find_arg(std::string_view name) const {
    for (const ArgSpec& arg : args) {
        if (arg.name == name) {
            return &arg;
        }
    }
    return nullptr;
}

bool Arguments::has(std::string_view name) const {
    return find(name) != nullptr;
}

const Value* Arguments::find(std::string_view name) const {
    const auto it = values_.find(name);
    return it == values_.end() ? nullptr : &it->second;
}

const Value& Arguments::at(std::string_view name) const {
    if (const Value* value = find(name)) {
        return *value;
    }
    throw_argument(name, "missing");
}

bool Arguments::boolean(std::string_view name) const {
    return read_argument(*this, name, [](const Value& value) { return value.as_boolean(); });
}

std::int64_t Arguments::integer(std::string_view name) const {
    return read_argument(*this, name, [](const Value& value) { return value.as_integer(); });
}

double Arguments::number(std::string_view name) const {
    return read_argument(*this, name, [](const Value& value) { return value.as_number(); });
}

const std::string& Arguments::string(std::string_view name) const {
    return read_argument(
        *this, name, [](const Value& value) -> const std::string& { return value.as_string(); });
}

const std::filesystem::path& Arguments::path(std::string_view name) const {
    return read_argument(*this, name, [](const Value& value) -> const std::filesystem::path& {
        return value.as_path();
    });
}

model::ObjectId Arguments::object(std::string_view name) const {
    return read_argument(*this, name, to_object_id);
}

std::vector<model::ObjectId> Arguments::objects(std::string_view name) const {
    return read_argument(*this, name, [](const Value& value) {
        std::vector<model::ObjectId> ids;
        for (const Value& item : value.as_list()) {
            ids.push_back(to_object_id(item));
        }
        return ids;
    });
}

const Value::List& Arguments::list(std::string_view name) const {
    return read_argument(*this, name,
                         [](const Value& value) -> const Value::List& { return value.as_list(); });
}

const Value::Dict& Arguments::dict(std::string_view name) const {
    return read_argument(*this, name,
                         [](const Value& value) -> const Value::Dict& { return value.as_dict(); });
}

}  // namespace fem::command
