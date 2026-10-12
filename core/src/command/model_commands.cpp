#include "fem/core/command/model_commands.h"

#include "fem/core/command/command_bus.h"
#include "fem/core/command/error.h"
#include "fem/core/model/document.h"
#include "fem/core/model/error.h"
#include "fem/core/model/units.h"

#include "model_commands_registry.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace fem::command::detail {

namespace {

[[noreturn]] void throw_argument(std::string_view name, const std::string& problem) {
    throw CommandError(CommandErrc::invalid_arguments,
                       "argument '" + std::string(name) + "': " + problem);
}

model::Properties to_properties(const Value& value) {
    switch (value.kind()) {
        case Value::Kind::none:
            return nullptr;
        case Value::Kind::boolean:
            return value.as_boolean();
        case Value::Kind::integer:
            return value.as_integer();
        case Value::Kind::number:
            return value.as_number();
        case Value::Kind::string:
            return value.as_string();
        case Value::Kind::path:
            throw_argument("properties", "properties hold JSON; pass the path as text");
        case Value::Kind::list: {
            model::Properties list = model::Properties::array();
            for (const Value& item : value.as_list()) {
                list.push_back(to_properties(item));
            }
            return list;
        }
        case Value::Kind::dict: {
            model::Properties dict = model::Properties::object();
            for (const auto& [key, item] : value.as_dict()) {
                dict[key] = to_properties(item);
            }
            return dict;
        }
    }
    return nullptr;
}

std::string describe(const model::Document& doc,
                     const std::vector<model::RemovalBlocker>& blockers) {
    std::string text = "removal is blocked:";
    for (const model::RemovalBlocker& blocker : blockers) {
        const std::string owner = "'" + doc.get(blocker.opaque).owner() + "'";
        if (blocker.reason == model::RemovalBlocker::Reason::opaque) {
            text += " " + model::to_string(blocker.object) + " belongs to disabled module " +
                    owner + ";";
        } else {
            text += " " + model::to_string(blocker.object) + " is needed by " +
                    model::to_string(blocker.opaque) + " of disabled module " + owner + ";";
        }
    }
    text.pop_back();
    return text;
}

template <class Unit>
Unit parse_unit(const Arguments& args, std::string_view name,
                std::optional<Unit> (*parse)(std::string_view) noexcept) {
    const std::string& symbol = args.string(name);
    const std::optional<Unit> unit = parse(symbol);
    if (!unit) {
        throw_argument(name, "unknown unit '" + symbol + "'");
    }
    return *unit;
}

ArgSpec required(std::string name, ArgType type) {
    return {.name = std::move(name), .type = type};
}

ArgSpec optional(std::string name, ArgType type, std::optional<Value> default_value = {}) {
    return {.name = std::move(name),
            .type = type,
            .required = false,
            .default_value = std::move(default_value)};
}

}  // namespace

void register_model_commands(CommandBus& bus) {
    const std::string owner(model::core_owner);
    const auto spec = [&](std::string_view id, std::vector<ArgSpec> args) {
        return CommandSpec{.id = std::string(id), .owner = owner, .args = std::move(args)};
    };

    bus.register_command(
        spec(model_commands::create,
             {required("type", ArgType::string), required("owner", ArgType::string),
              optional("name", ArgType::string, Value("")), optional("parent", ArgType::object),
              optional("dependencies", ArgType::list, Value(Value::List{})),
              optional("schema_version", ArgType::integer, Value(1)),
              optional("properties", ArgType::dict, Value(Value::Dict{}))}),
        [](CommandContext& context) {
            const Arguments& args = context.args();
            model::NewObject init;
            init.type = args.string("type");
            init.owner = args.string("owner");
            init.name = args.string("name");
            if (args.has("parent")) {
                init.parent = args.object("parent");
            }
            init.dependencies = args.objects("dependencies");
            const std::int64_t version = args.integer("schema_version");
            if (version < 0 || version > std::numeric_limits<std::uint32_t>::max()) {
                throw_argument("schema_version", "out of range");
            }
            init.schema_version = static_cast<std::uint32_t>(version);
            init.properties = to_properties(args.at("properties"));
            return Value(context.editor().create(std::move(init)));
        });

    bus.register_command(
        spec(model_commands::rename,
             {required("object", ArgType::object), required("name", ArgType::string)}),
        [](CommandContext& context) {
            context.editor().rename(context.args().object("object"), context.args().string("name"));
            return CommandResult{};
        });

    bus.register_command(
        spec(model_commands::set_properties,
             {required("object", ArgType::object), required("properties", ArgType::dict)}),
        [](CommandContext& context) {
            context.editor().set_properties(context.args().object("object"),
                                            to_properties(context.args().at("properties")));
            return CommandResult{};
        });

    bus.register_command(spec(model_commands::remove, {required("object", ArgType::object)}),
                         [](CommandContext& context) {
                             const model::ObjectId id = context.args().object("object");
                             const model::RemoveResult result = context.editor().remove(id);
                             if (!result.ok()) {
                                 throw model::ModelError(
                                     model::ModelErrc::removal_blocked,
                                     describe(context.document(), result.blockers), id);
                             }
                             Value::List removed;
                             for (const model::ObjectId member : result.removed) {
                                 removed.emplace_back(member);
                             }
                             return Value(std::move(removed));
                         });

    bus.register_command(
        spec(model_commands::add_dependency,
             {required("object", ArgType::object), required("target", ArgType::object)}),
        [](CommandContext& context) {
            return Value(context.editor().add_dependency(context.args().object("object"),
                                                         context.args().object("target")));
        });

    bus.register_command(
        spec(model_commands::remove_dependency,
             {required("object", ArgType::object), required("target", ArgType::object)}),
        [](CommandContext& context) {
            return Value(context.editor().remove_dependency(context.args().object("object"),
                                                            context.args().object("target")));
        });

    bus.register_command(
        spec(model_commands::set_units,
             {required("length", ArgType::string), required("mass", ArgType::string),
              required("time", ArgType::string), required("temperature", ArgType::string)}),
        [](CommandContext& context) {
            const Arguments& args = context.args();
            const model::UnitSystem units{
                parse_unit<model::LengthUnit>(args, "length", model::parse_length_unit),
                parse_unit<model::MassUnit>(args, "mass", model::parse_mass_unit),
                parse_unit<model::TimeUnit>(args, "time", model::parse_time_unit),
                parse_unit<model::TemperatureUnit>(args, "temperature",
                                                   model::parse_temperature_unit),
            };
            context.editor().set_units(units);
            return CommandResult{};
        });
}

}  // namespace fem::command::detail
