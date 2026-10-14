#pragma once

#include "fem/core/extension/contributions.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/model/quantity.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Forms from schemas (docs/architecture.md §4.2 `propertySchemas` and `settings`,
// docs/adr/0014-tree-and-forms.md). A schema is read once into a `FormSpec`: the fields the shell
// draws, in order, with their types, units, limits and allowed values. The same spec checks values,
// so the form, the status of a node in the model tree and scripts agree on what is wrong.
//
// Schemas are JSON Schema (type, title, description, default, enum, minimum, maximum,
// exclusiveMinimum, exclusiveMaximum, required, properties, readOnly, additionalProperties) with
// extensions of StressLab:
//
//   x-quantity = "pressure"  the value is a physical quantity in the units of the model
//                            (`fem::model::find_quantity`); forms show it with a unit to pick
//   x-advanced = true        the field is in the folded "Advanced" section of its group
//   x-order = 10             the order of fields (objects of JSON and TOML sort their keys); fields
//                            with an order come first, by it, then the others by key
//   x-enumTitles = [...]     titles of the values of `enum`, in the same order
//   format = "path"          a text field holding a path to a file
//
// A nested `type = "object"` with `properties` is a group of fields. What a form cannot show
// (`oneOf`, `$ref`, arrays...) becomes a read-only field with the value as JSON text.
namespace fem::form {

enum class FieldKind : std::uint8_t {
    boolean,
    integer,
    number,
    string,
    path,
    choice,   ///< One of the values of `enum`, whatever their type.
    group,    ///< A nested object: its fields are in `fields`, its value is an object.
    section,  ///< A heading over `fields` that holds no value itself (settings of one module).
    raw,      ///< What forms cannot show: the value as JSON text, read-only.
};

/// "boolean", "integer", "number", "string", "path", "choice", "group", "section", "raw".
[[nodiscard]] std::string_view to_string(FieldKind kind) noexcept;

/// A value of a choice field and its title as written (may be "%key%"); empty title: the value.
struct Choice {
    nlohmann::json value;
    std::string title;
};

/// One field of a form.
struct Field {
    std::string key;  ///< Name of the property within its group, or the key of a setting.
    /// Where the value is: for properties, the path from the root as a JSON Pointer without the
    /// leading '/' ("solver/tolerance"), as `model.set_property` takes it; for settings, the key.
    std::string pointer;
    FieldKind kind = FieldKind::raw;
    std::string title;  ///< As written; may be "%key%"; empty: the key.
    std::string description;
    bool required = false;
    bool advanced = false;
    bool read_only = false;
    std::optional<std::int64_t> order;
    nlohmann::json default_value;  ///< Null if none.
    std::optional<double> minimum;
    std::optional<double> maximum;
    bool exclusive_minimum = false;
    bool exclusive_maximum = false;
    std::vector<Choice> choices;                ///< Of a choice field.
    const model::Quantity* quantity = nullptr;  ///< Of an integer or number field, if any.
    /// A group refuses properties it does not describe (`additionalProperties = false`).
    bool closed = false;
    std::vector<Field> fields;  ///< Of a group or a section, in order.
    nlohmann::json schema;      ///< The schema of the field as written.
};

/// A form: the fields of the root object of a schema.
struct FormSpec {
    std::vector<Field> fields;
    bool closed = false;  ///< `additionalProperties = false` at the root.
};

/// A mistake in a schema, for the author of the module.
struct SchemaError {
    std::string pointer;  ///< The field; empty for the root.
    std::string message;
};

/// Reads a schema of an object. Mistakes in what forms rely on (`x-quantity` of an unknown
/// quantity, `x-order` that is not an integer, `x-enumTitles` of another length, a root that is
/// not an object schema...) are reported in `errors`; the form goes on without what is wrong (a
/// field that is not a schema becomes a raw one).
[[nodiscard]] FormSpec parse(const nlohmann::json& schema,
                             std::vector<SchemaError>* errors = nullptr);

/// The form of settings: a section per owner (in the order of `schemas`, owners as they first
/// appear), the settings of the owner in it by key. A field's `key` and `pointer` are the key of
/// the setting, its title the `title` of the entry (or empty), and its kind follows the type of
/// the setting (`enum` makes a choice).
[[nodiscard]] FormSpec settings_form(
    std::span<const extension::SettingContribution* const> schemas);

/// The fields of a form, of its groups and sections, depth first.
[[nodiscard]] std::vector<const Field*> all_fields(const FormSpec& form);
/// The field at a pointer, or nullptr.
[[nodiscard]] const Field* find_field(const FormSpec& form, std::string_view pointer);

/// The spec as JSON, for golden tests and diagnostics: deterministic, the `schema` of fields left
/// out.
[[nodiscard]] nlohmann::json to_json(const FormSpec& form);

// --- Checks --------------------------------------------------------------------------------------

enum class Severity : std::uint8_t { ok, warning, error };

/// "ok", "warning", "error".
[[nodiscard]] std::string_view to_string(Severity severity) noexcept;

enum class ProblemCode : std::uint8_t {
    required,            ///< A required field is empty.
    wrong_type,          ///< The value is not of the type of the field.
    below_minimum,       ///< detail: {"limit": number, "exclusive": bool}
    above_maximum,       ///< detail: {"limit": number, "exclusive": bool}
    not_in_enum,         ///< detail: {"values": "a, b"}
    unknown_property,    ///< A closed object has a property the schema does not describe.
    missing_dependency,  ///< detail: {"object": id} — the object depends on a removed one.
    schema_version,      ///< detail: {"object": n, "schema": m} — written with another schema.
};

/// "required", "wrongType", "belowMinimum", "aboveMaximum", "notInEnum", "unknownProperty",
/// "missingDependency", "schemaVersion".
[[nodiscard]] std::string_view to_string(ProblemCode code) noexcept;

/// Something wrong with values or an object.
struct Problem {
    ProblemCode code = ProblemCode::required;
    Severity severity = Severity::error;
    std::string pointer;  ///< The field or property; empty for the object as a whole.
    std::string title;    ///< Of the field as written (may be "%key%"); empty: the pointer.
    nlohmann::json detail = nlohmann::json::object();
};

/// Whether a value fits a field; nothing if it does. The value is present: an empty value is
/// checked by `check`. A group, a section and a raw field accept anything.
[[nodiscard]] std::optional<Problem> check_value(const Field& field, const nlohmann::json& value);

/// True for what counts as no value: absent (`nullptr`), null, or empty text.
[[nodiscard]] bool is_empty(const nlohmann::json* value) noexcept;

/// Problems of properties against a form: required fields that are empty, values that do not fit,
/// and properties of closed objects that the form does not describe (warnings). In the order of
/// the fields.
[[nodiscard]] std::vector<Problem> check(const FormSpec& form, const nlohmann::json& properties);

/// The text of a problem for users ("form.problem.<code>" of the catalogs of the core). Titles
/// written as "%key%" are strings of `owner`.
[[nodiscard]] std::string text(const Problem& problem, const l10n::Localization& localization,
                               std::string_view owner);

/// The worst severity of the problems; ok if there are none.
[[nodiscard]] Severity worst(std::span<const Problem> problems) noexcept;

}  // namespace fem::form
