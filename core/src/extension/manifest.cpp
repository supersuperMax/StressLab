#include "fem/core/extension/manifest.h"

#include "fem/core/command/error.h"
#include "fem/core/context/error.h"
#include "fem/core/extension/error.h"
#include "fem/core/model/object.h"

#include "files.h"
#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace fem::extension {

namespace {

// --- Names ---------------------------------------------------------------------------------------

bool is_lower(char c) {
    return c >= 'a' && c <= 'z';
}

bool is_letter(char c) {
    return is_lower(c) || (c >= 'A' && c <= 'Z');
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

/// Segments joined by `separator`, between `min_segments` and `max_segments` of them, each starting
/// with a character `first` accepts and made of characters `rest` accepts.
template <class First, class Rest>
bool is_segmented(std::string_view text, char separator, std::size_t min_segments,
                  std::size_t max_segments, First first, Rest rest) {
    std::size_t segments = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find(separator, start);
        const std::string_view segment = text.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (segment.empty() || !first(segment[0])) {
            return false;
        }
        for (const char c : segment) {
            if (!rest(c)) {
                return false;
            }
        }
        ++segments;
        if (end == std::string_view::npos) {
            return segments >= min_segments && segments <= max_segments;
        }
        start = end + 1;
    }
}

bool is_id_char(char c) {
    return is_letter(c) || is_digit(c) || c == '_' || c == '-';
}

constexpr std::size_t unlimited = std::numeric_limits<std::size_t>::max();

/// Ids of stages, panels, layers, filters, icons, file formats and settings.
bool is_contribution_id(std::string_view id) {
    return is_segmented(id, '.', 1, unlimited, is_letter, is_id_char);
}

/// Ids of solvers and meshers (a part of context keys), analysis names.
bool is_single_id(std::string_view id) {
    return is_segmented(id, '.', 1, 1, is_letter, is_id_char);
}

/// Object types of the Model IR.
bool is_object_type(std::string_view type) {
    return is_segmented(
        type, '.', 1, 1, [](char c) { return is_letter(c) || c == '_'; }, is_id_char);
}

bool is_menu_location(std::string_view location) {
    return is_segmented(location, '/', 1, unlimited, is_letter, is_id_char);
}

/// One part of a menu location: the id of a submenu.
bool is_menu_id(std::string_view id) {
    return is_segmented(id, '/', 1, 1, is_letter, is_id_char);
}

/// "ru", "en", "pt-BR": a language of two or three lowercase letters and optional subtags.
bool is_language(std::string_view language) {
    const std::size_t dash = language.find('-');
    const std::string_view primary = language.substr(0, dash);
    if (primary.size() < 2 || primary.size() > 3) {
        return false;
    }
    for (const char c : primary) {
        if (!is_lower(c)) {
            return false;
        }
    }
    return dash == std::string_view::npos ||
           is_segmented(
               language.substr(dash + 1), '-', 1, unlimited,
               [](char c) { return is_letter(c) || is_digit(c); },
               [](char c) { return is_letter(c) || is_digit(c); });
}

/// A path inside the module folder: relative, '/' between folders, no '.', '..' or empty parts.
bool is_relative_path(std::string_view path) {
    if (path.empty() || path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t end = path.find('/', start);
        const std::string_view part = path.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        for (const char c : part) {
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
        }
        if (end == std::string_view::npos) {
            return true;
        }
        start = end + 1;
    }
}

bool is_python_identifier(std::string_view name) {
    return is_segmented(
        name, '.', 1, unlimited, [](char c) { return is_letter(c) || c == '_'; },
        [](char c) { return is_letter(c) || is_digit(c) || c == '_'; });
}

/// "package.module:function".
bool is_python_entry(std::string_view entry) {
    const std::size_t colon = entry.find(':');
    if (colon == std::string_view::npos || entry.find(':', colon + 1) != std::string_view::npos) {
        return false;
    }
    const std::string_view function = entry.substr(colon + 1);
    return is_python_identifier(entry.substr(0, colon)) && is_python_identifier(function) &&
           function.find('.') == std::string_view::npos;
}

std::string ascii_lower(std::string_view text) {
    std::string lower(text);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return lower;
}

/// Why `key` is not a shortcut, or nothing if it is one.
std::optional<std::string> check_shortcut(std::string_view key) {
    constexpr std::array<std::string_view, 5> modifiers = {"mod", "ctrl", "alt", "shift", "meta"};
    const auto is_modifier = [&](std::string_view part) {
        const std::string lower = ascii_lower(part);
        return std::find(modifiers.begin(), modifiers.end(), lower) != modifiers.end();
    };
    std::set<std::string> seen;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = key.find('+', start);
        const std::string_view part =
            key.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (part.empty()) {
            return "an empty part between '+'";
        }
        if (end == std::string_view::npos) {
            if (is_modifier(part)) {
                return "no key after the modifiers";
            }
            for (const char c : part) {
                if (c <= ' ' || c > '~') {
                    return "the key '" + std::string(part) + "' is not printable ASCII";
                }
            }
            return std::nullopt;
        }
        if (!is_modifier(part)) {
            return "'" + std::string(part) +
                   "' is not a modifier; use Mod, Ctrl, Alt, Shift or Meta";
        }
        if (!seen.insert(ascii_lower(part)).second) {
            return "the modifier '" + std::string(part) + "' repeats";
        }
        start = end + 1;
    }
}

/// Length of the UTF-8 sequence starting with `c`.
std::size_t sequence_length(char c) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x80) {
        return 1;
    }
    if ((byte >> 5) == 0x6) {
        return 2;
    }
    if ((byte >> 4) == 0xE) {
        return 3;
    }
    return (byte >> 3) == 0x1E ? 4 : 1;
}

