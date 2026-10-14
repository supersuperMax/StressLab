#include "fem/core/form/form.h"

#include "fem/core/model/object.h"

#include "../model/utf8.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace fem::form {

namespace {

using Json = nlohmann::json;

/// A name as a part of a JSON Pointer: '~' is "~0", '/' is "~1".
std::string escape(std::string_view name) {
    std::string text;
    for (const char c : name) {
        if (c == '~') {
            text += "~0";
        } else if (c == '/') {
            text += "~1";
        } else {
            text += c;
        }
    }
    return text;
}

std::string join(std::string_view prefix, std::string_view key) {
    return prefix.empty() ? escape(key) : std::string(prefix) + "/" + escape(key);
}

/// Value as JSON text for messages: "8", "\"fast\"".
std::string shown(const Json& value) {
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

class Reader {
public:
    explicit Reader(std::vector<SchemaError>* errors) : errors_(errors) {}

    /// Fields of an object schema; `closed` gets `additionalProperties = false`.
    std::vector<Field> object_fields(const Json& schema, const std::string& prefix, bool& closed) {
        std::vector<Field> fields;
        if (const auto additional = schema.find("additionalProperties");
            additional != schema.end() && additional->is_boolean()) {
            closed = !additional->get<bool>();
        }
        std::set<std::string, std::less<>> required;
        if (const auto names = schema.find("required"); names != schema.end()) {
            if (!names->is_array()) {
                error(prefix, "'required' must be an array of property names");
            } else {
                for (const Json& name : *names) {
                    if (name.is_string()) {
                        required.insert(name.get<std::string>());
                    } else {
                        error(prefix,
                              "'required' must be an array of property names, not " + shown(name));
                    }
                }
            }
        }
        const auto properties = schema.find("properties");
        if (properties != schema.end() && !properties->is_object()) {
            error(prefix, "'properties' must be an object of property schemas");
        } else if (properties != schema.end()) {
            for (const auto& [key, property] : properties->items()) {
                fields.push_back(field(key, join(prefix, key), property, required.contains(key)));
            }
        }
        sort(fields);
        return fields;
    }

    void error(std::string pointer, std::string message) const {
        if (errors_ != nullptr) {
            errors_->push_back({std::move(pointer), std::move(message)});
        }
    }

    static void sort(std::vector<Field>& fields) {
        std::stable_sort(fields.begin(), fields.end(), [](const Field& a, const Field& b) {
            if (a.order.has_value() != b.order.has_value()) {
                return a.order.has_value();
            }
            if (a.order && *a.order != *b.order) {
                return *a.order < *b.order;
            }
            return a.key < b.key;
        });
    }

private:
    Field field(const std::string& key, std::string pointer, const Json& schema, bool required) {
        Field result;
        result.key = key;
        result.pointer = std::move(pointer);
        result.required = required;
        result.schema = schema;
        if (!schema.is_object()) {
            error(result.pointer,
                  "the schema of a property must be an object, not " + shown(schema));
            return result;
        }
        result.title = text(schema, "title", result.pointer);
        result.description = text(schema, "description", result.pointer);
        result.advanced = flag(schema, "x-advanced", result.pointer);
        result.read_only = flag(schema, "readOnly", result.pointer);
        if (const auto order = schema.find("x-order"); order != schema.end()) {
            if (order->is_number_integer()) {
                result.order = order->get<std::int64_t>();
            } else {
                error(result.pointer, "'x-order' must be an integer, not " + shown(*order));
            }
        }
        if (const auto value = schema.find("default"); value != schema.end()) {
            result.default_value = *value;
        }

        const std::string type = text(schema, "type", result.pointer);
        if (const auto values = schema.find("enum"); values != schema.end()) {
            choices(result, schema, *values);
        } else if (type == "boolean") {
            result.kind = FieldKind::boolean;
        } else if (type == "integer") {
            result.kind = FieldKind::integer;
        } else if (type == "number") {
            result.kind = FieldKind::number;
        } else if (type == "string") {
            const auto format = schema.find("format");
            result.kind =
                format != schema.end() && *format == "path" ? FieldKind::path : FieldKind::string;
        } else if (type == "object" && schema.contains("properties")) {
            result.kind = FieldKind::group;
            result.fields = object_fields(schema, result.pointer, result.closed);
        }

        if (result.kind == FieldKind::integer || result.kind == FieldKind::number) {
            limits(result, schema);
        }
        if (const auto quantity = schema.find("x-quantity"); quantity != schema.end()) {
            const model::Quantity* found = quantity->is_string()
                                               ? model::find_quantity(quantity->get<std::string>())
                                               : nullptr;
            if (result.kind != FieldKind::integer && result.kind != FieldKind::number) {
                error(result.pointer,
                      "'x-quantity' is only for properties of type number or "
                      "integer");
            } else if (found == nullptr) {
                error(result.pointer, "unknown quantity " + shown(*quantity) +
                                          " in 'x-quantity'; see fem/core/model/quantity.h");
            } else {
                result.quantity = found;
            }
        }
        return result;
    }

    void choices(Field& field, const Json& schema, const Json& values) {
        if (!values.is_array() || values.empty()) {
            error(field.pointer, "'enum' must be a non-empty array of allowed values");
            return;
        }
        field.kind = FieldKind::choice;
        for (const Json& value : values) {
            field.choices.push_back({.value = value, .title = {}});
        }
        const auto titles = schema.find("x-enumTitles");
        if (titles == schema.end()) {
            return;
        }
        if (!titles->is_array() || titles->size() != values.size()) {
            error(field.pointer,
                  "'x-enumTitles' must be an array with a title for each value of "
                  "'enum'");
            return;
        }
        for (std::size_t i = 0; i < values.size(); ++i) {
            if ((*titles)[i].is_string()) {
                field.choices[i].title = (*titles)[i].get<std::string>();
            } else {
                error(field.pointer,
                      "a title of 'x-enumTitles' must be text, not " + shown((*titles)[i]));
            }
        }
    }

    void limits(Field& field, const Json& schema) {
        const auto number = [&](std::string_view name) -> std::optional<double> {
            const auto found = schema.find(name);
            if (found == schema.end() || found->is_boolean()) {
                return std::nullopt;
            }
            if (!found->is_number()) {
                error(field.pointer,
                      "'" + std::string(name) + "' must be a number, not " + shown(*found));
                return std::nullopt;
            }
            return found->get<double>();
        };
        // Draft 6 and later: exclusiveMinimum is the limit; draft 4: a flag on minimum.
        field.minimum = number("minimum");
        field.maximum = number("maximum");
        field.exclusive_minimum = flag(schema, "exclusiveMinimum", {});
        field.exclusive_maximum = flag(schema, "exclusiveMaximum", {});
        if (const auto limit = number("exclusiveMinimum")) {
            field.minimum = limit;
            field.exclusive_minimum = true;
        }
        if (const auto limit = number("exclusiveMaximum")) {
            field.maximum = limit;
            field.exclusive_maximum = true;
        }
    }

    /// A text member; empty if absent or of another type (reported, except for "type", which
    /// JSON Schema allows to be an array).
    std::string text(const Json& schema, std::string_view name, const std::string& pointer) {
        const auto found = schema.find(name);
        if (found == schema.end()) {
            return {};
        }
        if (!found->is_string()) {
            if (name != "type") {
                error(pointer, "'" + std::string(name) + "' must be text, not " + shown(*found));
            }
            return {};
        }
        return found->get<std::string>();
    }

    /// A boolean member; false if absent or of another type (reported, unless it is a number
    /// where JSON Schema allows one).
    bool flag(const Json& schema, std::string_view name, const std::string& pointer) {
        const auto found = schema.find(name);
        if (found == schema.end()) {
            return false;
        }
        if (found->is_boolean()) {
            return found->get<bool>();
        }
        if (!found->is_number()) {
            error(pointer,
                  "'" + std::string(name) + "' must be true or false, not " + shown(*found));
        }
        return false;
    }

    std::vector<SchemaError>* errors_;
};

void collect(const std::vector<Field>& fields, std::vector<const Field*>& out) {
    for (const Field& field : fields) {
        out.push_back(&field);
        collect(field.fields, out);
    }
}

Json field_json(const Field& field) {
    Json out = Json::object();
    out["key"] = field.key;
    out["pointer"] = field.pointer;
    out["kind"] = to_string(field.kind);
    const auto set_text = [&out](const char* name, const std::string& value) {
        if (!value.empty()) {
            out[name] = value;
        }
    };
    set_text("title", field.title);
    set_text("description", field.description);
    const auto set_flag = [&out](const char* name, bool value) {
        if (value) {
            out[name] = true;
        }
    };
    set_flag("required", field.required);
    set_flag("advanced", field.advanced);
    set_flag("readOnly", field.read_only);
    set_flag("closed", field.closed);
    if (field.order) {
        out["order"] = *field.order;
    }
    if (!field.default_value.is_null()) {
        out["default"] = field.default_value;
    }
    if (field.minimum) {
        out["minimum"] = *field.minimum;
        set_flag("exclusiveMinimum", field.exclusive_minimum);
    }
    if (field.maximum) {
        out["maximum"] = *field.maximum;
        set_flag("exclusiveMaximum", field.exclusive_maximum);
    }
    if (field.quantity != nullptr) {
        out["quantity"] = field.quantity->id;
    }
    if (!field.choices.empty()) {
        Json choices = Json::array();
        for (const Choice& choice : field.choices) {
            choices.push_back(choice.title.empty()
                                  ? Json{{"value", choice.value}}
                                  : Json{{"value", choice.value}, {"title", choice.title}});
        }
        out["choices"] = std::move(choices);
    }
    if (!field.fields.empty()) {
        Json fields = Json::array();
        for (const Field& child : field.fields) {
            fields.push_back(field_json(child));
        }
        out["fields"] = std::move(fields);
    }
    return out;
}

std::optional<Problem> limit_problem(const Field& field, double number) {
    if (field.minimum &&
        (field.exclusive_minimum ? number <= *field.minimum : number < *field.minimum)) {
        return Problem{
            .code = ProblemCode::below_minimum,
            .severity = Severity::error,
            .pointer = field.pointer,
            .title = field.title,
            .detail = {{"limit", *field.minimum}, {"exclusive", field.exclusive_minimum}}};
    }
    if (field.maximum &&
        (field.exclusive_maximum ? number >= *field.maximum : number > *field.maximum)) {
        return Problem{
            .code = ProblemCode::above_maximum,
            .severity = Severity::error,
            .pointer = field.pointer,
            .title = field.title,
            .detail = {{"limit", *field.maximum}, {"exclusive", field.exclusive_maximum}}};
    }
    return std::nullopt;
}

void check_fields(const std::vector<Field>& fields, bool closed, const Json& values,
                  const std::string& prefix, std::vector<Problem>& problems) {
    for (const Field& field : fields) {
        if (field.kind == FieldKind::section) {
            check_fields(field.fields, false, values, prefix, problems);
            continue;
        }
        const Json* value = nullptr;
        if (values.is_object()) {
            if (const auto found = values.find(field.key); found != values.end()) {
                value = &*found;
            }
        }
        if (is_empty(value)) {
            if (field.required) {
                problems.push_back({.code = ProblemCode::required,
                                    .severity = Severity::error,
                                    .pointer = field.pointer,
                                    .title = field.title,
                                    .detail = Json::object()});
            }
            continue;
        }
        if (field.kind == FieldKind::group) {
            if (!value->is_object()) {
                problems.push_back({.code = ProblemCode::wrong_type,
                                    .severity = Severity::error,
                                    .pointer = field.pointer,
                                    .title = field.title,
                                    .detail = {{"type", to_string(field.kind)}}});
            } else {
                check_fields(field.fields, field.closed, *value, field.pointer, problems);
            }
            continue;
        }
        if (auto problem = check_value(field, *value)) {
            problems.push_back(std::move(*problem));
        }
    }
    if (!closed || !values.is_object()) {
        return;
    }
    for (const auto& [key, value] : values.items()) {
        const bool described = std::any_of(fields.begin(), fields.end(), [&key](const Field& f) {
            return f.key == key && f.kind != FieldKind::section;
        });
        if (!described) {
            problems.push_back({.code = ProblemCode::unknown_property,
                                .severity = Severity::warning,
                                .pointer = join(prefix, key),
                                .title = {},
                                .detail = Json::object()});
        }
    }
}

}  // namespace

std::string_view to_string(FieldKind kind) noexcept {
    switch (kind) {
        case FieldKind::boolean:
            return "boolean";
        case FieldKind::integer:
            return "integer";
        case FieldKind::number:
            return "number";
        case FieldKind::string:
            return "string";
        case FieldKind::path:
            return "path";
        case FieldKind::choice:
            return "choice";
        case FieldKind::group:
            return "group";
        case FieldKind::section:
            return "section";
        case FieldKind::raw:
            break;
    }
    return "raw";
}

FormSpec parse(const Json& schema, std::vector<SchemaError>* errors) {
    Reader reader(errors);
    FormSpec form;
    if (!schema.is_object()) {
        reader.error({}, "a property schema must be an object schema, not " + shown(schema));
        return form;
    }
    if (const auto type = schema.find("type"); type != schema.end() && *type != "object") {
        reader.error({}, "a property schema must have type \"object\", not " + shown(*type));
        return form;
    }
    form.fields = reader.object_fields(schema, {}, form.closed);
    return form;
}

FormSpec settings_form(std::span<const extension::SettingContribution* const> schemas) {
    using Type = extension::SettingContribution::Type;
    FormSpec form;
    for (const extension::SettingContribution* setting : schemas) {
        auto section = std::find_if(form.fields.begin(), form.fields.end(),
                                    [setting](const Field& f) { return f.key == setting->owner; });
        if (section == form.fields.end()) {
            Field heading;
            heading.key = setting->owner;
            heading.kind = FieldKind::section;
            form.fields.push_back(std::move(heading));
            section = std::prev(form.fields.end());
        }
        Field field;
        field.key = setting->key;
        field.pointer = setting->key;
        field.schema = setting->schema;
        field.description = setting->description;
        if (const auto title = setting->schema.find("title");
            title != setting->schema.end() && title->is_string()) {
            field.title = title->get<std::string>();
        }
        field.default_value = setting->default_value;
        switch (setting->type) {
            case Type::boolean:
                field.kind = FieldKind::boolean;
                break;
            case Type::integer:
                field.kind = FieldKind::integer;
                break;
            case Type::number:
                field.kind = FieldKind::number;
                break;
            case Type::string:
                field.kind = FieldKind::string;
                break;
            case Type::path:
                field.kind = FieldKind::path;
                break;
        }
        if (const auto values = setting->schema.find("enum");
            values != setting->schema.end() && values->is_array()) {
            field.kind = FieldKind::choice;
            for (const Json& value : *values) {
                field.choices.push_back({.value = value, .title = {}});
            }
        }
        for (const auto& [name, limit] :
             {std::pair{"minimum", &field.minimum}, std::pair{"maximum", &field.maximum}}) {
            if (const auto found = setting->schema.find(name);
                found != setting->schema.end() && found->is_number()) {
                *limit = found->get<double>();
            }
        }
        section->fields.push_back(std::move(field));
    }
    for (Field& section : form.fields) {
        Reader::sort(section.fields);
    }
    return form;
}

std::vector<const Field*> all_fields(const FormSpec& form) {
    std::vector<const Field*> out;
    collect(form.fields, out);
    return out;
}

const Field* find_field(const FormSpec& form, std::string_view pointer) {
    for (const Field* field : all_fields(form)) {
        if (field->kind != FieldKind::section && field->pointer == pointer) {
            return field;
        }
    }
    return nullptr;
}

Json to_json(const FormSpec& form) {
    Json fields = Json::array();
    for (const Field& field : form.fields) {
        fields.push_back(field_json(field));
    }
    Json out{{"fields", std::move(fields)}};
    if (form.closed) {
        out["closed"] = true;
    }
    return out;
}

std::string_view to_string(Severity severity) noexcept {
    switch (severity) {
        case Severity::warning:
            return "warning";
        case Severity::error:
            return "error";
        case Severity::ok:
            break;
    }
    return "ok";
}

std::string_view to_string(ProblemCode code) noexcept {
    switch (code) {
        case ProblemCode::required:
            return "required";
        case ProblemCode::wrong_type:
            return "wrongType";
        case ProblemCode::below_minimum:
            return "belowMinimum";
        case ProblemCode::above_maximum:
            return "aboveMaximum";
        case ProblemCode::not_in_enum:
            return "notInEnum";
        case ProblemCode::unknown_property:
            return "unknownProperty";
        case ProblemCode::missing_dependency:
            return "missingDependency";
        case ProblemCode::schema_version:
            break;
    }
    return "schemaVersion";
}

bool is_empty(const Json* value) noexcept {
    return value == nullptr || value->is_null() ||
           (value->is_string() && value->get_ref<const std::string&>().empty());
}

std::optional<Problem> check_value(const Field& field, const Json& value) {
    bool fits = true;
    switch (field.kind) {
        case FieldKind::boolean:
            fits = value.is_boolean();
            break;
        case FieldKind::integer:
            fits = value.is_number_integer();
            break;
        case FieldKind::number:
            fits = value.is_number() && std::isfinite(value.get<double>());
            break;
        case FieldKind::string:
        case FieldKind::path:
            fits = value.is_string() &&
                   model::detail::is_valid_utf8(value.get_ref<const std::string&>()) &&
                   (field.kind != FieldKind::path ||
                    value.get_ref<const std::string&>().find('\0') == std::string::npos);
            break;
        case FieldKind::choice: {
            const bool listed =
                std::any_of(field.choices.begin(), field.choices.end(),
                            [&value](const Choice& choice) { return choice.value == value; });
            if (!listed) {
                std::string values;
                for (const Choice& choice : field.choices) {
                    values += (values.empty() ? "" : ", ") + shown(choice.value);
                }
                return Problem{.code = ProblemCode::not_in_enum,
                               .severity = Severity::error,
                               .pointer = field.pointer,
                               .title = field.title,
                               .detail = {{"values", values}}};
            }
            return std::nullopt;
        }
        case FieldKind::group:
        case FieldKind::section:
        case FieldKind::raw:
            return std::nullopt;
    }
    if (!fits) {
        return Problem{.code = ProblemCode::wrong_type,
                       .severity = Severity::error,
                       .pointer = field.pointer,
                       .title = field.title,
                       .detail = {{"type", to_string(field.kind)}}};
    }
    if (field.kind == FieldKind::integer || field.kind == FieldKind::number) {
        return limit_problem(field, value.get<double>());
    }
    return std::nullopt;
}

std::vector<Problem> check(const FormSpec& form, const Json& properties) {
    std::vector<Problem> problems;
    check_fields(form.fields, form.closed, properties, {}, problems);
    return problems;
}

std::string text(const Problem& problem, const l10n::Localization& localization,
                 std::string_view owner) {
    l10n::Args args;
    args.emplace("field", problem.title.empty() ? problem.pointer
                                                : localization.manifest_text(owner, problem.title));
    std::string key = "form.problem." + std::string(to_string(problem.code));
    for (const auto& [name, value] : problem.detail.items()) {
        if (name == "exclusive") {
            if (value == true) {
                key += ".exclusive";
            }
        } else if (name == "type") {
            args.emplace(name, localization.text(model::core_owner,
                                                 "form.type." + value.get<std::string>()));
        } else if (value.is_string()) {
            args.emplace(name, value.get<std::string>());
        } else if (value.is_number_integer()) {
            args.emplace(name, value.get<std::int64_t>());
        } else if (value.is_number()) {
            args.emplace(name, value.get<double>());
        }
    }
    return localization.text(model::core_owner, key, args);
}

Severity worst(std::span<const Problem> problems) noexcept {
    Severity result = Severity::ok;
    for (const Problem& problem : problems) {
        result = std::max(result, problem.severity);
    }
    return result;
}

}  // namespace fem::form
