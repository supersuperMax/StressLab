#include "inp/import.h"

#include "inp/deck.h"
#include "inp/elements.h"
#include "inp/numbers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fem::inp {

namespace {

using Json = nlohmann::json;

// --- Places of nodes and elements ----------------------------------------------------------------
//
// Every node and element lives in a container: the model itself (a file without parts), a part or
// the assembly (reference points). A set names nodes or elements by place: the model, the
// assembly, an instance (by number), or "self" for a set of a part, which every instance of the
// part has. Each place with nodes becomes one mesh of the Model IR.

constexpr int place_model = -1;
constexpr int place_assembly = -2;
constexpr int place_self = -3;
constexpr int any_place = -100;

/// Positions of labels: a table when the labels are dense enough (the usual 1..n), a hash map
/// otherwise. Meshes of millions of nodes are looked up here for every element.
class LabelIndex {
public:
    /// Indexes `labels`; the position of the first label given twice, if any.
    std::optional<std::size_t> build(std::span<const std::int64_t> labels) {
        table_.clear();
        map_.clear();
        if (labels.empty()) {
            dense_ = true;
            return std::nullopt;
        }
        const auto [low, high] = std::minmax_element(labels.begin(), labels.end());
        first_ = *low;
        const auto range = static_cast<std::uint64_t>(*high) - static_cast<std::uint64_t>(*low);
        dense_ = range < 4 * static_cast<std::uint64_t>(labels.size()) + 1024;
        if (dense_) {
            table_.assign(static_cast<std::size_t>(range) + 1, -1);
            for (std::size_t i = 0; i < labels.size(); ++i) {
                std::int32_t& slot = table_[static_cast<std::size_t>(labels[i] - first_)];
                if (slot >= 0) {
                    return i;
                }
                slot = static_cast<std::int32_t>(i);
            }
        } else {
            map_.reserve(labels.size());
            for (std::size_t i = 0; i < labels.size(); ++i) {
                if (!map_.try_emplace(labels[i], static_cast<std::int32_t>(i)).second) {
                    return i;
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::int32_t> find(std::int64_t label) const {
        if (dense_) {
            if (label < first_ || static_cast<std::uint64_t>(label - first_) >= table_.size()) {
                return std::nullopt;
            }
            const std::int32_t index = table_[static_cast<std::size_t>(label - first_)];
            return index >= 0 ? std::optional<std::int32_t>(index) : std::nullopt;
        }
        const auto it = map_.find(label);
        return it != map_.end() ? std::optional<std::int32_t>(it->second) : std::nullopt;
    }

private:
    bool dense_ = true;
    std::int64_t first_ = 0;
    std::vector<std::int32_t> table_;
    std::unordered_map<std::int64_t, std::int32_t> map_;
};

struct ElementGroup {
    ElementKind kind;
    std::vector<std::int64_t> labels;
    std::vector<std::int64_t> nodes;     // Node labels, kind.nodes per element.
    std::vector<const DataLine*> lines;  // First line of each element.
};

struct Container {
    std::string name;  // Part or assembly name; empty for the model.
    std::vector<std::int64_t> node_ids;
    std::vector<double> x, y, z;
    std::vector<const DataLine*> node_lines;
    std::vector<ElementGroup> groups;
    std::array<std::map<std::string, std::size_t>, 3> sets;  // By kind: upper name -> set.

    // Made by `finish`.
    bool finished = false;
    std::vector<MeshBlock> blocks;
    std::vector<ElementKind> block_kinds;
    std::vector<std::int32_t> block_offsets;  // blocks.size() + 1 entries.
    LabelIndex nodes;                         // Label -> index.
    LabelIndex elements;                      // Label -> global index.

    [[nodiscard]] bool has_geometry() const { return !node_ids.empty() || !groups.empty(); }
};

struct SetDef {
    SetKind kind = SetKind::node;
    std::string name;
    std::size_t container = 0;                                       // Whose names it is among.
    std::map<int, std::vector<std::int64_t>> labels;                 // Node or element sets.
    std::map<int, std::vector<std::pair<std::int64_t, int>>> faces;  // Surfaces: element, face.
    /// Kept in passthrough (a node-based surface, faces of shells): what uses it is kept too.
    bool unsupported = false;
    bool implicit = false;  // Made by the importer for labels written in a load.
    std::size_t file = 0;
    std::int64_t line = 0;
};

/// A set as something refers to it; `place` limits a set of a part to one instance.
struct SetUse {
    std::size_t set = 0;
    int place = any_place;
};

struct Instance {
    std::string name;
    std::size_t part = 0;
    std::array<double, 3> translation{};
    bool rotated = false;
    std::array<double, 3> a{}, b{};
    double angle = 0.0;  // Degrees.
};

enum class ParentKind : std::uint8_t { root, input, step, material };

struct Parent {
    ParentKind kind = ParentKind::root;
    std::size_t index = 0;
};

/// An object of `fem.model.structural` waiting for its sets to become objects.
struct Pending {
    std::string type;
    std::string name;
    Json properties = Json::object();
    Parent parent;
    std::vector<SetUse> sets;
    std::optional<std::size_t> material;
    std::size_t order = 0;  // Index of the keyword, for the order among siblings.
};

struct PendingSection {
    std::string type;
    Json properties = Json::object();
    std::string material;  // Resolved at the end: Abaqus/CAE writes materials after the parts.
    SetUse set;
    Family family = Family::solid;
    std::size_t keyword = 0;
    Json context = Json::object();
};

struct Passthrough {
    std::string keyword;  // "*CONTACT CONTROLS"
    std::string text;
    std::size_t file = 0;
    std::int64_t line = 0;
    std::size_t index = 0;
    Parent parent{ParentKind::input, 0};
    Json context = Json::object();  // part, assembly, instance, material, step.
};

struct Material {
    std::string name;
    Json properties = Json::object();
};

struct Step {
    std::string name;
    Json properties = Json::object();
    std::size_t index = 0;
};

/// A set of the Model IR on one mesh.
struct OutSet {
    SetKind kind = SetKind::node;
    std::string name;
    std::vector<std::int32_t> indices;
    ItemRef item = no_item;
    bool own = false;  // A set of the part of an instance, not one of the assembly.
    /// Labels the mesh does not have (elements of types kept in passthrough), for the export.
    std::vector<std::int64_t> kept;
};

struct Scope {
    int place = place_model;
    std::size_t container = 0;
    std::string name;
    std::vector<OutSet> sets;
    std::map<std::size_t, std::size_t> by_set;  // SetDef -> OutSet.
    ItemRef mesh = no_item;
};

const std::set<std::string, std::less<>>& material_options() {
    static const std::set<std::string, std::less<>> options{
        "ELASTIC",
        "DENSITY",
        "PLASTIC",
        "EXPANSION",
        "CONDUCTIVITY",
        "SPECIFIC HEAT",
        "LATENT HEAT",
        "DAMPING",
        "HYPERELASTIC",
        "HYPERFOAM",
        "VISCOELASTIC",
        "VISCOUS",
        "CREEP",
        "SWELLING",
        "DEPVAR",
        "USER MATERIAL",
        "USER DEFINED FIELD",
        "DIELECTRIC",
        "ELECTRICAL CONDUCTIVITY",
        "JOULE HEAT FRACTION",
        "MULLINS EFFECT",
        "POROUS ELASTIC",
        "POROUS METAL PLASTICITY",
        "CONCRETE",
        "CONCRETE DAMAGED PLASTICITY",
        "CONCRETE TENSION STIFFENING",
        "CONCRETE COMPRESSION HARDENING",
        "CONCRETE TENSION DAMAGE",
        "CONCRETE COMPRESSION DAMAGE",
        "DAMAGE INITIATION",
        "DAMAGE EVOLUTION",
        "DAMAGE STABILIZATION",
        "DRUCKER PRAGER",
        "DRUCKER PRAGER HARDENING",
        "MOHR COULOMB",
        "MOHR COULOMB HARDENING",
        "CAP PLASTICITY",
        "CAP HARDENING",
        "CAST IRON PLASTICITY",
        "CRUSHABLE FOAM",
        "CRUSHABLE FOAM HARDENING",
        "CYCLIC HARDENING",
        "RATE DEPENDENT",
        "POTENTIAL",
        "HYSTERESIS",
        "FAIL STRESS",
        "FAIL STRAIN",
        "ANISOTROPIC HYPERELASTIC",
        "HYPOELASTIC",
        "NO COMPRESSION",
        "NO TENSION",
        "PERMEABILITY",
        "SORPTION",
        "SOLUBILITY",
        "DIFFUSIVITY",
        "GASKET THICKNESS BEHAVIOR",
        "INELASTIC HEAT FRACTION",
        "HEAT GENERATION",
        "REGULARIZE",
    };
    return options;
}

const std::set<std::string, std::less<>>& procedures() {
    static const std::set<std::string, std::less<>> names{
        "STATIC",
        "FREQUENCY",
        "BUCKLE",
        "DYNAMIC",
        "DYNAMIC TEMPERATURE-DISPLACEMENT",
        "HEAT TRANSFER",
        "COUPLED TEMPERATURE-DISPLACEMENT",
        "COUPLED THERMAL-ELECTRIC",
        "COUPLED THERMAL-ELECTRICAL-STRUCTURAL",
        "VISCO",
        "STEADY STATE DYNAMICS",
        "MODAL DYNAMIC",
        "RANDOM RESPONSE",
        "RESPONSE SPECTRUM",
        "SOILS",
        "GEOSTATIC",
        "COMPLEX FREQUENCY",
        "SUBSTRUCTURE GENERATE",
        "MATRIX GENERATE",
        "DIRECT CYCLIC",
        "ANNEAL",
        "MASS DIFFUSION",
        "ELECTROMAGNETIC",
        "ELECTRIC",
        "UNCOUPLED TEMPERATURE-DISPLACEMENT",
        "GREEN FUNCTION",
        "CFD",
    };
    return names;
}

/// Keywords of history output that belong to the *OUTPUT before them.
bool is_output_part(std::string_view name) {
    return name == "NODE OUTPUT" || name == "ELEMENT OUTPUT" || name == "ENERGY OUTPUT" ||
           name == "CONTACT OUTPUT" || name == "INCREMENTATION OUTPUT" ||
           name == "INTEGRATED OUTPUT" || name == "RADIATION OUTPUT" || name == "MODAL OUTPUT" ||
           name == "DIAGNOSTICS";
}

/// Keywords that make nodes or elements in ways the importer does not follow: without them the
/// mesh would be wrong, so the file is refused.
bool is_refused(std::string_view name) {
    return name == "SYSTEM" || name == "NGEN" || name == "NFILL" || name == "NCOPY" ||
           name == "NMAP" || name == "ELGEN" || name == "ELCOPY" || name == "PARAMETER";
}

std::string title_case(std::string_view upper_name) {
    // "CLOAD" -> "Cload", for the names of sets the importer makes ("_Cload-1").
    std::string out;
    bool start = true;
    for (const char c : upper_name) {
        if (c == ' ') {
            start = true;
            continue;
        }
        out.push_back(start || !(c >= 'A' && c <= 'Z') ? c : static_cast<char>(c - 'A' + 'a'));
        start = false;
    }
    return out;
}

constexpr std::array<std::string_view, 6> displacement_keys{"ux", "uy", "uz", "rx", "ry", "rz"};
constexpr std::array<std::string_view, 6> load_keys{"fx", "fy", "fz", "mx", "my", "mz"};

class Importer {
public:
    Importer(Deck deck, std::filesystem::path file)
        : deck_(std::move(deck)), file_(std::move(file)) {
        containers_.emplace_back();  // The model.
        problems_ = deck_.problems;
    }

    Import run() {
        read_keywords();
        return finish();
    }

private:
    // --- Problems --------------------------------------------------------------------------------

    [[nodiscard]] const std::string& display(std::size_t file) const {
        return deck_.files[file].display;
    }

    /// A problem at a keyword, or at one of its data lines.
    [[nodiscard]] Problem at(Problem::Severity severity, std::string key, Json args,
                             const Keyword& keyword, const DataLine* line) const {
        return Problem{severity, std::move(key), std::move(args),
                       display(line != nullptr ? line->file : keyword.file),
                       line != nullptr ? line->line : keyword.line};
    }

    void warn(std::string key, Json args, const Keyword& keyword, const DataLine* line = nullptr) {
        problems_.push_back(
            at(Problem::Severity::warning, std::move(key), std::move(args), keyword, line));
    }

    [[noreturn]] void fail(std::string key, Json args, const Keyword& keyword,
                           const DataLine* line = nullptr) const {
        throw InpError(
            at(Problem::Severity::error, std::move(key), std::move(args), keyword, line));
    }

    std::optional<double> number(std::string_view field, const Keyword& keyword,
                                 const DataLine& line) const {
        if (field.empty()) {
            return std::nullopt;
        }
        const std::optional<double> value = parse_number(field);
        if (!value) {
            fail("error.badNumber", {{"value", std::string(field)}, {"keyword", keyword.title()}},
                 keyword, &line);
        }
        return value;
    }

    std::int64_t integer(std::string_view field, const Keyword& keyword,
                         const DataLine& line) const {
        const std::optional<std::int64_t> value = parse_integer(field);
        if (!value) {
            fail("error.badNumber", {{"value", std::string(field)}, {"keyword", keyword.title()}},
                 keyword, &line);
        }
        return *value;
    }

    /// The parameter, which the keyword must have.
    std::string required(const Keyword& keyword, std::string_view key) const {
        const std::optional<std::string> value = keyword.value(key);
        if (!value || value->empty()) {
            fail("error.missingParameter",
                 {{"keyword", keyword.title()}, {"parameter", std::string(key)}}, keyword);
        }
        return *value;
    }

    /// The first parameter not in `known`, if any.
    static std::optional<std::string> unknown_parameter(
        const Keyword& keyword, std::initializer_list<std::string_view> known) {
        for (const Parameter& parameter : keyword.params) {
            if (std::find(known.begin(), known.end(), parameter.key) == known.end()) {
                return parameter.key;
            }
        }
        return std::nullopt;
    }

    /// Warns about parameters of a definition (nodes, elements, sets) that change nothing here.
    void ignore_unknown(const Keyword& keyword, std::initializer_list<std::string_view> known) {
        for (const Parameter& parameter : keyword.params) {
            if (std::find(known.begin(), known.end(), parameter.key) == known.end()) {
                warn("problem.ignoredParameter",
                     {{"keyword", keyword.title()}, {"parameter", parameter.key}}, keyword);
            }
        }
    }

    // --- Passthrough -----------------------------------------------------------------------------

    Json context_here() const {
        Json context = Json::object();
        if (region_ == Region::part) {
            context["part"] = containers_[container_].name;
        } else if (region_ == Region::assembly || region_ == Region::instance) {
            context["assembly"] = containers_[container_].name;
            if (region_ == Region::instance) {
                context["instance"] = instances_[static_cast<std::size_t>(instance_)].name;
            }
        }
        return context;
    }

    Parent parent_here() const {
        if (step_) {
            return {ParentKind::step, *step_};
        }
        return {ParentKind::input, 0};
    }

    /// Keeps the keyword as it is. `reason` is the problem to report, or empty for the plain
    /// "kept without interpretation".
    std::size_t keep(const Keyword& keyword, std::string text, Parent parent, Json context,
                     bool report_it = true) {
        Passthrough block;
        block.keyword = keyword.title();
        block.text = std::move(text);
        block.file = keyword.file;
        block.line = keyword.line;
        block.index = keyword.index;
        block.parent = parent;
        block.context = std::move(context);
        passthrough_.push_back(std::move(block));
        if (report_it) {
            const auto [it, added] =
                kept_.try_emplace(keyword.title(), Problem{Problem::Severity::warning,
                                                           "problem.passthrough",
                                                           {{"keyword", keyword.title()}},
                                                           display(keyword.file),
                                                           keyword.line});
            if (added) {
                kept_order_.push_back(keyword.title());
            }
            ++kept_count_[keyword.title()];
        }
        return passthrough_.size() - 1;
    }

    std::size_t keep(const Keyword& keyword) {
        return keep(keyword, keyword.text(), parent_here(), context_here());
    }

    /// Keeps a keyword whose parameter `parameter` the Model IR cannot hold.
    void keep_for_parameter(const Keyword& keyword, const std::string& parameter) {
        warn("problem.unsupportedParameter",
             {{"keyword", keyword.title()}, {"parameter", parameter}}, keyword);
        keep(keyword, keyword.text(), parent_here(), context_here(), false);
    }

    /// Lines of a keyword the importer did not understand, kept with its keyword line.
    void keep_lines(const Keyword& keyword, const std::vector<const DataLine*>& lines,
                    Parent parent) {
        if (lines.empty()) {
            return;
        }
        std::string text = keyword.written;
        for (const DataLine* line : lines) {
            text += '\n';
            text += line->text;
        }
        warn("problem.unsupportedLines",
             {{"keyword", keyword.title()}, {"count", static_cast<std::int64_t>(lines.size())}},
             keyword, lines.front());
        keep(keyword, std::move(text), parent, context_here(), false);
    }

    // --- Keywords --------------------------------------------------------------------------------

    enum class Region : std::uint8_t { model, part, assembly, instance };

    void read_keywords() {
        const std::vector<Keyword>& keywords = deck_.keywords;
        for (std::size_t k = 0; k < keywords.size(); ++k) {
            const Keyword& keyword = keywords[k];
            const std::string& name = keyword.name;
            if (material_ && material_options().contains(name)) {
                material_option(keyword);
                continue;
            }
            material_.reset();
            if (is_refused(name)) {
                fail("error.unsupportedKeyword", {{"keyword", keyword.title()}}, keyword);
            }
            if (region_ == Region::instance && name != "END INSTANCE") {
                keep(keyword);
                continue;
            }
            if (name == "STEP") {
                k = read_step(k);
            } else if (name == "HEADING") {
                for (const DataLine& line : keyword.data) {
                    heading_ += (heading_.empty() ? "" : "\n") + std::string(line.text);
                }
            } else if (name == "PART") {
                begin_part(keyword);
            } else if (name == "END PART") {
                end_region(keyword, Region::part);
            } else if (name == "ASSEMBLY") {
                begin_assembly(keyword);
            } else if (name == "END ASSEMBLY") {
                end_region(keyword, Region::assembly);
            } else if (name == "INSTANCE") {
                begin_instance(keyword);
            } else if (name == "END INSTANCE") {
                region_ = Region::assembly;
                instance_ = -1;
            } else if (name == "NODE") {
                read_nodes(keyword);
            } else if (name == "ELEMENT") {
                read_elements(keyword);
            } else if (name == "NSET" || name == "ELSET") {
                read_set(keyword, name == "NSET" ? SetKind::node : SetKind::element);
            } else if (name == "SURFACE") {
                read_surface(keyword);
            } else if (name == "MATERIAL") {
                begin_material(keyword);
            } else if (name == "SOLID SECTION" || name == "SHELL SECTION" ||
                       name == "BEAM SECTION") {
                read_section(keyword);
            } else if (name == "BOUNDARY") {
                read_boundary(keyword, {ParentKind::root, 0});
            } else {
                keep(keyword);
            }
        }
        if (region_ != Region::model) {
            const Keyword& last = keywords.back();
            fail("error.unclosed", {{"keyword", region_ == Region::part ? "*PART" : "*ASSEMBLY"}},
                 last);
        }
    }

    void begin_part(const Keyword& keyword) {
        if (region_ != Region::model) {
            fail("error.misplaced", {{"keyword", keyword.title()}}, keyword);
        }
        ignore_unknown(keyword, {"NAME"});
        const std::string name = required(keyword, "NAME");
        if (parts_.contains(upper(name))) {
            fail("error.duplicateName", {{"keyword", keyword.title()}, {"name", name}}, keyword);
        }
        Container part;
        part.name = name;
        containers_.push_back(std::move(part));
        container_ = containers_.size() - 1;
        parts_.emplace(upper(name), container_);
        region_ = Region::part;
    }

    void begin_assembly(const Keyword& keyword) {
        if (region_ != Region::model || assembly_) {
            fail("error.misplaced", {{"keyword", keyword.title()}}, keyword);
        }
        ignore_unknown(keyword, {"NAME"});
        Container assembly;
        assembly.name = keyword.value("NAME").value_or("Assembly");
        if (assembly.name.empty()) {
            assembly.name = "Assembly";
        }
        containers_.push_back(std::move(assembly));
        container_ = containers_.size() - 1;
        assembly_ = container_;
        region_ = Region::assembly;
    }

    void end_region(const Keyword& keyword, Region region) {
        if (region_ != region) {
            fail("error.misplaced", {{"keyword", keyword.title()}}, keyword);
        }
        region_ = Region::model;
        container_ = 0;
    }

    void begin_instance(const Keyword& keyword) {
        if (region_ != Region::assembly) {
            fail("error.misplaced", {{"keyword", keyword.title()}}, keyword);
        }
        ignore_unknown(keyword, {"NAME", "PART"});
        Instance instance;
        instance.name = required(keyword, "NAME");
        const std::string part = required(keyword, "PART");
        const auto it = parts_.find(upper(part));
        if (it == parts_.end()) {
            fail("error.unknownPart", {{"part", part}}, keyword);
        }
        if (instances_by_name_.contains(upper(instance.name))) {
            fail("error.duplicateName", {{"keyword", keyword.title()}, {"name", instance.name}},
                 keyword);
        }
        instance.part = it->second;
        std::vector<std::string_view> fields;
        for (std::size_t l = 0; l < keyword.data.size() && l < 2; ++l) {
            const DataLine& line = keyword.data[l];
            split_fields(line.text, fields);
            std::array<double, 7> values{};
            for (std::size_t f = 0; f < fields.size() && f < values.size(); ++f) {
                values[f] = number(fields[f], keyword, line).value_or(0.0);
            }
            if (l == 0) {
                instance.translation = {values[0], values[1], values[2]};
            } else {
                instance.rotated = true;
                instance.a = {values[0], values[1], values[2]};
                instance.b = {values[3], values[4], values[5]};
                instance.angle = values[6];
            }
        }
        if (keyword.data.size() > 2) {
            warn("problem.ignoredLines", {{"keyword", keyword.title()}}, keyword, &keyword.data[2]);
        }
        instances_by_name_.emplace(upper(instance.name), static_cast<int>(instances_.size()));
        instances_.push_back(std::move(instance));
        instance_ = static_cast<int>(instances_.size()) - 1;
        region_ = Region::instance;
    }

    // --- Nodes and elements ----------------------------------------------------------------------

    /// The container where definitions go now; the model outside parts.
    Container& here() { return containers_[container_]; }

    int place_here() const {
        if (region_ == Region::part) {
            return place_self;
        }
        if (region_ == Region::assembly) {
            return place_assembly;
        }
        return place_model;
    }

    void read_nodes(const Keyword& keyword) {
        ignore_unknown(keyword, {"NSET", "SYSTEM"});
        if (const auto system = keyword.value("SYSTEM"); system && upper(*system) != "R") {
            fail("error.unsupportedParameter",
                 {{"keyword", keyword.title()}, {"parameter", "SYSTEM=" + *system}}, keyword);
        }
        Container& container = here();
        std::vector<std::int64_t> labels;
        std::vector<std::string_view> fields;
        for (const DataLine& line : keyword.data) {
            split_fields(line.text, fields);
            const std::int64_t label = integer(fields[0], keyword, line);
            const auto coordinate = [&](std::size_t f) {
                return f < fields.size() ? number(fields[f], keyword, line).value_or(0.0) : 0.0;
            };
            container.node_ids.push_back(label);
            container.node_lines.push_back(&line);
            container.x.push_back(coordinate(1));
            container.y.push_back(coordinate(2));
            container.z.push_back(coordinate(3));
            labels.push_back(label);
        }
        if (const auto nset = keyword.value("NSET"); nset && !nset->empty()) {
            add_to_set(keyword, SetKind::node, *nset, place_here(), labels);
        }
    }

    void read_elements(const Keyword& keyword) {
        ignore_unknown(keyword, {"TYPE", "ELSET"});
        const std::string type = upper(required(keyword, "TYPE"));
        const std::optional<ElementKind> kind = element_kind(type);
        if (!kind) {
            warn("problem.unsupportedElementType", {{"type", type}}, keyword);
            keep(keyword, keyword.text(), parent_here(), context_here(), false);
            return;
        }
        if (!kind->basic) {
            warn("problem.elementType", {{"type", type}, {"shape", std::string(kind->shape)}},
                 keyword);
        }
        Container& container = here();
        ElementGroup group;
        group.kind = *kind;
        const auto per_element = static_cast<std::size_t>(kind->nodes) + 1;
        std::vector<std::int64_t> pending;
        const DataLine* first_line = nullptr;
        std::vector<std::string_view> fields;
        for (const DataLine& line : keyword.data) {
            split_fields(line.text, fields);
            if (fields.size() > 1 && fields.back().empty()) {
                fields.pop_back();  // A comma at the end: the element goes on in the next line.
            }
            if (pending.empty()) {
                first_line = &line;
            }
            for (const std::string_view field : fields) {
                pending.push_back(integer(field, keyword, line));
            }
            if (pending.size() > per_element) {
                fail("error.tooManyNodes",
                     {{"element", pending.front()}, {"type", type}, {"count", kind->nodes}},
                     keyword, first_line);
            }
            if (pending.size() == per_element) {
                group.labels.push_back(pending.front());
                group.nodes.insert(group.nodes.end(), pending.begin() + 1, pending.end());
                group.lines.push_back(first_line);
                pending.clear();
            }
        }
        if (!pending.empty()) {
            fail("error.incompleteElement",
                 {{"element", pending.front()}, {"type", type}, {"count", kind->nodes}}, keyword,
                 first_line);
        }
        if (const auto elset = keyword.value("ELSET"); elset && !elset->empty()) {
            add_to_set(keyword, SetKind::element, *elset, place_here(), group.labels);
        }
        container.groups.push_back(std::move(group));
    }

    // --- Sets ------------------------------------------------------------------------------------

    /// The set of this name in the container where definitions go now, made if there is none.
    std::size_t set_here(const Keyword& keyword, SetKind kind, const std::string& name) {
        auto& by_name = containers_[naming_container()].sets[static_cast<std::size_t>(kind)];
        const std::string key = upper(name);
        if (const auto it = by_name.find(key); it != by_name.end()) {
            return it->second;
        }
        SetDef set;
        set.kind = kind;
        set.name = name;
        set.container = naming_container();
        set.file = keyword.file;
        set.line = keyword.line;
        sets_.push_back(std::move(set));
        by_name.emplace(key, sets_.size() - 1);
        return sets_.size() - 1;
    }

    void add_to_set(const Keyword& keyword, SetKind kind, const std::string& name, int place,
                    const std::vector<std::int64_t>& labels) {
        std::vector<std::int64_t>& members = sets_[set_here(keyword, kind, name)].labels[place];
        members.insert(members.end(), labels.begin(), labels.end());
    }

    /// A set of `kind` named in a data line or a parameter where definitions go now: "Set-1"
    /// (the container's own), "Part-1-1.Set-1" (a set of the part of an instance, written in
    /// the assembly or in history data). Nothing if there is none.
    /// Outside parts and the assembly (model and history data) names are those of the assembly,
    /// or of the model if it has no parts.
    [[nodiscard]] bool outside() const { return step_ || region_ == Region::model; }
    [[nodiscard]] std::size_t naming_container() const {
        return outside() ? assembly_.value_or(0) : container_;
    }
    [[nodiscard]] bool assembly_names() const {
        return outside() ? assembly_.has_value() : region_ == Region::assembly;
    }

    std::optional<SetUse> find_set(SetKind kind, std::string_view name) const {
        const auto index = static_cast<std::size_t>(kind);
        const std::string key = upper(name);
        const auto& own = containers_[naming_container()].sets[index];
        if (const auto it = own.find(key); it != own.end()) {
            return SetUse{it->second, any_place};
        }
        if (assembly_names()) {
            const std::size_t dot = key.find('.');
            if (dot != std::string::npos) {
                const auto instance = instances_by_name_.find(key.substr(0, dot));
                if (instance != instances_by_name_.end()) {
                    const Instance& inst = instances_[static_cast<std::size_t>(instance->second)];
                    const auto& sets = containers_[inst.part].sets[index];
                    if (const auto it = sets.find(key.substr(dot + 1)); it != sets.end()) {
                        return SetUse{it->second, instance->second};
                    }
                }
            }
        }
        return std::nullopt;
    }

    /// A node or element written by its label where definitions go now: its place and label.
    /// "12" is in the container (the model, or the assembly), "Part-1-1.12" in an instance.
    std::optional<std::pair<int, std::int64_t>> find_label(std::string_view field) const {
        if (const std::optional<std::int64_t> label = parse_integer(field)) {
            const int place = assembly_names()                        ? place_assembly
                              : region_ == Region::part && !outside() ? place_self
                                                                      : place_model;
            return std::pair{place, *label};
        }
        const std::size_t dot = field.rfind('.');
        if (!assembly_names() || dot == std::string_view::npos) {
            return std::nullopt;
        }
        const auto instance = instances_by_name_.find(upper(field.substr(0, dot)));
        const std::optional<std::int64_t> label = parse_integer(field.substr(dot + 1));
        if (instance == instances_by_name_.end() || !label) {
            return std::nullopt;
        }
        return std::pair{instance->second, *label};
    }

    /// Labels of a set by place, a set of a part limited to one instance as `use` says.
    std::map<int, std::vector<std::int64_t>> members(const SetUse& use) const {
        const SetDef& set = sets_[use.set];
        if (use.place == any_place) {
            return set.labels;
        }
        std::map<int, std::vector<std::int64_t>> out;
        if (const auto it = set.labels.find(place_self); it != set.labels.end()) {
            out.emplace(use.place, it->second);
        }
        return out;
    }

    void read_set(const Keyword& keyword, SetKind kind) {
        const char* parameter = kind == SetKind::node ? "NSET" : "ELSET";
        ignore_unknown(keyword, {parameter, "GENERATE", "INSTANCE", "INTERNAL", "UNSORTED",
                                 kind == SetKind::node ? "ELSET" : "GENERATE"});
        const std::string name = required(keyword, parameter);
        const std::size_t index = set_here(keyword, kind, name);
        int place = place_here();
        std::optional<std::size_t> part;  // Names of sets in the data lines are of this part.
        if (const auto instance = keyword.value("INSTANCE")) {
            const auto it = instances_by_name_.find(upper(*instance));
            if (region_ != Region::assembly || it == instances_by_name_.end()) {
                warn("problem.unknownReference",
                     {{"keyword", keyword.title()}, {"name", *instance}}, keyword);
                return;
            }
            place = it->second;
            part = instances_[static_cast<std::size_t>(place)].part;
        }
        std::map<int, std::vector<std::int64_t>> added;
        if (kind == SetKind::node && keyword.has("ELSET")) {
            // The nodes of the elements of a set.
            const std::string elset = *keyword.value("ELSET");
            const std::optional<SetUse> use = find_set(SetKind::element, elset);
            if (!use) {
                warn("problem.unknownReference", {{"keyword", keyword.title()}, {"name", elset}},
                     keyword);
                return;
            }
            for (const auto& [where, labels] : members(*use)) {
                const Container& geometry =
                    where == place_self ? containers_[container_] : geometry_of(where);
                const std::unordered_set<std::int64_t> wanted(labels.begin(), labels.end());
                for (const ElementGroup& group : geometry.groups) {
                    const auto n = static_cast<std::size_t>(group.kind.nodes);
                    for (std::size_t e = 0; e < group.labels.size(); ++e) {
                        if (wanted.contains(group.labels[e])) {
                            added[where].insert(added[where].end(), group.nodes.begin() + e * n,
                                                group.nodes.begin() + (e + 1) * n);
                        }
                    }
                }
            }
        }
        const bool generate = keyword.has("GENERATE");
        std::vector<std::string_view> fields;
        for (const DataLine& line : keyword.data) {
            split_fields(line.text, fields);
            if (generate) {
                if (fields.size() < 2 || fields.size() > 4) {
                    fail("error.badGenerate", {{"keyword", keyword.title()}}, keyword, &line);
                }
                const std::int64_t first = integer(fields[0], keyword, line);
                const std::int64_t last = integer(fields[1], keyword, line);
                const std::int64_t step =
                    fields.size() > 2 && !fields[2].empty() ? integer(fields[2], keyword, line) : 1;
                if (step <= 0 || last < first || (last - first) / step > 100'000'000) {
                    fail("error.badGenerate", {{"keyword", keyword.title()}}, keyword, &line);
                }
                std::vector<std::int64_t>& labels = added[place];
                for (std::int64_t label = first; label <= last; label += step) {
                    labels.push_back(label);
                }
                continue;
            }
            for (const std::string_view field : fields) {
                if (field.empty()) {
                    continue;
                }
                if (const std::optional<std::int64_t> label = parse_integer(field)) {
                    added[place].push_back(*label);
                    continue;
                }
                if (part) {
                    // A set of the instance's part.
                    const auto& sets = containers_[*part].sets[static_cast<std::size_t>(kind)];
                    if (const auto it = sets.find(upper(field)); it != sets.end()) {
                        const SetDef& source = sets_[it->second];
                        if (const auto own = source.labels.find(place_self);
                            own != source.labels.end()) {
                            added[place].insert(added[place].end(), own->second.begin(),
                                                own->second.end());
                        }
                        continue;
                    }
                } else if (const std::optional<SetUse> use = find_set(kind, field)) {
                    for (const auto& [where, labels] : members(*use)) {
                        added[where].insert(added[where].end(), labels.begin(), labels.end());
                    }
                    continue;
                } else if (const auto label = find_label(field)) {
                    added[label->first].push_back(label->second);
                    continue;
                }
                warn("problem.unknownReference",
                     {{"keyword", keyword.title()}, {"name", std::string(field)}}, keyword, &line);
            }
        }
        SetDef& set = sets_[index];
        for (auto& [where, labels] : added) {
            std::vector<std::int64_t>& target = set.labels[where];
            target.insert(target.end(), labels.begin(), labels.end());
        }
    }

    void read_surface(const Keyword& keyword) {
        ignore_unknown(keyword, {"NAME", "TYPE", "INTERNAL"});
        const std::string name = required(keyword, "NAME");
        const std::size_t index = set_here(keyword, SetKind::surface, name);
        const std::string type = upper(keyword.value("TYPE").value_or("ELEMENT"));
        if (type != "ELEMENT") {
            sets_[index].unsupported = true;
            warn("problem.unsupportedSurface", {{"name", name}, {"type", type}}, keyword);
            keep(keyword, keyword.text(), parent_here(), context_here(), false);
            return;
        }
        std::vector<std::string_view> fields;
        std::map<int, std::vector<std::pair<std::int64_t, int>>> faces;
        for (const DataLine& line : keyword.data) {
            split_fields(line.text, fields);
            const std::string face = fields.size() > 1 ? upper(fields[1]) : std::string();
            int face_number = 0;
            if (face.size() == 2 && face[0] == 'S' && face[1] >= '1' && face[1] <= '6') {
                face_number = face[1] - '0';
            } else {
                // SPOS/SNEG of shells, edges, or the free faces of a set (no face): not in the IR.
                sets_[index].unsupported = true;
                warn("problem.unsupportedSurface", {{"name", name}, {"type", face}}, keyword,
                     &line);
                keep(keyword, keyword.text(), parent_here(), context_here(), false);
                return;
            }
            if (const std::optional<SetUse> use = find_set(SetKind::element, fields[0])) {
                for (const auto& [where, labels] : members(*use)) {
                    for (const std::int64_t label : labels) {
                        faces[where].emplace_back(label, face_number);
                    }
                }
            } else if (const auto label = find_label(fields[0])) {
                faces[label->first].emplace_back(label->second, face_number);
            } else {
                warn("problem.unknownReference",
                     {{"keyword", keyword.title()}, {"name", std::string(fields[0])}}, keyword,
                     &line);
            }
        }
        SetDef& set = sets_[index];
        for (auto& [where, list] : faces) {
            auto& target = set.faces[where];
            target.insert(target.end(), list.begin(), list.end());
        }
    }

    // --- Materials and sections ------------------------------------------------------------------

    void begin_material(const Keyword& keyword) {
        ignore_unknown(keyword, {"NAME"});
        const std::string name = required(keyword, "NAME");
        if (materials_by_name_.contains(upper(name))) {
            fail("error.duplicateName", {{"keyword", keyword.title()}, {"name", name}}, keyword);
        }
        materials_.push_back({name, Json::object()});
        material_ = materials_.size() - 1;
        materials_by_name_.emplace(upper(name), *material_);
    }

    void keep_material_option(const Keyword& keyword) {
        Json context = context_here();
        context["material"] = materials_[*material_].name;
        keep(keyword, keyword.text(), {ParentKind::material, *material_}, std::move(context));
    }

    void material_option(const Keyword& keyword) {
        Material& material = materials_[*material_];
        const bool elastic = keyword.name == "ELASTIC";
        if (!elastic && keyword.name != "DENSITY") {
            keep_material_option(keyword);
            return;
        }
        const std::optional<std::string> unknown =
            elastic ? unknown_parameter(keyword, {"TYPE"}) : unknown_parameter(keyword, {});
        const bool isotropic = upper(keyword.value("TYPE").value_or("ISOTROPIC")) == "ISOTROPIC";
        if (unknown || !isotropic) {
            warn("problem.unsupportedParameter",
                 {{"keyword", keyword.title()},
                  {"parameter", unknown ? *unknown : "TYPE=" + *keyword.value("TYPE")}},
                 keyword);
            Json context = context_here();
            context["material"] = material.name;
            keep(keyword, keyword.text(), {ParentKind::material, *material_}, std::move(context),
                 false);
            return;
        }
        if (keyword.data.size() != 1) {
            // Values by temperature, or none.
            warn("problem.temperatureDependent",
                 {{"keyword", keyword.title()}, {"material", material.name}}, keyword);
            Json context = context_here();
            context["material"] = material.name;
            keep(keyword, keyword.text(), {ParentKind::material, *material_}, std::move(context),
                 false);
            return;
        }
        const DataLine& line = keyword.data.front();
        const std::vector<std::string_view> fields = split_fields(line.text);
        const auto value = [&](std::size_t f) {
            return f < fields.size() ? number(fields[f], keyword, line) : std::nullopt;
        };
        if (elastic) {
            if (const auto e = value(0)) {
                material.properties["youngsModulus"] = *e;
            }
            if (const auto nu = value(1)) {
                material.properties["poissonsRatio"] = *nu;
            }
        } else if (const auto rho = value(0)) {
            material.properties["density"] = *rho;
        }
    }

    void read_section(const Keyword& keyword) {
        const std::string& name = keyword.name;
        std::optional<std::string> unknown;
        if (name == "SOLID SECTION") {
            unknown = unknown_parameter(keyword, {"ELSET", "MATERIAL"});
        } else if (name == "SHELL SECTION") {
            unknown =
                unknown_parameter(keyword, {"ELSET", "MATERIAL", "OFFSET", "SECTION INTEGRATION"});
        } else {
            unknown = unknown_parameter(keyword, {"ELSET", "MATERIAL", "SECTION"});
        }
        if (unknown) {
            keep_for_parameter(keyword, *unknown);
            return;
        }
        const std::string elset = required(keyword, "ELSET");
        const std::string material = required(keyword, "MATERIAL");
        const std::optional<SetUse> set = find_set(SetKind::element, elset);
        if (!set) {
            warn("problem.unknownReference", {{"keyword", keyword.title()}, {"name", elset}},
                 keyword);
            keep(keyword, keyword.text(), parent_here(), context_here(), false);
            return;
        }
        PendingSection section;
        section.material = material;
        section.set = *set;
        section.keyword = keyword.index;
        section.context = context_here();
        std::vector<std::string_view> first;
        if (!keyword.data.empty()) {
            first = split_fields(keyword.data.front().text);
        }
        const auto value = [&](std::size_t line, std::size_t f) -> std::optional<double> {
            if (line >= keyword.data.size()) {
                return std::nullopt;
            }
            const std::vector<std::string_view> fields = split_fields(keyword.data[line].text);
            return f < fields.size() ? number(fields[f], keyword, keyword.data[line])
                                     : std::nullopt;
        };
        if (name == "SOLID SECTION") {
            section.type = "solid_section";
            section.family = Family::solid;
        } else if (name == "SHELL SECTION") {
            section.type = "shell_section";
            section.family = Family::shell;
            if (const auto thickness = value(0, 0)) {
                section.properties["thickness"] = *thickness;
            }
            if (const auto points = value(0, 1)) {
                section.properties["integrationPoints"] = static_cast<std::int64_t>(*points);
            }
            if (const auto offset = keyword.value("OFFSET")) {
                const std::string text = upper(*offset);
                if (text == "SPOS") {
                    section.properties["offset"] = 0.5;
                } else if (text == "SNEG") {
                    section.properties["offset"] = -0.5;
                } else if (text == "MIDDLE") {
                    section.properties["offset"] = 0.0;
                } else if (const auto fraction = parse_number(*offset)) {
                    section.properties["offset"] = *fraction;
                } else {
                    keep_for_parameter(keyword, "OFFSET=" + *offset);
                    return;
                }
            }
        } else {
            section.type = "beam_section";
            section.family = Family::beam;
            const std::string shape = upper(keyword.value("SECTION").value_or(""));
            if (shape == "RECT") {
                section.properties["shape"] = "rectangle";
                if (const auto a = value(0, 0)) {
                    section.properties["width"] = *a;
                }
                if (const auto b = value(0, 1)) {
                    section.properties["height"] = *b;
                }
            } else if (shape == "CIRC") {
                section.properties["shape"] = "circle";
                if (const auto r = value(0, 0)) {
                    section.properties["radius"] = *r;
                }
            } else if (shape == "PIPE") {
                section.properties["shape"] = "pipe";
                if (const auto r = value(0, 0)) {
                    section.properties["radius"] = *r;
                }
                if (const auto t = value(0, 1)) {
                    section.properties["wallThickness"] = *t;
                }
            } else {
                keep_for_parameter(keyword, "SECTION=" + keyword.value("SECTION").value_or(""));
                return;
            }
            if (keyword.data.size() > 1) {
                section.properties["direction"] = {{"x", value(1, 0).value_or(0.0)},
                                                   {"y", value(1, 1).value_or(0.0)},
                                                   {"z", value(1, 2).value_or(0.0)}};
            }
        }
        sections_.push_back(std::move(section));
    }

    // --- Steps -----------------------------------------------------------------------------------

    std::size_t read_step(std::size_t start) {
        const std::vector<Keyword>& keywords = deck_.keywords;
        const Keyword& keyword = keywords[start];
        std::size_t end = start + 1;
        while (end < keywords.size() && keywords[end].name != "END STEP") {
            if (keywords[end].name == "STEP") {
                fail("error.unclosed", {{"keyword", "*STEP"}}, keywords[end]);
            }
            ++end;
        }
        if (end == keywords.size()) {
            fail("error.unclosed", {{"keyword", "*STEP"}}, keyword);
        }
        const std::string name =
            keyword.value("NAME").value_or("Step-" + std::to_string(steps_.size() + 1));
        std::vector<std::string> found;
        for (std::size_t k = start + 1; k < end; ++k) {
            const Keyword& inner = keywords[k];
            if (procedures().contains(inner.name)) {
                found.push_back(inner.name == "STATIC" && inner.has("RIKS") ? "STATIC, RIKS"
                                                                            : inner.name);
            }
        }
        if (found.size() != 1 || found.front() != "STATIC") {
            // Not a linear static step: kept whole.
            std::string text;
            for (std::size_t k = start; k <= end; ++k) {
                text += (k == start ? "" : "\n") + keywords[k].text();
            }
            std::string procedure;
            for (const std::string& p : found) {
                procedure += (procedure.empty() ? "*" : ", *") + p;
            }
            warn("problem.unsupportedStep", {{"step", name}, {"procedure", procedure}}, keyword);
            step_names_.push_back(name);
            Json context = context_here();
            context["step"] = name;
            keep(keyword, std::move(text), {ParentKind::input, 0}, std::move(context), false);
            return end;
        }
        Step step;
        step.name = name;
        step.index = keyword.index;
        for (const Parameter& parameter : keyword.params) {
            if (parameter.key == "NLGEOM") {
                if (upper(parameter.value) != "NO") {
                    warn("problem.nlgeom", {{"step", name}}, keyword);
                }
            } else if (parameter.key == "INC") {
                const std::optional<std::int64_t> inc = parse_integer(parameter.value);
                if (inc && *inc >= 1) {
                    step.properties["maxIncrements"] = *inc;
                } else {
                    warn("problem.ignoredParameter",
                         {{"keyword", keyword.title()}, {"parameter", "INC=" + parameter.value}},
                         keyword);
                }
            } else if (parameter.key != "NAME" && parameter.key != "PERTURBATION") {
                warn("problem.ignoredParameter",
                     {{"keyword", keyword.title()}, {"parameter", parameter.key}}, keyword);
            }
        }
        step_names_.push_back(name);
        steps_.push_back(std::move(step));
        step_ = steps_.size() - 1;
        const Parent parent{ParentKind::step, *step_};
        std::optional<std::size_t> history;     // The passthrough of an open *OUTPUT, HISTORY.
        const Keyword* field_output = nullptr;  // An open *OUTPUT, FIELD.
        for (std::size_t k = start + 1; k < end; ++k) {
            const Keyword& inner = keywords[k];
            const std::string& n = inner.name;
            if (history && is_output_part(n)) {
                passthrough_[*history].text += "\n" + inner.text();
                continue;
            }
            history.reset();
            if (!is_output_part(n)) {
                field_output = nullptr;
            }
            if (n == "STATIC") {
                read_static(inner);
            } else if (n == "BOUNDARY") {
                read_boundary(inner, parent);
            } else if (n == "CLOAD") {
                read_cload(inner, parent);
            } else if (n == "DLOAD") {
                read_dload(inner, parent);
            } else if (n == "DSLOAD") {
                read_dsload(inner, parent);
            } else if (n == "OUTPUT") {
                if (inner.has("HISTORY")) {
                    history = keep(inner);
                } else {
                    field_output = &inner;
                    read_output(inner);
                }
            } else if (n == "NODE OUTPUT" || n == "ELEMENT OUTPUT") {
                read_output_part(inner, field_output, parent);
            } else if (n == "NODE FILE" || n == "EL FILE" || n == "NODE PRINT" || n == "EL PRINT") {
                read_file_output(inner, parent);
            } else if (is_output_part(n) && field_output != nullptr) {
                keep(inner, field_output->written + "\n" + inner.text(), parent, context_here());
            } else {
                keep(inner);
            }
        }
        step_.reset();
        return end;
    }

    void read_static(const Keyword& keyword) {
        ignore_unknown(keyword, {"DIRECT"});
        Json& properties = steps_[*step_].properties;
        double period = 1.0;
        if (!keyword.data.empty()) {
            const DataLine& line = keyword.data.front();
            const std::vector<std::string_view> fields = split_fields(line.text);
            const auto value = [&](std::size_t f) {
                return f < fields.size() ? number(fields[f], keyword, line) : std::nullopt;
            };
            if (const auto p = value(1); p && *p > 0) {
                period = *p;
            }
            properties["timePeriod"] = period;
            const auto initial = value(0);
            properties["initialIncrement"] = initial && *initial > 0 ? *initial : period;
        }
    }

    /// What a field of a load or boundary line names: a set, or nodes or elements by label.
    struct Target {
        std::optional<SetUse> set;
        int place = place_model;
        std::int64_t label = 0;
    };

    /// The target of a data line; nothing (after a warning) if it names nothing or something
    /// kept in passthrough.
    std::optional<Target> find_target(const Keyword& keyword, const DataLine& line, SetKind kind,
                                      std::string_view field) {
        if (const std::optional<SetUse> use = find_set(kind, field)) {
            if (sets_[use->set].unsupported) {
                warn("problem.dependsOnPassthrough",
                     {{"keyword", keyword.title()}, {"name", std::string(field)}}, keyword, &line);
                return std::nullopt;
            }
            return Target{use, 0, 0};
        }
        if (kind != SetKind::surface) {
            if (const auto label = find_label(field)) {
                return Target{std::nullopt, label->first, label->second};
            }
        }
        warn("problem.unknownReference",
             {{"keyword", keyword.title()}, {"name", std::string(field)}}, keyword, &line);
        return std::nullopt;
    }

    /// A set the importer makes for labels written in a load: "_Cload-1".
    std::size_t implicit_set(const Keyword& keyword, SetKind kind) {
        const std::string name =
            "_" + title_case(keyword.name) + "-" + std::to_string(++implicit_[keyword.name]);
        SetDef set;
        set.kind = kind;
        set.name = name;
        set.container = assembly_.value_or(0);
        set.implicit = true;
        set.file = keyword.file;
        set.line = keyword.line;
        sets_.push_back(std::move(set));
        return sets_.size() - 1;
    }

    [[nodiscard]] std::string set_name(const SetUse& use) const {
        const SetDef& set = sets_[use.set];
        if (use.place >= 0 && !set.implicit && set.container != assembly_.value_or(0)) {
            return instances_[static_cast<std::size_t>(use.place)].name + "." + set.name;
        }
        return set.name;
    }

    /// Groups the lines of a load or a boundary keyword by what they act on: by set, and labels
    /// with the same values into one set the importer makes.
    struct Group {
        SetUse set;
        Json properties = Json::object();
        std::int64_t line = 0;
    };

    class Grouper {
    public:
        Grouper(Importer& importer, const Keyword& keyword, SetKind kind)
            : importer_(importer), keyword_(keyword), kind_(kind) {}

        /// The values of what `target` names: of its set, or of its node or element. The values of
        /// one node given in several lines are merged.
        Json& at(const Target& target, std::int64_t line) {
            if (target.set) {
                const auto key = std::pair{target.set->set, target.set->place};
                auto [it, added] = named_.try_emplace(key, groups_.size());
                if (added) {
                    groups_.push_back({*target.set, Json::object(), line});
                }
                return groups_[it->second].properties;
            }
            const auto key = std::pair{target.place, target.label};
            auto [it, added] = labelled_.try_emplace(key, Json::object());
            if (added) {
                labels_.push_back(key);
            }
            return it->second;
        }

        /// The groups: of sets, then labels with the same values in one set the importer makes.
        [[nodiscard]] std::vector<Group>& groups() {
            std::map<std::string, std::size_t> by_values;
            for (const auto& key : labels_) {
                const Json& values = labelled_.at(key);
                auto [it, added] = by_values.try_emplace(values.dump(), groups_.size());
                if (added) {
                    const std::size_t set = importer_.implicit_set(keyword_, kind_);
                    groups_.push_back({SetUse{set, any_place}, values, 0});
                }
                importer_.sets_[groups_[it->second].set.set].labels[key.first].push_back(
                    key.second);
            }
            labels_.clear();
            return groups_;
        }

    private:
        Importer& importer_;
        const Keyword& keyword_;
        SetKind kind_;
        std::vector<Group> groups_;
        std::map<std::pair<std::size_t, int>, std::size_t> named_;
        std::map<std::pair<int, std::int64_t>, Json> labelled_;
        std::vector<std::pair<int, std::int64_t>> labels_;
    };

    void add_pending(std::string type, std::string name, Json properties, Parent parent,
                     std::vector<SetUse> sets, const Keyword& keyword) {
        Pending pending;
        pending.type = std::move(type);
        pending.name = std::move(name);
        pending.properties = std::move(properties);
        pending.parent = parent;
        pending.sets = std::move(sets);
        pending.order = keyword.index;
        pending_.push_back(std::move(pending));
    }

    void read_boundary(const Keyword& keyword, Parent parent) {
        if (const auto unknown = unknown_parameter(keyword, {"OP", "TYPE"})) {
            keep_for_parameter(keyword, *unknown);
            return;
        }
        if (const auto op = keyword.value("OP"); op && upper(*op) != "MOD") {
            keep_for_parameter(keyword, "OP=" + *op);
            return;
        }
        if (const auto type = keyword.value("TYPE"); type && upper(*type) != "DISPLACEMENT") {
            keep_for_parameter(keyword, "TYPE=" + *type);
            return;
        }
        static const std::map<std::string, std::vector<int>, std::less<>> shortcuts{
            {"ENCASTRE", {1, 2, 3, 4, 5, 6}},
            {"PINNED", {1, 2, 3}},
            {"XSYMM", {1, 5, 6}},
            {"YSYMM", {2, 4, 6}},
            {"ZSYMM", {3, 4, 5}},
            {"XASYMM", {2, 3, 4}},
            {"YASYMM", {1, 3, 5}},
            {"ZASYMM", {1, 2, 6}},
        };
        Grouper grouper(*this, keyword, SetKind::node);
        std::vector<const DataLine*> unsupported;
        std::vector<std::string_view> fields;
        for (const DataLine& line : keyword.data) {
            split_fields(line.text, fields);
            if (fields.size() < 2) {
                fail("error.syntax", {{"keyword", keyword.title()}}, keyword, &line);
            }
            Json values = Json::object();
            if (const auto shortcut = shortcuts.find(upper(fields[1]));
                shortcut != shortcuts.end()) {
                for (const int dof : shortcut->second) {
                    values[std::string(displacement_keys[static_cast<std::size_t>(dof - 1)])] = 0.0;
                }
            } else {
                const std::int64_t first = integer(fields[1], keyword, line);
                const std::int64_t last = fields.size() > 2 && !fields[2].empty()
                                              ? integer(fields[2], keyword, line)
                                              : first;
                const double value =
                    fields.size() > 3 ? number(fields[3], keyword, line).value_or(0.0) : 0.0;
                if (first < 1 || last > 6 || last < first) {
                    unsupported.push_back(&line);  // Temperature, warping, pressure dofs.
                    continue;
                }
                for (std::int64_t dof = first; dof <= last; ++dof) {
                    values[std::string(displacement_keys[static_cast<std::size_t>(dof - 1)])] =
                        value;
                }
            }
            const std::optional<Target> target =
                find_target(keyword, line, SetKind::node, fields[0]);
            if (!target) {
                unsupported.push_back(&line);
                continue;
            }
            grouper.at(*target, line.line).update(values);
        }
        keep_lines(keyword, unsupported, parent);
        for (Group& group : grouper.groups()) {
            add_pending("boundary_condition", set_name(group.set), std::move(group.properties),
                        parent, {group.set}, keyword);
        }
    }

    void read_cload(const Keyword& keyword, Parent parent) {
        if (const auto unknown = unknown_parameter(keyword, {"OP"})) {
            keep_for_parameter(keyword, *unknown);
            return;
        }
        if (const auto op = keyword.value("OP"); op && upper(*op) != "MOD") {
            keep_for_parameter(keyword, "OP=" + *op);
            return;
        }
        Grouper grouper(*this, keyword, SetKind::node);
        std::vector<const DataLine*> unsupported;
        std::vector<std::string_view> fields;
        for (const DataLine& line : keyword.data) {
            split_fields(line.text, fields);
            if (fields.size() < 3) {
                fail("error.syntax", {{"keyword", keyword.title()}}, keyword, &line);
            }
            const std::int64_t dof = integer(fields[1], keyword, line);
            const double value = number(fields[2], keyword, line).value_or(0.0);
            if (dof < 1 || dof > 6) {
                unsupported.push_back(&line);
                continue;
            }
            const std::optional<Target> target =
                find_target(keyword, line, SetKind::node, fields[0]);
            if (!target) {
                unsupported.push_back(&line);
                continue;
            }
            const std::string key(load_keys[static_cast<std::size_t>(dof - 1)]);
            Json& properties = grouper.at(*target, line.line);
            // Loads on the same degree of freedom add up.
            properties[key] = properties.value(key, 0.0) + value;
        }
        keep_lines(keyword, unsupported, parent);
        for (Group& group : grouper.groups()) {
            add_pending("concentrated_load", set_name(group.set), std::move(group.properties),
                        parent, {group.set}, keyword);
        }
    }

    void read_dload(const Keyword& keyword, Parent parent) {
        if (const auto unknown = unknown_parameter(keyword, {"OP"})) {
            keep_for_parameter(keyword, *unknown);
            return;
        }
        if (const auto op = keyword.value("OP"); op && upper(*op) != "MOD") {
            keep_for_parameter(keyword, "OP=" + *op);
            return;
        }
        std::vector<const DataLine*> unsupported;
        std::vector<std::string_view> fields;
        for (const DataLine& line : keyword.data) {
            split_fields(line.text, fields);
            if (fields.size() < 3) {
                fail("error.syntax", {{"keyword", keyword.title()}}, keyword, &line);
            }
            const std::string type = upper(fields[1]);
            const double magnitude = number(fields[2], keyword, line).value_or(0.0);
            if (type == "GRAV") {
                std::array<double, 3> direction{};
                for (std::size_t c = 0; c < 3; ++c) {
                    direction[c] = fields.size() > 3 + c
                                       ? number(fields[3 + c], keyword, line).value_or(0.0)
                                       : 0.0;
                }
                const double sign = magnitude < 0 ? -1.0 : 1.0;
                Json properties = {{"acceleration", std::abs(magnitude)},
                                   {"direction",
                                    {{"x", sign * direction[0]},
                                     {"y", sign * direction[1]},
                                     {"z", sign * direction[2]}}}};
                std::vector<SetUse> sets;
                std::string name = "Gravity";
                if (!fields[0].empty()) {
                    const std::optional<Target> target =
                        find_target(keyword, line, SetKind::element, fields[0]);
                    if (!target) {
                        unsupported.push_back(&line);
                        continue;
                    }
                    const SetUse use = target->set ? *target->set : labelled_set(keyword, *target);
                    sets.push_back(use);
                    name = set_name(use);
                }
                add_pending("gravity", std::move(name), std::move(properties), parent,
                            std::move(sets), keyword);
                continue;
            }
            const bool face =
                type.size() == 2 && type[0] == 'P' && type[1] >= '1' && type[1] <= '6';
            if (type != "P" && !face) {
                unsupported.push_back(&line);
                continue;
            }
            const std::optional<Target> target =
                find_target(keyword, line, SetKind::element, fields[0]);
            if (!target) {
                unsupported.push_back(&line);
                continue;
            }
            SetUse use;
            if (face) {
                // Pressure on faces of solids: a surface of these faces.
                const std::size_t surface = implicit_set(keyword, SetKind::surface);
                const int face_number = type[1] - '0';
                const std::map<int, std::vector<std::int64_t>> elements =
                    target->set ? members(*target->set)
                                : std::map<int, std::vector<std::int64_t>>{
                                      {target->place, {target->label}}};
                for (const auto& [where, labels] : elements) {
                    auto& list = sets_[surface].faces[where];
                    for (const std::int64_t label : labels) {
                        list.emplace_back(label, face_number);
                    }
                }
                use = SetUse{surface, any_place};
            } else {
                use = target->set ? *target->set : labelled_set(keyword, *target);
            }
            add_pending("pressure", set_name(use), {{"magnitude", magnitude}}, parent, {use},
                        keyword);
        }
        keep_lines(keyword, unsupported, parent);
    }

    /// A set the importer makes for one element or node written by its label.
    SetUse labelled_set(const Keyword& keyword, const Target& target) {
        const std::size_t set = implicit_set(keyword, SetKind::element);
        sets_[set].labels[target.place].push_back(target.label);
        return SetUse{set, any_place};
    }

    void read_dsload(const Keyword& keyword, Parent parent) {
        if (const auto unknown = unknown_parameter(keyword, {"OP"})) {
            keep_for_parameter(keyword, *unknown);
            return;
        }
        if (const auto op = keyword.value("OP"); op && upper(*op) != "MOD") {
            keep_for_parameter(keyword, "OP=" + *op);
            return;
        }
        std::vector<const DataLine*> unsupported;
        std::vector<std::string_view> fields;
        for (const DataLine& line : keyword.data) {
            split_fields(line.text, fields);
            if (fields.size() < 3) {
                fail("error.syntax", {{"keyword", keyword.title()}}, keyword, &line);
            }
            if (upper(fields[1]) != "P") {
                unsupported.push_back(&line);
                continue;
            }
            const double magnitude = number(fields[2], keyword, line).value_or(0.0);
            const std::optional<Target> target =
                find_target(keyword, line, SetKind::surface, fields[0]);
            if (!target) {
                unsupported.push_back(&line);
                continue;
            }
            add_pending("pressure", set_name(*target->set), {{"magnitude", magnitude}}, parent,
                        {*target->set}, keyword);
        }
        keep_lines(keyword, unsupported, parent);
    }

    /// Output frequency of a keyword; nothing (after keeping it) if the IR cannot hold it.
    std::optional<std::int64_t> output_frequency(const Keyword& keyword) {
        const std::optional<std::string> text = keyword.value("FREQUENCY");
        if (!text) {
            return 1;
        }
        const std::optional<std::int64_t> frequency = parse_integer(*text);
        if (!frequency || *frequency < 1) {
            keep_for_parameter(keyword, "FREQUENCY=" + *text);
            return std::nullopt;
        }
        return frequency;
    }

    void read_output(const Keyword& keyword) {
        if (const auto unknown =
                unknown_parameter(keyword, {"FIELD", "VARIABLE", "FREQUENCY", "OP"})) {
            keep_for_parameter(keyword, *unknown);
            return;
        }
        const std::optional<std::string> variable = keyword.value("VARIABLE");
        if (!variable) {
            return;  // The variables come with *NODE OUTPUT and *ELEMENT OUTPUT.
        }
        const std::string which = upper(*variable);
        if (which != "PRESELECT" && which != "ALL") {
            keep_for_parameter(keyword, "VARIABLE=" + *variable);
            return;
        }
        const std::optional<std::int64_t> frequency = output_frequency(keyword);
        if (!frequency) {
            return;
        }
        add_pending("output_request", "Field output",
                    {{"displacement", true},
                     {"reactionForce", true},
                     {"stress", true},
                     {"strain", true},
                     {"destination", "results"},
                     {"frequency", *frequency}},
                    {ParentKind::step, *step_}, {}, keyword);
    }

    /// The variables of an output keyword the IR has, as flags; the others go to `rest`.
    static Json output_flags(const Keyword& keyword, bool nodal, std::vector<std::string>& rest) {
        Json flags = {{"displacement", false},
                      {"reactionForce", false},
                      {"stress", false},
                      {"strain", false}};
        std::vector<std::string> names;
        if (const auto variable = keyword.value("VARIABLE")) {
            names.push_back(upper(*variable));
        }
        for (const DataLine& line : keyword.data) {
            for (const std::string_view field : split_fields(line.text)) {
                if (!field.empty()) {
                    names.push_back(upper(field));
                }
            }
        }
        for (const std::string& name : names) {
            if (name == "PRESELECT" || name == "ALL") {
                flags[nodal ? "displacement" : "stress"] = true;
                flags[nodal ? "reactionForce" : "strain"] = true;
            } else if (nodal && name == "U") {
                flags["displacement"] = true;
            } else if (nodal && name == "RF") {
                flags["reactionForce"] = true;
            } else if (!nodal && name == "S") {
                flags["stress"] = true;
            } else if (!nodal && name == "E") {
                flags["strain"] = true;
            } else {
                rest.push_back(name);
            }
        }
        return flags;
    }

    void add_output(const Keyword& keyword, bool nodal, const std::string& destination,
                    std::int64_t frequency, const char* set_parameter, Parent parent,
                    const std::string& prefix) {
        std::vector<std::string> rest;
        Json properties = output_flags(keyword, nodal, rest);
        properties["destination"] = destination;
        properties["frequency"] = frequency;
        std::vector<SetUse> sets;
        std::string name = nodal ? "Node output" : "Element output";
        if (const auto set = keyword.value(set_parameter)) {
            const std::optional<SetUse> use =
                find_set(nodal ? SetKind::node : SetKind::element, *set);
            if (!use) {
                warn("problem.unknownReference", {{"keyword", keyword.title()}, {"name", *set}},
                     keyword);
                keep(keyword, prefix + keyword.text(), parent, context_here(), false);
                return;
            }
            sets.push_back(*use);
            name = set_name(*use);
        }
        if (!rest.empty()) {
            std::string variables;
            for (const std::string& variable : rest) {
                variables += (variables.empty() ? "" : ", ") + variable;
            }
            warn("problem.unsupportedVariables",
                 {{"keyword", keyword.title()}, {"variables", variables}}, keyword);
            std::string written = keyword.written;
            keep(keyword, prefix + written + "\n" + variables, parent, context_here(), false);
        }
        const bool any = properties["displacement"].get<bool>() ||
                         properties["reactionForce"].get<bool>() ||
                         properties["stress"].get<bool>() || properties["strain"].get<bool>();
        if (any) {
            add_pending("output_request", std::move(name), std::move(properties), parent,
                        std::move(sets), keyword);
        }
    }

    void read_output_part(const Keyword& keyword, const Keyword* output, Parent parent) {
        const bool nodal = keyword.name == "NODE OUTPUT";
        if (const auto unknown =
                unknown_parameter(keyword, {nodal ? "NSET" : "ELSET", "VARIABLE", "DIRECTIONS"})) {
            keep_for_parameter(keyword, *unknown);
            return;
        }
        std::int64_t frequency = 1;
        std::string prefix;
        if (output != nullptr) {
            if (const auto f = output->value("FREQUENCY")) {
                frequency = parse_integer(*f).value_or(1);
            }
            prefix = output->written + "\n";
        }
        add_output(keyword, nodal, "results", frequency, nodal ? "NSET" : "ELSET", parent, prefix);
    }

    void read_file_output(const Keyword& keyword, Parent parent) {
        const bool nodal = keyword.name.starts_with("NODE");
        const bool print = keyword.name.ends_with("PRINT");
        const char* set = nodal ? "NSET" : "ELSET";
        if (const auto unknown =
                unknown_parameter(keyword, {set, "FREQUENCY", "TOTALS", "GLOBAL", "OUTPUT"})) {
            keep_for_parameter(keyword, *unknown);
            return;
        }
        const std::optional<std::int64_t> frequency = output_frequency(keyword);
        if (!frequency) {
            return;
        }
        add_output(keyword, nodal, print ? "table" : "results", *frequency, set, parent, "");
    }

    // --- Meshes and sets of the Model IR ---------------------------------------------------------

    const Container& geometry_of(int place) const {
        if (place >= 0) {
            return containers_[instances_[static_cast<std::size_t>(place)].part];
        }
        if (place == place_assembly) {
            return containers_[*assembly_];
        }
        return containers_[0];
    }

    void finish_container(Container& container) {
        if (container.finished) {
            return;
        }
        container.finished = true;
        const auto refuse = [this](const char* key, Json args, const DataLine& line) {
            throw InpError(Problem{Problem::Severity::error, key, std::move(args),
                                   display(line.file), line.line});
        };
        if (const auto twice = container.nodes.build(container.node_ids)) {
            refuse("error.duplicateNode", {{"node", container.node_ids[*twice]}},
                   *container.node_lines[*twice]);
        }
        container.node_lines.clear();
        container.node_lines.shrink_to_fit();
        std::map<std::string_view, std::size_t> block_of;
        std::vector<std::vector<const DataLine*>> lines;  // Of the elements of each block.
        for (ElementGroup& group : container.groups) {
            const auto [it, added] =
                block_of.try_emplace(group.kind.abaqus, container.blocks.size());
            if (added) {
                container.blocks.push_back(
                    {std::string(group.kind.shape), std::string(group.kind.abaqus), {}, {}});
                container.block_kinds.push_back(group.kind);
                lines.emplace_back();
            }
            MeshBlock& block = container.blocks[it->second];
            block.ids.insert(block.ids.end(), group.labels.begin(), group.labels.end());
            lines[it->second].insert(lines[it->second].end(), group.lines.begin(),
                                     group.lines.end());
            const auto n = static_cast<std::size_t>(group.kind.nodes);
            block.connectivity.reserve(block.connectivity.size() + group.nodes.size());
            for (std::size_t i = 0; i < group.nodes.size(); ++i) {
                const std::optional<std::int32_t> node = container.nodes.find(group.nodes[i]);
                if (!node) {
                    refuse("error.unknownNode",
                           {{"node", group.nodes[i]}, {"element", group.labels[i / n]}},
                           *group.lines[i / n]);
                }
                block.connectivity.push_back(*node);
            }
            group = ElementGroup{group.kind, {}, {}, {}};  // Its memory is not needed any more.
        }
        container.block_offsets.push_back(0);
        std::vector<std::int64_t> ids;  // Element labels by global index.
        for (const MeshBlock& block : container.blocks) {
            container.block_offsets.push_back(container.block_offsets.back() +
                                              static_cast<std::int32_t>(block.ids.size()));
            ids.insert(ids.end(), block.ids.begin(), block.ids.end());
        }
        if (const auto twice = container.elements.build(ids)) {
            const std::size_t block = block_of_index(container, static_cast<std::int32_t>(*twice));
            refuse(
                "error.duplicateElement", {{"element", ids[*twice]}},
                *lines[block][*twice - static_cast<std::size_t>(container.block_offsets[block])]);
        }
    }

    /// The block of a global element index.
    static std::size_t block_of_index(const Container& container, std::int32_t element) {
        const auto it = std::upper_bound(container.block_offsets.begin(),
                                         container.block_offsets.end(), element);
        return static_cast<std::size_t>(it - container.block_offsets.begin()) - 1;
    }

    static std::array<double, 3> transform(const Instance& instance, std::array<double, 3> p) {
        for (std::size_t c = 0; c < 3; ++c) {
            p[c] += instance.translation[c];
        }
        if (!instance.rotated || instance.angle == 0.0) {
            return p;
        }
        // Rotation about the axis through a and b (Rodrigues' formula).
        std::array<double, 3> k{instance.b[0] - instance.a[0], instance.b[1] - instance.a[1],
                                instance.b[2] - instance.a[2]};
        const double length = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
        if (length == 0.0) {
            return p;
        }
        for (double& c : k) {
            c /= length;
        }
        const double angle = instance.angle * std::acos(-1.0) / 180.0;
        const double cosine = std::cos(angle);
        const double sine = std::sin(angle);
        const std::array<double, 3> v{p[0] - instance.a[0], p[1] - instance.a[1],
                                      p[2] - instance.a[2]};
        const double dot = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
        const std::array<double, 3> cross{k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2],
                                          k[0] * v[1] - k[1] * v[0]};
        std::array<double, 3> out{};
        for (std::size_t c = 0; c < 3; ++c) {
            out[c] = instance.a[c] + v[c] * cosine + cross[c] * sine + k[c] * dot * (1.0 - cosine);
        }
        return out;
    }

    MeshData mesh_of(const Scope& scope) const {
        const Container& container = containers_[scope.container];
        MeshData mesh;
        mesh.node_ids = container.node_ids;
        mesh.x = container.x;
        mesh.y = container.y;
        mesh.z = container.z;
        if (scope.place >= 0) {
            const Instance& instance = instances_[static_cast<std::size_t>(scope.place)];
            for (std::size_t i = 0; i < mesh.x.size(); ++i) {
                const std::array<double, 3> p =
                    transform(instance, {mesh.x[i], mesh.y[i], mesh.z[i]});
                mesh.x[i] = p[0];
                mesh.y[i] = p[1];
                mesh.z[i] = p[2];
            }
        }
        mesh.blocks = container.blocks;
        return mesh;
    }

    /// The sets of a set definition on a scope: nothing if it has no members there.
    std::optional<OutSet> out_set(const Scope& scope, const SetDef& set, int place) {
        const Container& container = containers_[scope.container];
        OutSet out;
        out.kind = set.kind;
        out.name = set.name;
        std::size_t undefined = 0;
        if (set.kind == SetKind::surface) {
            const auto it = set.faces.find(place);
            if (it == set.faces.end()) {
                return std::nullopt;
            }
            std::set<std::pair<std::int32_t, std::int32_t>> sides;
            for (const auto& [label, face] : it->second) {
                const std::optional<std::int32_t> element = container.elements.find(label);
                if (!element) {
                    ++undefined;
                    continue;
                }
                const std::size_t block = block_of_index(container, *element);
                const std::optional<std::int32_t> side =
                    neutral_side(container.blocks[block].shape, face);
                if (!side) {
                    ++undefined;
                    continue;
                }
                sides.emplace(*element, *side);
            }
            for (const auto& [element, side] : sides) {
                out.indices.push_back(element);
                out.indices.push_back(side);
            }
        } else {
            const auto it = set.labels.find(place);
            if (it == set.labels.end()) {
                return std::nullopt;
            }
            const LabelIndex& index =
                set.kind == SetKind::node ? container.nodes : container.elements;
            for (const std::int64_t label : it->second) {
                if (const std::optional<std::int32_t> found = index.find(label)) {
                    out.indices.push_back(*found);
                } else {
                    ++undefined;
                    out.kept.push_back(label);
                }
            }
            std::sort(out.indices.begin(), out.indices.end());
            out.indices.erase(std::unique(out.indices.begin(), out.indices.end()),
                              out.indices.end());
            std::sort(out.kept.begin(), out.kept.end());
            out.kept.erase(std::unique(out.kept.begin(), out.kept.end()), out.kept.end());
        }
        if (undefined > 0) {
            problems_.push_back(Problem{Problem::Severity::warning,
                                        "problem.undefinedMembers",
                                        {{"set", set.name},
                                         {"mesh", scope.name},
                                         {"count", static_cast<std::int64_t>(undefined)}},
                                        display(set.file),
                                        set.line});
        }
        return out;
    }

    void build_scopes() {
        for (Container& container : containers_) {
            finish_container(container);
        }
        const auto stem = [this] {
            const std::u8string text = file_.stem().u8string();
            return std::string(text.begin(), text.end());
        };
        if (containers_[0].has_geometry()) {
            scopes_.push_back({place_model, 0, stem(), {}, {}, no_item});
        }
        for (std::size_t i = 0; i < instances_.size(); ++i) {
            scopes_.push_back(
                {static_cast<int>(i), instances_[i].part, instances_[i].name, {}, {}, no_item});
        }
        if (assembly_ && containers_[*assembly_].has_geometry()) {
            scopes_.push_back(
                {place_assembly, *assembly_, containers_[*assembly_].name, {}, {}, no_item});
        }
        for (Scope& scope : scopes_) {
            std::array<std::set<std::string>, 3> taken;
            for (std::size_t s = 0; s < sets_.size(); ++s) {
                const SetDef& set = sets_[s];
                if (set.unsupported) {
                    continue;
                }
                // An instance has the sets of its part ("self") and those of the assembly on it;
                // the model and the assembly have their own sets.
                const bool own = scope.place >= 0 && set.container == scope.container;
                const bool on_it = scope.place >= 0 ? own || set.container == assembly_.value_or(0)
                                                    : set.container == scope.container;
                if (!on_it) {
                    continue;
                }
                const int place = own ? place_self : scope.place;
                std::optional<OutSet> out = out_set(scope, set, place);
                if (!out) {
                    continue;
                }
                std::set<std::string>& names = taken[static_cast<std::size_t>(set.kind)];
                if (!names.insert(upper(out->name)).second && !own) {
                    // An assembly set named like a set of the part.
                    const std::string renamed = containers_[set.container].name + "." + out->name;
                    problems_.push_back(
                        Problem{Problem::Severity::info,
                                "problem.renamedSet",
                                {{"set", out->name}, {"mesh", scope.name}, {"name", renamed}},
                                display(set.file),
                                set.line});
                    out->name = renamed;
                    names.insert(upper(renamed));
                }
                out->own = own;
                scope.by_set.emplace(s, scope.sets.size());
                scope.sets.push_back(std::move(*out));
            }
        }
    }

    /// Where a set is on the meshes: (scope, out set) pairs.
    std::vector<std::pair<std::size_t, std::size_t>> placed(const SetUse& use) const {
        std::vector<std::pair<std::size_t, std::size_t>> out;
        for (std::size_t s = 0; s < scopes_.size(); ++s) {
            if (use.place != any_place && scopes_[s].place != use.place) {
                continue;
            }
            if (const auto it = scopes_[s].by_set.find(use.set); it != scopes_[s].by_set.end()) {
                out.emplace_back(s, it->second);
            }
        }
        return out;
    }

    // --- Items -----------------------------------------------------------------------------------

    ItemRef add_item(Import& result, Item item) {
        result.items.push_back(std::move(item));
        return result.items.size() - 1;
    }

    Import finish() {
        build_scopes();
        // A section of a material the file does not define is kept as it is.
        for (const PendingSection& section : sections_) {
            if (!materials_by_name_.contains(upper(section.material))) {
                const Keyword& keyword = deck_.keywords[section.keyword];
                warn("problem.unknownReference",
                     {{"keyword", keyword.title()}, {"name", section.material}}, keyword);
                keep(keyword, keyword.text(), {ParentKind::input, 0}, section.context, false);
            }
        }
        Import result;
        // The file.
        {
            Item input;
            input.type = "abaqus_input";
            input.owner = module_owner;
            input.name = display(0);
            input.properties = {{"file", display(0)}};
            if (!heading_.empty()) {
                input.properties["heading"] = heading_;
            }
            // For the export (task 3.3): steps kept whole go back between the others.
            input.properties["steps"] = step_names_;
            if (assembly_) {
                input.properties["assembly"] = containers_[*assembly_].name;
            }
            result.input = add_item(result, std::move(input));
        }
        // Passthrough by parent, in the order of the file.
        std::stable_sort(
            passthrough_.begin(), passthrough_.end(),
            [](const Passthrough& a, const Passthrough& b) { return a.index < b.index; });
        const auto add_passthrough = [&](ParentKind kind, std::size_t index, ItemRef parent) {
            for (const Passthrough& block : passthrough_) {
                if (block.parent.kind != kind || block.parent.index != index) {
                    continue;
                }
                Item item;
                item.type = "abaqus_keyword";
                item.owner = module_owner;
                item.name = block.keyword + " (" + display(block.file) + ":" +
                            std::to_string(block.line) + ")";
                item.properties = block.context;
                item.properties["keyword"] = block.keyword;
                item.properties["text"] = block.text;
                item.properties["file"] = display(block.file);
                item.properties["line"] = block.line;
                item.properties["index"] = static_cast<std::int64_t>(block.index);
                if (kind == ParentKind::step) {
                    // How many objects of the step come before it: its place among them.
                    item.properties["position"] = static_cast<std::int64_t>(std::count_if(
                        pending_.begin(), pending_.end(), [&](const Pending& pending) {
                            return pending.parent.kind == ParentKind::step &&
                                   pending.parent.index == index && pending.order < block.index;
                        }));
                }
                item.parent = parent;
                add_item(result, std::move(item));
            }
        };
        // Materials.
        std::vector<ItemRef> material_items;
        for (std::size_t m = 0; m < materials_.size(); ++m) {
            Item item;
            item.type = "material";
            item.owner = structural_owner;
            item.name = materials_[m].name;
            item.properties = materials_[m].properties;
            material_items.push_back(add_item(result, std::move(item)));
            add_passthrough(ParentKind::material, m, material_items.back());
        }
        // Sections may split a set by integration: that makes sets, so it comes before the sets.
        struct SectionItem {
            Item item;
            std::size_t scope;
            std::size_t set;
        };
        std::vector<SectionItem> sections;
        for (const PendingSection& section : sections_) {
            const auto material = materials_by_name_.find(upper(section.material));
            if (material != materials_by_name_.end()) {
                plan_section(section, material_items[material->second], sections);
            }
        }
        // Meshes and their sets.
        for (Scope& scope : scopes_) {
            Item mesh;
            mesh.type = "mesh";
            mesh.owner = core_owner;
            mesh.name = scope.name;
            mesh.data = mesh_of(scope);
            scope.mesh = add_item(result, std::move(mesh));
            Item types;
            types.type = "abaqus_element_types";
            types.owner = module_owner;
            types.parent = scope.mesh;
            types.properties["blocks"] = Json::array();
            for (const MeshBlock& block : containers_[scope.container].blocks) {
                types.properties["blocks"].push_back(block.abaqus);
            }
            // Where the mesh was in the file, for the export (task 3.3).
            if (scope.place >= 0) {
                types.properties["part"] = containers_[scope.container].name;
                types.properties["instance"] = scope.name;
                types.properties["partSets"] = Json::array();
                for (const OutSet& set : scope.sets) {
                    if (set.own) {
                        types.properties["partSets"].push_back(set.name);
                    }
                }
            } else if (scope.place == place_assembly) {
                types.properties["assembly"] = true;
            }
            for (const OutSet& set : scope.sets) {
                if (!set.kept.empty()) {
                    const char* type = set.kind == SetKind::node ? "node_set " : "element_set ";
                    types.properties["keptMembers"][type + set.name] = set.kept;
                }
            }
            add_item(result, std::move(types));
            for (OutSet& set : scope.sets) {
                Item item;
                item.type = set.kind == SetKind::node      ? "node_set"
                            : set.kind == SetKind::element ? "element_set"
                                                           : "surface";
                item.owner = core_owner;
                item.name = set.name;
                item.parent = scope.mesh;
                item.data = SetData{set.kind, set.indices};
                set.item = add_item(result, std::move(item));
            }
        }
        for (SectionItem& section : sections) {
            section.item.dependencies.insert(section.item.dependencies.begin(),
                                             scopes_[section.scope].sets[section.set].item);
            add_item(result, std::move(section.item));
        }
        // Boundary conditions of all steps, then the steps with what acts in them.
        std::stable_sort(pending_.begin(), pending_.end(),
                         [](const Pending& a, const Pending& b) { return a.order < b.order; });
        const auto add_pending_items = [&](ParentKind kind, std::size_t index, ItemRef parent) {
            for (const Pending& pending : pending_) {
                if (pending.parent.kind != kind || pending.parent.index != index) {
                    continue;
                }
                Item item;
                item.type = pending.type;
                item.owner = structural_owner;
                item.name = pending.name;
                item.properties = pending.properties;
                item.parent = parent;
                for (const SetUse& use : pending.sets) {
                    for (const auto& [scope, set] : placed(use)) {
                        item.dependencies.push_back(scopes_[scope].sets[set].item);
                    }
                }
                if (!pending.sets.empty() && item.dependencies.empty()) {
                    problems_.push_back(Problem{Problem::Severity::warning,
                                                "problem.emptyTarget",
                                                {{"name", pending.name}},
                                                {},
                                                0});
                }
                add_item(result, std::move(item));
            }
        };
        add_pending_items(ParentKind::root, 0, no_item);
        for (std::size_t s = 0; s < steps_.size(); ++s) {
            Item step;
            step.type = "static_step";
            step.owner = structural_owner;
            step.name = steps_[s].name;
            step.properties = steps_[s].properties;
            const ItemRef item = add_item(result, std::move(step));
            // Objects and passthrough of a step in the order of the file.
            add_pending_items(ParentKind::step, s, item);
            add_passthrough(ParentKind::step, s, item);
        }
        add_passthrough(ParentKind::input, 0, result.input);

        for (const std::string& keyword : kept_order_) {
            Problem problem = kept_.at(keyword);
            const std::int64_t count = kept_count_.at(keyword);
            if (count > 1) {
                problem.key = "problem.passthroughRepeated";
                problem.args["count"] = count;
            }
            problems_.push_back(std::move(problem));
        }
        problems_.push_back(
            Problem{Problem::Severity::info, "problem.units", Json::object(), display(0), 0});
        result.problems = std::move(problems_);
        return result;
    }

    template <class SectionItem>
    void plan_section(const PendingSection& section, ItemRef material,
                      std::vector<SectionItem>& out) {
        const Keyword& keyword = deck_.keywords[section.keyword];
        const std::vector<std::pair<std::size_t, std::size_t>> where = placed(section.set);
        if (where.empty()) {
            problems_.push_back(Problem{Problem::Severity::warning,
                                        "problem.emptyTarget",
                                        {{"name", sets_[section.set.set].name}},
                                        display(keyword.file),
                                        keyword.line});
        }
        for (const std::pair<std::size_t, std::size_t>& placement : where) {
            // Plain variables: AppleClang does not let lambdas capture structured bindings.
            const std::size_t s = placement.first;
            const std::size_t o = placement.second;
            Scope& scope = scopes_[s];
            const Container& container = containers_[scope.container];
            // Integration and the kind of section from the element types.
            std::vector<std::int32_t> full;
            std::vector<std::int32_t> reduced;
            std::set<std::string> misfits;
            for (const std::int32_t element : scope.sets[o].indices) {
                const ElementKind& kind = container.block_kinds[block_of_index(container, element)];
                (kind.reduced ? reduced : full).push_back(element);
                if (kind.family != section.family) {
                    misfits.insert(std::string(kind.abaqus));
                }
            }
            for (const std::string& type : misfits) {
                problems_.push_back(Problem{
                    Problem::Severity::warning,
                    "problem.sectionFamily",
                    {{"keyword", keyword.title()}, {"set", scope.sets[o].name}, {"type", type}},
                    display(keyword.file),
                    keyword.line});
            }
            const bool shared_part =
                scope.place >= 0 &&
                std::count_if(instances_.begin(), instances_.end(),
                              [&](const Instance& i) { return i.part == scope.container; }) > 1;
            const std::string base =
                shared_part ? scope.name + "." + scope.sets[o].name : scope.sets[o].name;
            const auto add = [&](std::size_t set, const char* integration,
                                 const std::string& name) {
                SectionItem item;
                item.item.type = section.type;
                item.item.owner = structural_owner;
                item.item.name = name;
                item.item.properties = section.properties;
                if (section.type != "beam_section") {
                    item.item.properties["integration"] = integration;
                }
                item.item.dependencies.push_back(material);
                item.scope = s;
                item.set = set;
                out.push_back(std::move(item));
            };
            if (!full.empty() && !reduced.empty()) {
                const std::string name = scope.sets[o].name;
                const bool own = scope.sets[o].own;
                const std::size_t full_set = scope.sets.size();
                scope.sets.push_back({SetKind::element, name + "-full", full, no_item, own, {}});
                scope.sets.push_back(
                    {SetKind::element, name + "-reduced", reduced, no_item, own, {}});
                problems_.push_back(Problem{
                    Problem::Severity::warning,
                    "problem.mixedIntegration",
                    {{"set", name}, {"full", name + "-full"}, {"reduced", name + "-reduced"}},
                    display(keyword.file),
                    keyword.line});
                add(full_set, "full", base + "-full");
                add(full_set + 1, "reduced", base + "-reduced");
            } else {
                add(o, reduced.empty() ? "full" : "reduced", base);
            }
        }
    }

    Deck deck_;
    std::filesystem::path file_;
    std::vector<Problem> problems_;

    std::vector<Container> containers_;
    std::map<std::string, std::size_t> parts_;
    std::optional<std::size_t> assembly_;
    std::vector<Instance> instances_;
    std::map<std::string, int> instances_by_name_;
    std::vector<SetDef> sets_;
    std::vector<Material> materials_;
    std::map<std::string, std::size_t> materials_by_name_;
    std::vector<PendingSection> sections_;
    std::vector<Step> steps_;
    std::vector<std::string> step_names_;  // Of all steps, kept ones too, in the order of the file.
    std::vector<Pending> pending_;
    std::vector<Passthrough> passthrough_;
    std::string heading_;
    std::map<std::string, std::int64_t> implicit_;
    std::vector<Scope> scopes_;

    // Passthrough problems, one per keyword with the count.
    std::map<std::string, Problem> kept_;
    std::map<std::string, std::int64_t> kept_count_;
    std::vector<std::string> kept_order_;

    Region region_ = Region::model;
    std::size_t container_ = 0;
    int instance_ = -1;
    std::optional<std::size_t> material_;
    std::optional<std::size_t> step_;
};

}  // namespace

const char* to_string(Problem::Severity severity) noexcept {
    switch (severity) {
        case Problem::Severity::info:
            return "info";
        case Problem::Severity::warning:
            return "warning";
        case Problem::Severity::error:
            return "error";
    }
    return "warning";
}

InpError::InpError(Problem problem)
    : std::runtime_error(problem.key), problem_(std::move(problem)) {}

Import import_file(const std::filesystem::path& file, Encoding encoding) {
    Deck deck = read_deck(file, encoding);
    return Importer(std::move(deck), file).run();
}

}  // namespace fem::inp