/// '*' matches any text, '?' one character; the rest literally, optionally without ASCII case.
bool glob_match(std::string_view pattern, std::string_view text, bool ignore_case) {
    const auto same = [ignore_case](char a, char b) {
        if (ignore_case) {
            a = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
            b = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b;
        }
        return a == b;
    };
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t star_text = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            star_text = t;
        } else if (p < pattern.size() && pattern[p] == '?') {
            ++p;
            t = std::min(text.size(), t + sequence_length(text[t]));
        } else if (p < pattern.size() && same(pattern[p], text[t])) {
            ++p;
            ++t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            star_text = std::min(text.size(), star_text + sequence_length(text[star_text]));
            t = star_text;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

/// Equal scalars of the kinds settings use: text, booleans and numbers (an integer equals a
/// number).
bool same_value(const toml::node& a, const toml::node& b) {
    if (a.is_string() && b.is_string()) {
        return a.value<std::string>() == b.value<std::string>();
    }
    if (a.is_boolean() && b.is_boolean()) {
        return a.value<bool>() == b.value<bool>();
    }
    const bool numbers =
        (a.is_integer() || a.is_floating_point()) && (b.is_integer() || b.is_floating_point());
    return numbers && a.value<double>() == b.value<double>();
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// --- Rules for messages --------------------------------------------------------------------------

constexpr std::string_view command_id_rule =
    "expected two or more parts of ASCII letters, digits, '_' and '-' joined by '.', each starting "
    "with a letter, like 'solver.run'";
constexpr std::string_view contribution_id_rule =
    "expected parts of ASCII letters, digits, '_' and '-' joined by '.', each starting with a "
    "letter";
constexpr std::string_view single_id_rule =
    "expected ASCII letters, digits, '_' and '-', starting with a letter, without '.'";
constexpr std::string_view object_type_rule =
    "expected ASCII letters, digits, '_' and '-', starting with a letter or '_'";
constexpr std::string_view path_rule =
    "expected a relative path inside the module folder, with '/' between folders and no '..'";

std::string_view type_name(const toml::node& node) {
    switch (node.type()) {
        case toml::node_type::string:
            return "text";
        case toml::node_type::integer:
            return "an integer";
        case toml::node_type::floating_point:
            return "a number";
        case toml::node_type::boolean:
            return "a boolean";
        case toml::node_type::table:
            return "a table";
        case toml::node_type::array:
            return "an array";
        case toml::node_type::date:
            return "a date";
        case toml::node_type::time:
            return "a time";
        case toml::node_type::date_time:
            return "a date-time";
        case toml::node_type::none:
            break;
    }
    return "nothing";
}

// --- Parser --------------------------------------------------------------------------------------

class Parser;

/// Reads the keys of one table and remembers which were read, so the others can be reported as
/// unknown.
class Fields {
public:
    Fields(Parser& parser, const toml::table& table, std::string where)
        : parser_(parser), table_(table), where_(std::move(where)) {}

    [[nodiscard]] const toml::table& table() const noexcept { return table_; }
    [[nodiscard]] const std::string& where() const noexcept { return where_; }

    /// The value of `key`, or nullptr.
    const toml::node* get(std::string_view key) {
        known_.emplace(key);
        return table_.get(key);
    }
    /// The value of `key`; reports it missing.
    const toml::node* require(std::string_view key);
    /// Text of `key`; nothing if it is absent or not text (reported). Required text is not empty.
    std::optional<std::string> text(std::string_view key, bool required);
    std::optional<bool> boolean(std::string_view key);
    std::optional<std::int64_t> integer(std::string_view key);
    /// Reports the keys that were not read.
    void finish();

private:
    Parser& parser_;
    const toml::table& table_;
    std::string where_;
    std::set<std::string, std::less<>> known_;
};

using Seen = std::map<std::string, std::uint32_t, std::less<>>;

class Parser {
public:
    Parser(const Version& engine, std::filesystem::path file)
        : engine_(engine), file_(std::move(file)) {}

    Manifest parse(std::string_view source);

    void error(const toml::source_region& where, std::string message) {
        diagnostics_.push_back({where.begin.line, where.begin.column, std::move(message)});
    }
    void wrong_type(const toml::node& node, std::string_view key, std::string_view where,
                    std::string_view expected) {
        error(node.source(), "'" + std::string(key) + "' in " + std::string(where) + " must be " +
                                 std::string(expected) + ", not " + std::string(type_name(node)));
    }

private:
    /// Reads [extension]; returns false if the module is for another version of the platform.
    bool parse_extension(const toml::table& table, Manifest& manifest);
    void parse_requirements(const toml::node& node, Manifest& manifest);
    void parse_activation(const toml::node& node, Manifest& manifest);
    void parse_contributes(const toml::table& table, Contributions& out);
    void parse_settings(const toml::node& node, Contributions& out);
    std::vector<command::ArgSpec> parse_args(const toml::node& node, const std::string& command);

    void each_table(Fields& contributes, std::string_view key,
                    const std::function<void(const toml::table&, const std::string&)>& read);
    std::string text(Fields& fields, std::string_view key, bool required) {
        return fields.text(key, required).value_or(std::string());
    }
    std::string id(Fields& fields, std::string_view key, bool (*valid)(std::string_view),
                   std::string_view rule, bool required = true);
    std::string path(Fields& fields, std::string_view key, bool required);
    /// A `when` clause (or another field written like one, such as `enablement`).
    std::optional<context::WhenExpression> when(Fields& fields, std::string_view key = "when");
    void unique(Seen& seen, const std::string& value, const toml::node* node,
                std::string_view what);
    std::optional<command::Value> to_value(const toml::node& node);
    std::optional<nlohmann::json> to_json(const toml::node& node);

    const Version& engine_;
    std::filesystem::path file_;
    std::vector<ManifestDiagnostic> diagnostics_;
    bool has_code_ = false;
};

const toml::node* Fields::require(std::string_view key) {
    const toml::node* node = get(key);
    if (node == nullptr) {
        parser_.error(table_.source(), "missing '" + std::string(key) + "' in " + where_);
    }
    return node;
}

std::optional<std::string> Fields::text(std::string_view key, bool required) {
    const toml::node* node = required ? require(key) : get(key);
    if (node == nullptr) {
        return std::nullopt;
    }
    const auto* value = node->as_string();
    if (value == nullptr) {
        parser_.wrong_type(*node, key, where_, "text");
        return std::nullopt;
    }
    if (required && value->get().empty()) {
        parser_.error(node->source(), "'" + std::string(key) + "' in " + where_ + " is empty");
    }
    return value->get();
}

std::optional<bool> Fields::boolean(std::string_view key) {
    const toml::node* node = get(key);
    if (node == nullptr) {
        return std::nullopt;
    }
    if (const auto* value = node->as_boolean()) {
        return value->get();
    }
    parser_.wrong_type(*node, key, where_, "true or false");
    return std::nullopt;
}

std::optional<std::int64_t> Fields::integer(std::string_view key) {
    const toml::node* node = get(key);
    if (node == nullptr) {
        return std::nullopt;
    }
    if (const auto* value = node->as_integer()) {
        return value->get();
    }
    parser_.wrong_type(*node, key, where_, "an integer");
    return std::nullopt;
}

void Fields::finish() {
    for (auto&& [key, node] : table_) {
        if (!known_.contains(key.str())) {
            parser_.error(key.source(),
                          "unknown key '" + std::string(key.str()) + "' in " + where_);
        }
    }
}

Manifest Parser::parse(std::string_view source) {
    Manifest manifest;
    manifest.file = file_;
    toml::table root;
    try {
        root = toml::parse(source);
    } catch (const toml::parse_error& e) {
        error(e.source(), "TOML syntax: " + std::string(e.description()));
        throw ManifestError(file_, std::move(diagnostics_));
    }

    Fields top(*this, root, "the manifest");
    const toml::node* header = top.get("extension");
    bool compatible = true;
    if (header == nullptr) {
        error(toml::source_region{}, "missing the [extension] table");
    } else if (const toml::table* table = header->as_table()) {
        compatible = parse_extension(*table, manifest);
    } else {
        wrong_type(*header, "extension", "the manifest", "a table, written as [extension]");
    }
    if (compatible) {
        manifest.contributes.owner = manifest.id;
        if (const toml::node* contributes = top.get("contributes")) {
            if (const toml::table* table = contributes->as_table()) {
                parse_contributes(*table, manifest.contributes);
            } else {
                wrong_type(*contributes, "contributes", "the manifest", "a table");
            }
        }
        top.finish();
    }
    if (!diagnostics_.empty()) {
        throw ManifestError(file_, std::move(diagnostics_));
    }
    return manifest;
}

bool Parser::parse_extension(const toml::table& table, Manifest& manifest) {
    Fields fields(*this, table, "[extension]");
    if (const toml::node* node = fields.require("id")) {
        if (const auto* value = node->as_string()) {
            manifest.id = value->get();
            if (manifest.id == model::core_owner) {
                error(node->source(), "'fem.core' is the platform itself, not a module");
            } else if (!is_valid_extension_id(manifest.id)) {
                error(node->source(),
                      "invalid module id '" + manifest.id +
                          "': expected two or more lowercase parts of ASCII letters, digits, '_' "
                          "and '-' joined by '.', each starting with a letter, like "
                          "'fem.mesher.gmsh'");
            }
        } else {
            wrong_type(*node, "id", fields.where(), "text");
        }
    }
    manifest.name = text(fields, "name", true);
    manifest.description = text(fields, "description", false);
    if (const auto version = fields.text("version", true)) {
        try {
            manifest.version = Version::parse(*version);
        } catch (const ExtensionError& e) {
            error(table.get("version")->source(), "invalid 'version': " + std::string(e.what()));
        }
    }
    bool engine_known = false;
    if (const auto engine = fields.text("engine", true)) {
        try {
            manifest.engine = VersionRange::parse(*engine);
            engine_known = true;
        } catch (const ExtensionError& e) {
            error(table.get("engine")->source(), "invalid 'engine': " + std::string(e.what()));
        }
    }
    if (engine_known && !manifest.engine.contains(engine_)) {
        // A module for another platform may use keys this one does not know: nothing else is read.
        return false;
    }

    const toml::node* kind = fields.get("kind");
    const toml::node* entry = fields.get("entry");
    has_code_ = kind != nullptr && entry != nullptr;
    if ((kind == nullptr) != (entry == nullptr)) {
        error(table.source(),
              "'kind' and 'entry' in [extension] go together: give both for a module with code, "
              "or neither for a module without code");
    }
    if (kind != nullptr) {
        if (const auto* value = kind->as_string()) {
            for (const ExtensionKind candidate :
                 {ExtensionKind::native, ExtensionKind::python, ExtensionKind::process}) {
                if (value->get() == to_string(candidate)) {
                    manifest.kind = candidate;
                }
            }
            if (!manifest.kind) {
                error(kind->source(),
                      "unknown kind '" + value->get() + "'; expected native, python or process");
            }
        } else {
            wrong_type(*kind, "kind", fields.where(), "text");
        }
    }
    if (entry != nullptr) {
        if (const auto* value = entry->as_string()) {
            manifest.entry = value->get();
        } else {
            wrong_type(*entry, "entry", fields.where(), "text");
        }
    }
    if (manifest.kind && entry != nullptr && entry->is_string()) {
        const std::string& value = manifest.entry;
        switch (*manifest.kind) {
            case ExtensionKind::native:
                if (!is_relative_path(value)) {
                    error(entry->source(),
                          "invalid entry '" + value +
                              "' of a native module: expected the path of the library inside the "
                              "module folder without the file extension, like 'bin/solver'");
                }
                break;
            case ExtensionKind::process:
                if (!is_relative_path(value)) {
                    error(entry->source(), "invalid entry '" + value +
                                               "' of a process module: expected the path of the "
                                               "executable inside the module folder");
                }
                break;
            case ExtensionKind::python:
                if (!is_python_entry(value)) {
                    error(entry->source(), "invalid entry '" + value +
                                               "' of a python module: expected "
                                               "package.module:function, like 'solver:activate'");
                }
                break;
        }
    }
    if (const toml::node* requirements = fields.get("requires")) {
        parse_requirements(*requirements, manifest);
    }
    if (const toml::node* activation = fields.get("activation")) {
        parse_activation(*activation, manifest);
    }
    fields.finish();
    return true;
}

void Parser::parse_requirements(const toml::node& node, Manifest& manifest) {
    const toml::array* array = node.as_array();
    if (array == nullptr) {
        wrong_type(node, "requires", "[extension]", "an array of text");
        return;
    }
    Seen seen;
    for (const toml::node& element : *array) {
        const auto* value = element.as_string();
        if (value == nullptr) {
            error(element.source(), "a requirement must be text like 'fem.other>=1.0', not " +
                                        std::string(type_name(element)));
            continue;
        }
        const std::string_view written = value->get();
        std::size_t end = 0;
        while (end < written.size() &&
               (is_lower(written[end]) || is_digit(written[end]) || written[end] == '.' ||
                written[end] == '_' || written[end] == '-')) {
            ++end;
        }
        Requirement requirement;
        requirement.id = std::string(written.substr(0, end));
        if (requirement.id == model::core_owner) {
            error(element.source(),
                  "'fem.core' is the platform: its version is given by 'engine', not 'requires'");
            continue;
        }
        if (!is_valid_extension_id(requirement.id)) {
            error(element.source(), "the requirement '" + std::string(written) +
                                        "' must start with a module id, like 'fem.other>=1.0'");
            continue;
        }
        if (requirement.id == manifest.id) {
            error(element.source(), "a module cannot require itself");
            continue;
        }
        const std::string_view range = trim(written.substr(end));
        if (!range.empty()) {
            try {
                requirement.range = VersionRange::parse(range);
            } catch (const ExtensionError& e) {
                error(element.source(), "invalid version range in the requirement '" +
                                            std::string(written) + "': " + e.what());
                continue;
            }
        }
        unique(seen, requirement.id, &element, "requirement of module");
        manifest.requirements.push_back(std::move(requirement));
    }
}

void Parser::parse_activation(const toml::node& node, Manifest& manifest) {
    const toml::array* array = node.as_array();
    if (array == nullptr) {
        wrong_type(node, "activation", "[extension]", "an array of text");
        return;
    }
    if (!has_code_ && !array->empty()) {
        error(node.source(),
              "activation events load code, but the module has no 'kind' and 'entry'");
    }
    for (const toml::node& element : *array) {
        const auto* value = element.as_string();
        std::optional<ActivationEvent> event;
        if (value != nullptr) {
            event = ActivationEvent::parse(value->get());
        }
        if (!event) {
            error(element.source(),
                  "unknown activation event" +
                      (value != nullptr ? " '" + value->get() + "'" : std::string()) +
                      "; expected onStartup, onCommand:<command id>, onOpenFile:<file name> or "
                      "onSolverRequested:<solver id>, where '*' matches any text and '?' one "
                      "character");
            continue;
        }
        manifest.activation.push_back(std::move(*event));
    }
}

void Parser::each_table(Fields& contributes, std::string_view key,
                        const std::function<void(const toml::table&, const std::string&)>& read) {
    const toml::node* node = contributes.get(key);
    if (node == nullptr) {
        return;
    }
    const std::string where = "[[contributes." + std::string(key) + "]]";
    const toml::array* array = node->as_array();
    if (array == nullptr) {
        error(node->source(), "'contributes." + std::string(key) +
                                  "' must be an array of tables, written as " + where);
        return;
    }
    for (const toml::node& element : *array) {
        if (const toml::table* table = element.as_table()) {
            read(*table, where);
        } else {
            error(element.source(), "an entry of 'contributes." + std::string(key) +
                                        "' must be a table, not " +
                                        std::string(type_name(element)));
        }
    }
}

std::string Parser::id(Fields& fields, std::string_view key, bool (*valid)(std::string_view),
                       std::string_view rule, bool required) {
    const toml::node* node = required ? fields.require(key) : fields.get(key);
    if (node == nullptr) {
        return {};
    }
    const auto* value = node->as_string();
    if (value == nullptr) {
        wrong_type(*node, key, fields.where(), "text");
        return {};
    }
    if (!valid(value->get())) {
        error(node->source(), "invalid " + std::string(key) + " '" + value->get() + "' in " +
                                  fields.where() + ": " + std::string(rule));
    }
    return value->get();
}

std::string Parser::path(Fields& fields, std::string_view key, bool required) {
    return id(fields, key, is_relative_path, path_rule, required);
}

std::optional<context::WhenExpression> Parser::when(Fields& fields, std::string_view key) {
    const toml::node* node = fields.get(key);
    if (node == nullptr) {
        return std::nullopt;
    }
    const auto* value = node->as_string();
    if (value == nullptr) {
        wrong_type(*node, key, fields.where(), "text");
        return std::nullopt;
    }
    try {
        return context::WhenExpression::parse(value->get());
    } catch (const context::WhenSyntaxError& e) {
        error(node->source(),
              "invalid '" + std::string(key) + "' in " + fields.where() + ": " + e.what());
        return std::nullopt;
    }
}

void Parser::unique(Seen& seen, const std::string& value, const toml::node* node,
                    std::string_view what) {
    if (value.empty() || node == nullptr) {
        return;
    }
    const auto [it, inserted] = seen.emplace(value, node->source().begin.line);
    if (!inserted) {
        error(node->source(), "duplicate " + std::string(what) + " '" + value +
                                  "' (first at line " + std::to_string(it->second) + ")");
    }
}

std::optional<command::Value> Parser::to_value(const toml::node& node) {
    if (const auto* value = node.as_string()) {
        return command::Value(value->get());
    }
    if (const auto* value = node.as_integer()) {
        return command::Value(value->get());
    }
    if (const auto* value = node.as_floating_point()) {
        if (!std::isfinite(value->get())) {
            error(node.source(), "infinite and NaN numbers are not allowed");
            return std::nullopt;
        }
        return command::Value(value->get());
    }
    if (const auto* value = node.as_boolean()) {
        return command::Value(value->get());
    }
    if (const auto* array = node.as_array()) {
        command::Value::List list;
        for (const toml::node& element : *array) {
            std::optional<command::Value> item = to_value(element);
            if (!item) {
                return std::nullopt;
            }
            list.push_back(std::move(*item));
        }
        return command::Value(std::move(list));
    }
    if (const auto* table = node.as_table()) {
        command::Value::Dict dict;
        for (auto&& [key, element] : *table) {
            std::optional<command::Value> item = to_value(element);
            if (!item) {
                return std::nullopt;
            }
            dict.emplace(std::string(key.str()), std::move(*item));
        }
        return command::Value(std::move(dict));
    }
    error(node.source(), "dates and times are not allowed here");
    return std::nullopt;
}

std::optional<nlohmann::json> Parser::to_json(const toml::node& node) {
    if (const auto* value = node.as_string()) {
        return nlohmann::json(value->get());
    }
    if (const auto* value = node.as_integer()) {
        return nlohmann::json(value->get());
    }
    if (const auto* value = node.as_floating_point()) {
        if (!std::isfinite(value->get())) {
            error(node.source(), "infinite and NaN numbers are not allowed");
            return std::nullopt;
        }
        return nlohmann::json(value->get());
    }
    if (const auto* value = node.as_boolean()) {
        return nlohmann::json(value->get());
    }
    if (const auto* array = node.as_array()) {
        nlohmann::json list = nlohmann::json::array();
        for (const toml::node& element : *array) {
            std::optional<nlohmann::json> item = to_json(element);
            if (!item) {
                return std::nullopt;
            }
            list.push_back(std::move(*item));
        }
        return list;
    }
    if (const auto* table = node.as_table()) {
        nlohmann::json object = nlohmann::json::object();
        for (auto&& [key, element] : *table) {
            std::optional<nlohmann::json> item = to_json(element);
            if (!item) {
                return std::nullopt;
            }
            object[std::string(key.str())] = std::move(*item);
        }
        return object;
    }
    error(node.source(), "dates and times are not allowed here");
    return std::nullopt;
}

std::vector<command::ArgSpec> Parser::parse_args(const toml::node& node,
                                                 const std::string& command) {
    std::vector<command::ArgSpec> args;
    const toml::array* array = node.as_array();
    if (array == nullptr) {
        wrong_type(node, "args", "[[contributes.commands]]", "an array of tables");
        return args;
    }
    Seen names;
    for (const toml::node& element : *array) {
        const toml::table* table = element.as_table();
        if (table == nullptr) {
            error(element.source(), "an argument of command '" + command +
                                        "' must be a table like { name = \"job\", type = "
                                        "\"string\" }, not " +
                                        std::string(type_name(element)));
            continue;
        }
        Fields fields(*this, *table, "an argument of command '" + command + "'");
        command::ArgSpec arg;
        bool valid = true;
        if (const auto name = fields.text("name", true)) {
            arg.name = *name;
            if (!command::is_valid_argument_name(arg.name)) {
                error(table->get("name")->source(),
                      "invalid argument name '" + arg.name + "' of command '" + command +
                          "': expected a Python keyword argument of lowercase ASCII letters, "
                          "digits and '_', not starting with a digit and not a Python keyword");
                valid = false;
            }
            unique(names, arg.name, table->get("name"), "argument");
        } else {
            valid = false;
        }
        if (const auto type = fields.text("type", false)) {
            if (const auto parsed = command::parse_arg_type(*type)) {
                arg.type = *parsed;
            } else {
                error(table->get("type")->source(),
                      "unknown argument type '" + *type +
                          "'; expected boolean, integer, number, string, path, object, list, dict "
                          "or any");
                valid = false;
            }
        } else if (table->contains("type")) {
            valid = false;
        }
        if (const toml::node* default_value = fields.get("default")) {
            arg.default_value = to_value(*default_value);
            valid = valid && arg.default_value.has_value();
        }
        const std::optional<bool> required = fields.boolean("required");
        arg.required = required.value_or(!arg.default_value.has_value());
        fields.finish();
        if (valid) {
            // The rules of the Command Bus, so the module registers without surprises.
            command::CommandSpec probe{
                .id = command::is_valid_command_id(command) ? command : "manifest.command",
                .owner = "manifest",
                .args = {arg}};
            try {
                command::check_spec(probe);
                arg = std::move(probe.args.front());
            } catch (const command::CommandError& e) {
                error(table->source(), e.what());
            }
        }
        args.push_back(std::move(arg));
    }
    return args;
}

void Parser::parse_settings(const toml::node& node, Contributions& out) {
    using Type = SettingContribution::Type;
    const toml::table* table = node.as_table();
    if (table == nullptr) {
        wrong_type(node, "settings", "[contributes]", "a table, written as [contributes.settings]");
        return;
    }
    for (auto&& [key, value] : *table) {
        const std::string name(key.str());
        if (!is_contribution_id(name)) {
            error(key.source(),
                  "invalid setting key '" + name + "': " + std::string(contribution_id_rule));
        }
        const toml::table* entry = value.as_table();
        if (entry == nullptr) {
            error(value.source(), "setting '" + name +
                                      "' must be a table like { type = \"boolean\", default = "
                                      "false }, not " +
                                      std::string(type_name(value)));
            continue;
        }
        Fields fields(*this, *entry, "setting '" + name + "'");
        SettingContribution setting;
        setting.owner = out.owner;
        setting.key = name;
        std::optional<Type> type;
        if (const auto type_name_text = fields.text("type", true)) {
            for (const Type candidate :
                 {Type::boolean, Type::integer, Type::number, Type::string, Type::path}) {
                if (*type_name_text == to_string(candidate)) {
                    type = candidate;
                }
            }
            if (!type) {
                error(entry->get("type")->source(),
                      "unknown setting type '" + *type_name_text +
                          "'; expected boolean, integer, number, string or path");
            }
        }
        setting.type = type.value_or(Type::string);
        if (const auto scope = fields.text("scope", false)) {
            if (*scope == "user") {
                setting.scope = SettingContribution::Scope::user;
            } else if (*scope != "project") {
                error(entry->get("scope")->source(),
                      "unknown setting scope '" + *scope + "'; expected project or user");
            }
        }
        const auto fits = [&type](const toml::node& item) {
            switch (*type) {
                case Type::boolean:
                    return item.is_boolean();
                case Type::integer:
                    return item.is_integer();
                case Type::number:
                    return item.is_integer() || item.is_floating_point();
                case Type::string:
                case Type::path:
                    return item.is_string();
            }
            return false;
        };
        const auto describe_type = [&type] { return std::string(to_string(*type)); };
        const toml::node* default_value = fields.get("default");
        if (default_value != nullptr && type && !fits(*default_value)) {
            error(default_value->source(), "the default of setting '" + name + "' must be " +
                                               describe_type() + ", not " +
                                               std::string(type_name(*default_value)));
        }
        setting.description = text(fields, "description", false);
        if (const toml::node* values = fields.get("enum")) {
            const toml::array* array = values->as_array();
            if (array == nullptr || array->empty()) {
                error(values->source(), "'enum' of setting '" + name +
                                            "' must be a non-empty array of allowed values");
            } else if (type && (*type == Type::boolean || *type == Type::path)) {
                error(values->source(),
                      "'enum' is only for settings of type string, integer or number");
            } else if (type) {
                bool listed = default_value == nullptr;
                for (const toml::node& item : *array) {
                    if (!fits(item)) {
                        error(item.source(), "a value of 'enum' of setting '" + name +
                                                 "' must be " + describe_type() + ", not " +
                                                 std::string(type_name(item)));
                    }
                    listed =
                        listed || (default_value != nullptr && same_value(item, *default_value));
                }
                if (!listed) {
                    error(default_value->source(), "the default of setting '" + name +
                                                       "' is not one of the values of 'enum'");
                }
            }
        }
        for (const std::string_view bound : {"minimum", "maximum"}) {
            const toml::node* limit = fields.get(bound);
            if (limit == nullptr) {
                continue;
            }
            if (type && *type != Type::integer && *type != Type::number) {
                error(limit->source(), "'" + std::string(bound) +
                                           "' is only for settings of type integer or number");
            } else if (!limit->is_integer() && !limit->is_floating_point()) {
                wrong_type(*limit, bound, fields.where(), "a number");
            } else if (type && default_value != nullptr && fits(*default_value)) {
                const double limit_value = limit->value<double>().value_or(0.0);
                const double default_number = default_value->value<double>().value_or(0.0);
                if (bound == "minimum" ? default_number < limit_value
                                       : default_number > limit_value) {
                    error(default_value->source(), "the default of setting '" + name +
                                                       "' is outside its '" + std::string(bound) +
                                                       "'");
                }
            }
        }
        if (default_value != nullptr) {
            setting.default_value = to_json(*default_value).value_or(nullptr);
        }
        setting.schema = to_json(*entry).value_or(nlohmann::json::object());
        fields.finish();
        out.settings.push_back(std::move(setting));
    }
}

void Parser::parse_contributes(const toml::table& table, Contributions& out) {
    Fields contributes(*this, table, "[contributes]");
    const std::string& owner = out.owner;

    Seen command_ids;
    bool reported_commands_without_code = false;
    each_table(contributes, "commands", [&](const toml::table& entry, const std::string& where) {
        if (!has_code_ && !reported_commands_without_code) {
            error(entry.source(), "commands need code, but the module has no 'kind' and 'entry'");
            reported_commands_without_code = true;
        }
        Fields fields(*this, entry, where);
        CommandContribution command;
        command.owner = owner;
        command.id = id(fields, "id", command::is_valid_command_id, command_id_rule);
        unique(command_ids, command.id, entry.get("id"), "command id");
        command.title = text(fields, "title", true);
        command.category = text(fields, "category", false);
        command.icon = path(fields, "icon", false);
        command.enablement = when(fields, "enablement");
        if (const toml::node* args = fields.get("args")) {
            command.args = parse_args(*args, command.id);
        }
        fields.finish();
        out.commands.push_back(std::move(command));
    });

    Seen stage_ids;
    each_table(contributes, "stages", [&](const toml::table& entry, const std::string& where) {
        Fields fields(*this, entry, where);
        StageContribution stage;
        stage.owner = owner;
        stage.id = id(fields, "id", is_contribution_id, contribution_id_rule);
        unique(stage_ids, stage.id, entry.get("id"), "stage id");
        stage.title = text(fields, "title", true);
        stage.icon = path(fields, "icon", false);
        if (const auto slot = fields.text("slot", true)) {
            const std::size_t colon = slot->find(':');
            const std::string placement = slot->substr(0, colon);
            stage.anchor = colon == std::string::npos ? std::string() : slot->substr(colon + 1);
            if ((placement != "before" && placement != "after") ||
                !is_contribution_id(stage.anchor)) {
                error(entry.get("slot")->source(),
                      "invalid slot '" + *slot +
                          "': expected before:<stage> or after:<stage>, like 'after:results'");
            }
            stage.placement = placement == "before" ? StageContribution::Placement::before
                                                    : StageContribution::Placement::after;
        }
        stage.when = when(fields);
        fields.finish();
        out.stages.push_back(std::move(stage));
    });

    Seen tree_types;
    each_table(contributes, "treeNodeTypes",
               [&](const toml::table& entry, const std::string& where) {
                   Fields fields(*this, entry, where);
                   TreeNodeTypeContribution node;
                   node.owner = owner;
                   node.type = id(fields, "type", is_object_type, object_type_rule);
                   unique(tree_types, node.type, entry.get("type"), "tree node type");
                   node.title = text(fields, "title", true);
                   node.icon = path(fields, "icon", false);
                   fields.finish();
                   out.tree_node_types.push_back(std::move(node));
               });

    const auto read_menu = [&](std::vector<MenuContribution>& menus) {
        return [&](const toml::table& entry, const std::string& where) {
            Fields fields(*this, entry, where);
            MenuContribution menu;
            menu.owner = owner;
            menu.location = id(fields, "location", is_menu_location,
                               "expected parts of ASCII letters, digits, '_' and '-' joined by "
                               "'/', each starting with a letter, like 'solve/run'");
            menu.command = id(fields, "command", command::is_valid_command_id, command_id_rule);
            menu.when = when(fields);
            menu.group = text(fields, "group", false);
            fields.finish();
            menus.push_back(std::move(menu));
        };
    };
    each_table(contributes, "menus", read_menu(out.menus));
    each_table(contributes, "contextMenus", read_menu(out.context_menus));

    Seen submenu_ids;
    each_table(contributes, "submenus", [&](const toml::table& entry, const std::string& where) {
        Fields fields(*this, entry, where);
        SubmenuContribution submenu;
        submenu.owner = owner;
        submenu.id = id(fields, "id", is_menu_id,
                        "expected ASCII letters, digits, '_' and '-', starting with a letter: one "
                        "part of a menu location, like 'optimize'");
        unique(submenu_ids, submenu.id, entry.get("id"), "submenu id");
        submenu.title = text(fields, "title", true);
        fields.finish();
        out.submenus.push_back(std::move(submenu));
    });

    each_table(
        contributes, "contextToolbar", [&](const toml::table& entry, const std::string& where) {
            Fields fields(*this, entry, where);
            ContextToolbarContribution item;
            item.owner = owner;
            item.command = id(fields, "command", command::is_valid_command_id, command_id_rule);
            item.when = when(fields);
            item.group = text(fields, "group", false);
            fields.finish();
            out.context_toolbar.push_back(std::move(item));
        });

    Seen schema_types;
    each_table(contributes, "propertySchemas",
               [&](const toml::table& entry, const std::string& where) {
                   Fields fields(*this, entry, where);
                   PropertySchemaContribution schema;
                   schema.owner = owner;
                   schema.type = id(fields, "type", is_object_type, object_type_rule);
                   unique(schema_types, schema.type, entry.get("type"), "property schema type");
                   if (const auto version = fields.integer("version")) {
                       if (*version < 1 || *version > std::numeric_limits<std::uint32_t>::max()) {
                           error(entry.get("version")->source(),
                                 "'version' of a property schema must be from 1 to 4294967295");
                       } else {
                           schema.version = static_cast<std::uint32_t>(*version);
                       }
                   }
                   if (const toml::node* value = fields.require("schema")) {
                       if (value->is_table()) {
                           schema.schema = to_json(*value).value_or(nlohmann::json::object());
                       } else {
                           wrong_type(*value, "schema", where, "a table with a JSON Schema");
                       }
                   }
                   fields.finish();
                   out.property_schemas.push_back(std::move(schema));
               });

    Seen format_ids;
    each_table(contributes, "fileFormats", [&](const toml::table& entry, const std::string& where) {
        using Role = FileFormatContribution::Role;
        Fields fields(*this, entry, where);
        FileFormatContribution format;
        format.owner = owner;
        format.id = id(fields, "id", is_contribution_id, contribution_id_rule);
        unique(format_ids, format.id, entry.get("id"), "file format id");
        format.title = text(fields, "title", false);
        if (const toml::node* extensions = fields.require("extensions")) {
            const toml::array* array = extensions->as_array();
            if (array == nullptr || array->empty()) {
                error(extensions->source(),
                      "'extensions' in " + where + " must be a non-empty array like [\".res\"]");
            } else {
                for (const toml::node& item : *array) {
                    const auto* value = item.as_string();
                    const std::string extension = value != nullptr ? value->get() : std::string();
                    const bool valid = extension.size() >= 2 && extension[0] == '.' &&
                                       is_segmented(extension.substr(1), '.', 1, unlimited,
                                                    is_id_char, is_id_char);
                    if (!valid) {
                        error(item.source(),
                              "invalid file extension in " + where +
                                  ": expected a dot and ASCII letters, digits, '_' and '-', like "
                                  "'.res'");
                    }
                    format.extensions.push_back(extension);
                }
            }
        }
        if (const auto role = fields.text("role", true)) {
            bool known = false;
            for (const Role candidate :
                 {Role::model_import, Role::model_export, Role::mesh_import, Role::mesh_export,
                  Role::results_import, Role::results_export}) {
                if (*role == to_string(candidate)) {
                    format.role = candidate;
                    known = true;
                }
            }
            if (!known) {
                error(entry.get("role")->source(),
                      "unknown role '" + *role +
                          "'; expected model-import, model-export, mesh-import, mesh-export, "
                          "results-import or results-export");
            }
        }
        fields.finish();
        out.file_formats.push_back(std::move(format));
    });

    Seen solver_ids;
    each_table(contributes, "solvers", [&](const toml::table& entry, const std::string& where) {
        Fields fields(*this, entry, where);
        SolverContribution solver;
        solver.owner = owner;
        solver.id = id(fields, "id", is_single_id, single_id_rule);
        unique(solver_ids, solver.id, entry.get("id"), "solver id");
        solver.label = text(fields, "label", true);
        if (const toml::node* analyses = fields.get("analyses")) {
            const toml::array* array = analyses->as_array();
            if (array == nullptr) {
                wrong_type(*analyses, "analyses", where, "an array of text");
            } else {
                for (const toml::node& item : *array) {
                    const auto* value = item.as_string();
                    if (value == nullptr || !is_single_id(value->get())) {
                        error(item.source(),
                              "invalid analysis in " + where + ": " + std::string(single_id_rule));
                        continue;
                    }
                    solver.analyses.push_back(value->get());
                }
            }
        }
        fields.finish();
        out.solvers.push_back(std::move(solver));
    });

    Seen mesher_ids;
    std::vector<std::pair<std::string, toml::source_region>> mesher_parameters;
    each_table(contributes, "meshers", [&](const toml::table& entry, const std::string& where) {
        Fields fields(*this, entry, where);
        MesherContribution mesher;
        mesher.owner = owner;
        mesher.id = id(fields, "id", is_single_id, single_id_rule);
        unique(mesher_ids, mesher.id, entry.get("id"), "mesher id");
        mesher.label = text(fields, "label", true);
        mesher.parameters = id(fields, "parameters", is_object_type, object_type_rule, false);
        if (!mesher.parameters.empty()) {
            mesher_parameters.emplace_back(mesher.parameters, entry.get("parameters")->source());
        }
        fields.finish();
        out.meshers.push_back(std::move(mesher));
    });

    each_table(contributes, "activityBar", [&](const toml::table& entry, const std::string& where) {
        if (!out.activity_bar.empty()) {
            error(entry.source(), "a module has at most one entry in " + where);
        }
        Fields fields(*this, entry, where);
        ActivityBarContribution item;
        item.owner = owner;
        item.id = id(fields, "id", is_contribution_id, contribution_id_rule);
        item.title = text(fields, "title", true);
        item.icon = path(fields, "icon", false);
        fields.finish();
        out.activity_bar.push_back(std::move(item));
    });

    Seen tab_ids;
    each_table(contributes, "bottomPanelTabs",
               [&](const toml::table& entry, const std::string& where) {
                   Fields fields(*this, entry, where);
                   BottomPanelTabContribution tab;
                   tab.owner = owner;
                   tab.id = id(fields, "id", is_contribution_id, contribution_id_rule);
                   unique(tab_ids, tab.id, entry.get("id"), "bottom panel tab id");
                   tab.title = text(fields, "title", true);
                   tab.when = when(fields);
                   fields.finish();
                   out.bottom_panel_tabs.push_back(std::move(tab));
               });

    Seen layer_ids;
    each_table(contributes, "viewportLayers",
               [&](const toml::table& entry, const std::string& where) {
                   Fields fields(*this, entry, where);
                   ViewportLayerContribution layer;
                   layer.owner = owner;
                   layer.id = id(fields, "id", is_contribution_id, contribution_id_rule);
                   unique(layer_ids, layer.id, entry.get("id"), "viewport layer id");
                   layer.title = text(fields, "title", true);
                   layer.when = when(fields);
                   fields.finish();
                   out.viewport_layers.push_back(std::move(layer));
               });

    Seen filter_ids;
    each_table(contributes, "resultFilters",
               [&](const toml::table& entry, const std::string& where) {
                   Fields fields(*this, entry, where);
                   ResultFilterContribution filter;
                   filter.owner = owner;
                   filter.id = id(fields, "id", is_contribution_id, contribution_id_rule);
                   unique(filter_ids, filter.id, entry.get("id"), "result filter id");
                   filter.title = text(fields, "title", true);
                   fields.finish();
                   out.result_filters.push_back(std::move(filter));
               });

    if (const toml::node* settings = contributes.get("settings")) {
        parse_settings(*settings, out);
    }

    each_table(contributes, "keybindings", [&](const toml::table& entry, const std::string& where) {
        Fields fields(*this, entry, where);
        KeybindingContribution binding;
        binding.owner = owner;
        binding.command = id(fields, "command", command::is_valid_command_id, command_id_rule);
        if (const auto key = fields.text("key", true)) {
            binding.key = *key;
            if (const auto problem = check_shortcut(*key)) {
                error(entry.get("key")->source(),
                      "invalid key '" + *key + "' in " + where + ": " + *problem +
                          "; write modifiers and a key joined by '+', like "
                          "'Mod+Shift+R'");
            }
        }
        binding.when = when(fields);
        fields.finish();
        out.keybindings.push_back(std::move(binding));
    });

    Seen icon_ids;
    each_table(contributes, "icons", [&](const toml::table& entry, const std::string& where) {
        Fields fields(*this, entry, where);
        IconContribution icon;
        icon.owner = owner;
        icon.id = id(fields, "id", is_contribution_id, contribution_id_rule);
        unique(icon_ids, icon.id, entry.get("id"), "icon id");
        icon.path = path(fields, "path", true);
        fields.finish();
        out.icons.push_back(std::move(icon));
    });

    Seen languages;
    each_table(contributes, "localization",
               [&](const toml::table& entry, const std::string& where) {
                   Fields fields(*this, entry, where);
                   LocalizationContribution catalog;
                   catalog.owner = owner;
                   catalog.language = id(fields, "language", is_language,
                                         "expected a language like 'ru', 'en' or 'pt-BR'");
                   unique(languages, catalog.language, entry.get("language"), "language");
                   catalog.path = path(fields, "path", true);
                   fields.finish();
                   out.localization.push_back(std::move(catalog));
               });

    Seen key_names;
    each_table(contributes, "contextKeys", [&](const toml::table& entry, const std::string& where) {
        using Kind = context::ContextValue::Kind;
        Fields fields(*this, entry, where);
        ContextKeyContribution key;
        key.owner = owner;
        key.name = id(fields, "name", context::is_valid_key,
                      "expected names of ASCII letters, digits, '_' and '-' joined by '.', each "
                      "starting with a letter or '_', like 'solver.ready'");
        unique(key_names, key.name, entry.get("name"), "context key");
        std::optional<Kind> type;
        if (const auto type_text = fields.text("type", true)) {
            for (const Kind candidate : {Kind::boolean, Kind::number, Kind::string, Kind::list}) {
                if (*type_text == context::to_string(candidate)) {
                    type = candidate;
                }
            }
            if (!type) {
                error(entry.get("type")->source(), "unknown context key type '" + *type_text +
                                                       "'; expected boolean, number, string or "
                                                       "list");
            }
        }
        key.type = type.value_or(Kind::boolean);
        if (const toml::node* value = fields.get("default"); value != nullptr && type) {
            bool fits = false;
            switch (*type) {
                case Kind::boolean:
                    if (const auto* boolean = value->as_boolean()) {
                        key.initial = boolean->get();
                        fits = true;
                    }
                    break;
                case Kind::number:
                    if (const auto number = value->value<double>();
                        number && (value->is_integer() || value->is_floating_point()) &&
                        std::isfinite(*number)) {
                        key.initial = *number;
                        fits = true;
                    }
                    break;
                case Kind::string:
                    if (const auto* text_value = value->as_string()) {
                        key.initial = text_value->get();
                        fits = true;
                    }
                    break;
                case Kind::list:
                    if (const toml::array* array = value->as_array()) {
                        context::ContextValue::List list;
                        fits = true;
                        for (const toml::node& item : *array) {
                            const auto* text_value = item.as_string();
                            fits = fits && text_value != nullptr;
                            if (text_value != nullptr) {
                                list.push_back(text_value->get());
                            }
                        }
                        key.initial = std::move(list);
                    }
                    break;
                case Kind::none:
                    break;
            }
            if (!fits) {
                error(value->source(),
                      "the default of context key '" + key.name + "' must be " +
                          (*type == Kind::list ? std::string("an array of text")
                                               : std::string(context::to_string(*type))) +
                          ", not " + std::string(type_name(*value)));
                key.initial = {};
            }
        }
        fields.finish();
        out.context_keys.push_back(std::move(key));
    });

    for (const auto& [type, where] : mesher_parameters) {
        if (!schema_types.contains(type)) {
            error(where, "the parameters '" + type +
                             "' of a mesher need a [[contributes.propertySchemas]] entry of the "
                             "module with type = '" +
                             type + "'");
        }
    }
    contributes.finish();
}

}  // namespace

// --- Public functions ----------------------------------------------------------------------------

std::string_view to_string(ExtensionKind kind) noexcept {
    switch (kind) {
        case ExtensionKind::native:
            return "native";
        case ExtensionKind::python:
            return "python";
        case ExtensionKind::process:
            return "process";
    }
    return "unknown";
}

bool is_valid_extension_id(std::string_view id) noexcept {
    return id != model::core_owner && is_segmented(id, '.', 2, unlimited, is_lower, [](char c) {
               return is_lower(c) || is_digit(c) || c == '_' || c == '-';
           });
}

ActivationEvent ActivationEvent::open_file(const std::filesystem::path& file) {
    const std::u8string name = file.filename().u8string();
    return {Trigger::open_file, std::string(name.begin(), name.end())};
}

std::optional<ActivationEvent> ActivationEvent::parse(std::string_view text) {
    if (text == "onStartup") {
        return startup();
    }
    struct Form {
        std::string_view prefix;
        Trigger trigger;
    };
    for (const Form form :
         {Form{"onCommand:", Trigger::command}, Form{"onOpenFile:", Trigger::open_file},
          Form{"onSolverRequested:", Trigger::solver_requested}}) {
        if (!text.starts_with(form.prefix)) {
            continue;
        }
        const std::string_view value = text.substr(form.prefix.size());
        if (value.empty()) {
            return std::nullopt;
        }
        for (const char c : value) {
            const bool pattern = is_id_char(c) || c == '*' || c == '?';
            const bool allowed =
                form.trigger == Trigger::command ? pattern || c == '.'
                : form.trigger == Trigger::solver_requested
                    ? pattern
                    : c != '/' && c != '\\' && static_cast<unsigned char>(c) >= 0x20;
            if (!allowed) {
                return std::nullopt;
            }
        }
        return ActivationEvent{form.trigger, std::string(value)};
    }
    return std::nullopt;
}

std::string ActivationEvent::to_string() const {
    switch (trigger) {
        case Trigger::startup:
            return "onStartup";
        case Trigger::command:
            return "onCommand:" + value;
        case Trigger::open_file:
            return "onOpenFile:" + value;
        case Trigger::solver_requested:
            return "onSolverRequested:" + value;
    }
    return {};
}

bool ActivationEvent::matches(const ActivationEvent& fired) const {
    if (trigger != fired.trigger) {
        return false;
    }
    return trigger == Trigger::startup ||
           glob_match(value, fired.value, trigger == Trigger::open_file);
}

Manifest parse_manifest(std::string_view text, const Version& engine,
                        const std::filesystem::path& file) {
    return Parser(engine, file).parse(text);
}

Manifest load_manifest(const std::filesystem::path& file, const Version& engine) {
    return parse_manifest(detail::read_file(file), engine, file);
}

}  // namespace fem::extension
