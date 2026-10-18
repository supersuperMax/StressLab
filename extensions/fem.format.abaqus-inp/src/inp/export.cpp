#include "inp/export.h"

#include "inp/deck.h"
#include "inp/elements.h"
#include "inp/numbers.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fem::inp {

using Json = nlohmann::json;

Model model_of(const Import& result) {
    Model model;
    model.units = {{"length", "m"}, {"mass", "kg"}, {"time", "s"}, {"temperature", "K"}};
    std::vector<std::vector<ItemRef>> children(result.items.size());
    std::vector<ItemRef> roots;
    for (ItemRef i = 0; i < result.items.size(); ++i) {
        const ItemRef parent = result.items[i].parent;
        (parent == no_item ? roots : children[parent]).push_back(i);
    }
    const std::function<void(ItemRef)> add = [&](ItemRef i) {
        const Item& item = result.items[i];
        ModelObject object;
        object.id = static_cast<std::int64_t>(i) + 1;
        object.type = item.type;
        object.owner = item.owner;
        object.name = item.name;
        object.parent = item.parent == no_item ? 0 : static_cast<std::int64_t>(item.parent) + 1;
        for (const ItemRef target : item.dependencies) {
            object.dependencies.push_back(static_cast<std::int64_t>(target) + 1);
        }
        std::sort(object.dependencies.begin(), object.dependencies.end());
        object.dependencies.erase(
            std::unique(object.dependencies.begin(), object.dependencies.end()),
            object.dependencies.end());
        object.properties = item.properties;
        if (const auto* mesh = std::get_if<MeshData>(&item.data)) {
            MeshView view{mesh->node_ids, mesh->x, mesh->y, mesh->z, {}};
            for (const MeshBlock& block : mesh->blocks) {
                view.blocks.push_back({block.shape, block.ids, block.connectivity});
            }
            object.data = std::move(view);
        } else if (const auto* set = std::get_if<SetData>(&item.data)) {
            object.data = SetView{set->kind, set->indices};
        }
        model.objects.push_back(std::move(object));
        for (const ItemRef child : children[i]) {
            add(child);
        }
    };
    for (const ItemRef root : roots) {
        add(root);
    }
    return model;
}

namespace {

constexpr std::string_view structural = "fem.model.structural";

struct DefaultType {
    std::string_view shape;
    std::string_view abaqus;
};

/// The Abaqus type of a shape whose mesh does not say it (made by a mesher, not imported).
constexpr std::array<DefaultType, 12> default_types{{
    {"tet4", "C3D4"},
    {"tet10", "C3D10"},
    {"hex8", "C3D8"},
    {"hex20", "C3D20"},
    {"wedge6", "C3D6"},
    {"wedge15", "C3D15"},
    {"tri3", "S3"},
    {"quad4", "S4"},
    {"tri6", "S6"},
    {"quad8", "S8R"},
    {"line2", "B31"},
    {"line3", "B32"},
}};

constexpr std::array<std::string_view, 6> displacement_keys{"ux", "uy", "uz", "rx", "ry", "rz"};
constexpr std::array<std::string_view, 6> load_keys{"fx", "fy", "fz", "mx", "my", "mz"};

/// Values per data line: Abaqus reads at most 16.
constexpr std::size_t per_line = 16;

/// A name as a parameter value or a data field: quoted if Abaqus would split it.
std::string as_name(std::string_view name) {
    if (name.find_first_of(" ,=*\t\"") == std::string_view::npos) {
        return std::string(name);
    }
    return "\"" + std::string(name) + "\"";
}

std::string number(const Json& value) {
    return format_number(value.is_number() ? value.get<double>() : 0.0);
}

const std::string* text_property(const ModelObject& object, std::string_view key) {
    const auto it = object.properties.find(key);
    return it != object.properties.end() && it->is_string() ? it->get_ptr<const std::string*>()
                                                            : nullptr;
}

struct MeshOut {
    const ModelObject* object = nullptr;
    const MeshView* view = nullptr;
    const ModelObject* types = nullptr;  ///< Its `abaqus_element_types`, if it has one.
    std::string part;                    ///< The *PART it is written as; empty if none.
    std::string source_part;             ///< The part it was an instance of in the file.
    std::string source_instance;         ///< Its instance in the file.
    bool assembly = false;               ///< The nodes and elements of the assembly itself.
    bool first_of_part = false;  ///< The first mesh of `source_part`: the part's keywords go here.
    std::set<std::string> part_sets;
    bool all_part_sets = false;
    std::vector<std::optional<ElementKind>> kinds;  ///< By block; nothing for a block not written.
    std::vector<std::int32_t> offsets;              ///< Global index of each block's first element.
    std::vector<const ModelObject*> sets;
};

struct SetOut {
    const ModelObject* object = nullptr;
    const SetView* view = nullptr;
    std::size_t mesh = 0;
    bool in_part = false;  ///< Written in the part of its mesh, not in the assembly.
};

class Writer {
public:
    Writer(const Model& model, std::string_view file_stem, bool text)
        : model_(model), file_stem_(file_stem), text_(text) {}

    Export run() {
        index();
        layout();
        heading();
        if (flat_) {
            if (!meshes_.empty()) {
                mesh_content(0);
                for (const ModelObject* set : meshes_[0].sets) {
                    write_set(sets_.at(set->id), false);
                }
            }
            for (const ModelObject* section : model_sections_) {
                write_section(*section, Area::model);
            }
        } else {
            parts();
            assembly();
        }
        keywords(model_keywords_);
        materials();
        for (const ModelObject* object : roots_) {
            if (object->type == "boundary_condition" && object->owner == structural) {
                (void)write_load(*object);
            }
        }
        steps();
        finish();
        return std::move(result_);
    }

private:
    enum class Area : std::uint8_t { model, part, assembly };

    // --- Text ------------------------------------------------------------------------------------

    void put(std::string_view line) {
        if (text_) {
            result_.text.append(line);
            result_.text.push_back('\n');
        }
    }

    void keyword(std::string_view line) {
        ++result_.keywords;
        put(line);
    }

    /// Values in data lines of at most 16; `continued` ends every line but the last with a comma,
    /// for an element whose nodes go on in the next line.
    void values(const std::vector<std::string>& fields, bool continued) {
        if (!text_) {
            return;
        }
        std::string line;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (i > 0 && i % per_line == 0) {
                put(continued ? line + "," : line);
                line.clear();
            } else if (i > 0) {
                line += ", ";
            }
            line += fields[i];
        }
        if (!line.empty()) {
            put(line);
        }
    }

    void loss(Problem::Severity severity, std::string key, Json args) {
        result_.losses.push_back(Problem{severity, std::move(key), std::move(args), {}, 0});
    }

    // --- The model -------------------------------------------------------------------------------

    void index() {
        for (const ModelObject& object : model_.objects) {
            by_id_.emplace(object.id, &object);
            if (object.parent == 0) {
                roots_.push_back(&object);
            } else {
                children_[object.parent].push_back(&object);
            }
        }
        for (const ModelObject& object : model_.objects) {
            if (object.type == "abaqus_input" && object.owner == module_owner) {
                inputs_.push_back(&object);
                handled_.insert(object.id);
            } else if (object.type == "abaqus_keyword" && object.owner == module_owner) {
                sort_keyword(object);
            }
        }
        const auto by_index = [](const ModelObject* a, const ModelObject* b) {
            return a->properties.value("index", std::int64_t{0}) <
                   b->properties.value("index", std::int64_t{0});
        };
        for (auto* list : {&model_keywords_, &assembly_keywords_, &kept_steps_}) {
            std::stable_sort(list->begin(), list->end(), by_index);
        }
        for (auto& [name, list] : part_keywords_) {
            std::stable_sort(list.begin(), list.end(), by_index);
        }
        for (auto& [name, list] : instance_keywords_) {
            std::stable_sort(list.begin(), list.end(), by_index);
        }
        for (auto& [id, list] : owned_keywords_) {
            std::stable_sort(
                list.begin(), list.end(), [](const ModelObject* a, const ModelObject* b) {
                    const auto key = [](const ModelObject* k) {
                        return std::pair{k->properties.value("position", std::int64_t{0}),
                                         k->properties.value("index", std::int64_t{0})};
                    };
                    return key(a) < key(b);
                });
        }
    }

    /// Where a kept keyword goes back: into its step or material, or by its place in the file.
    void sort_keyword(const ModelObject& object) {
        const auto parent = by_id_.find(object.parent);
        if (parent != by_id_.end() && parent->second->owner == structural &&
            (parent->second->type == "static_step" || parent->second->type == "material")) {
            owned_keywords_[object.parent].push_back(&object);
        } else if (object.properties.contains("step")) {
            kept_steps_.push_back(&object);
        } else if (const std::string* instance = text_property(object, "instance")) {
            instance_keywords_[*instance].push_back(&object);
        } else if (const std::string* part = text_property(object, "part")) {
            part_keywords_[*part].push_back(&object);
        } else if (object.properties.contains("assembly")) {
            assembly_keywords_.push_back(&object);
        } else {
            model_keywords_.push_back(&object);
        }
    }

    [[nodiscard]] const ModelObject* child_of_type(const ModelObject& parent,
                                                   std::string_view type) const {
        const auto it = children_.find(parent.id);
        if (it == children_.end()) {
            return nullptr;
        }
        for (const ModelObject* child : it->second) {
            if (child->type == type && child->owner == module_owner) {
                return child;
            }
        }
        return nullptr;
    }

    std::optional<ElementKind> kind_of(const MeshOut& mesh, std::size_t block) {
        const MeshBlockView& view = mesh.view->blocks[block];
        if (mesh.types != nullptr) {
            const Json& types = mesh.types->properties.value("blocks", Json::array());
            if (types.is_array() && types.size() == mesh.view->blocks.size() &&
                types[block].is_string()) {
                const std::optional<ElementKind> kind =
                    element_kind(upper(types[block].get<std::string>()));
                if (kind && kind->shape == view.shape) {
                    return kind;
                }
            }
        }
        for (const DefaultType& type : default_types) {
            if (type.shape == view.shape) {
                return element_kind(type.abaqus);
            }
        }
        loss(Problem::Severity::warning, "loss.elementType",
             {{"mesh", mesh.object->name},
              {"shape", view.shape},
              {"count", static_cast<std::int64_t>(view.ids.size())}});
        return std::nullopt;
    }

    void layout() {
        const ModelObject* input = inputs_.empty() ? nullptr : inputs_.front();
        for (const ModelObject& object : model_.objects) {
            if (object.type != "mesh" || object.owner != "fem.core" ||
                !std::holds_alternative<MeshView>(object.data)) {
                continue;
            }
            MeshOut mesh;
            mesh.object = &object;
            mesh.view = &std::get<MeshView>(object.data);
            mesh.types = child_of_type(object, "abaqus_element_types");
            if (mesh.types != nullptr) {
                handled_.insert(mesh.types->id);
            }
            mesh_index_.emplace(object.id, meshes_.size());
            meshes_.push_back(std::move(mesh));
        }
        flat_ = meshes_.size() <= 1;
        for (const MeshOut& mesh : meshes_) {
            if (mesh.types != nullptr && (mesh.types->properties.contains("part") ||
                                          mesh.types->properties.value("assembly", false))) {
                flat_ = false;
            }
        }
        if (input != nullptr && text_property(*input, "assembly") != nullptr) {
            assembly_ = *text_property(*input, "assembly");
        }
        std::set<std::string> parts;  // Upper case: Abaqus names are not case-sensitive.
        bool assembly_mesh = false;
        for (std::size_t m = 0; m < meshes_.size(); ++m) {
            MeshOut& mesh = meshes_[m];
            const Json& types =
                mesh.types != nullptr ? mesh.types->properties : static_cast<const Json&>(empty_);
            handled_.insert(mesh.object->id);
            if (!flat_ && types.value("assembly", false) && !assembly_mesh) {
                mesh.assembly = true;
                assembly_mesh = true;
                if (assembly_.empty()) {
                    assembly_ = mesh.object->name;
                }
            } else if (!flat_) {
                mesh.source_part = types.value("part", mesh.object->name);
                mesh.source_instance = types.value("instance", mesh.object->name);
                std::string part = mesh.source_part;
                if (parts.contains(upper(part))) {
                    loss(Problem::Severity::warning, "loss.sharedPart",
                         {{"part", mesh.source_part}, {"instance", mesh.object->name}});
                    part = mesh.object->name;
                    for (int n = 2; parts.contains(upper(part)); ++n) {
                        part = mesh.object->name + "-" + std::to_string(n);
                    }
                } else {
                    mesh.first_of_part = true;
                }
                parts.insert(upper(part));
                mesh.part = part;
                if (const auto it = types.find("partSets"); it != types.end() && it->is_array()) {
                    for (const Json& name : *it) {
                        if (name.is_string()) {
                            mesh.part_sets.insert(name.get<std::string>());
                        }
                    }
                } else {
                    mesh.all_part_sets = true;  // Sets of a mesh made here belong to its part.
                }
            }
            mesh.offsets.push_back(0);
            for (std::size_t b = 0; b < mesh.view->blocks.size(); ++b) {
                mesh.kinds.push_back(kind_of(mesh, b));
                mesh.offsets.push_back(mesh.offsets.back() +
                                       static_cast<std::int32_t>(mesh.view->blocks[b].ids.size()));
            }
            if (const auto it = children_.find(mesh.object->id); it != children_.end()) {
                for (const ModelObject* child : it->second) {
                    if (child->owner != "fem.core" ||
                        !std::holds_alternative<SetView>(child->data)) {
                        continue;
                    }
                    SetOut set;
                    set.object = child;
                    set.view = &std::get<SetView>(child->data);
                    set.mesh = m;
                    set.in_part = !flat_ && !mesh.assembly &&
                                  (mesh.all_part_sets || mesh.part_sets.contains(child->name));
                    sets_.emplace(child->id, set);
                    mesh.sets.push_back(child);
                    handled_.insert(child->id);
                }
            }
        }
        if (!flat_ && assembly_.empty()) {
            assembly_ = "Assembly";
        }
        if (flat_ && !meshes_.empty() && meshes_[0].object->name != file_stem_) {
            // The import names the mesh of a file without parts after the file.
            loss(Problem::Severity::info, "loss.meshName",
                 {{"mesh", meshes_[0].object->name}, {"file", std::string(file_stem_)}});
        }
        // Sections go where their set is.
        for (const ModelObject* object : roots_) {
            if (object->owner == structural &&
                (object->type == "solid_section" || object->type == "shell_section" ||
                 object->type == "beam_section")) {
                const SetOut* set = element_set_of(*object);
                if (set == nullptr || material_of(*object) == nullptr) {
                    loss(Problem::Severity::warning, "loss.noTarget", {{"name", object->name}});
                    handled_.insert(object->id);
                    continue;
                }
                check_integration(*object, *set);
                if (flat_) {
                    model_sections_.push_back(object);
                } else if (set->in_part) {
                    part_sections_[set->mesh].push_back(object);
                } else {
                    assembly_sections_.push_back(object);
                }
            }
        }
    }

    [[nodiscard]] const SetOut* element_set_of(const ModelObject& object) const {
        for (const std::int64_t id : object.dependencies) {
            const auto it = sets_.find(id);
            if (it != sets_.end() && it->second.view->kind == SetKind::element) {
                return &it->second;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const ModelObject* material_of(const ModelObject& object) const {
        for (const std::int64_t id : object.dependencies) {
            const auto it = by_id_.find(id);
            if (it != by_id_.end() && it->second->type == "material" &&
                it->second->owner == structural) {
                return it->second;
            }
        }
        return nullptr;
    }

    /// The import takes the integration of a section from the element types of its set: if the
    /// section says otherwise, the file cannot keep it.
    void check_integration(const ModelObject& section, const SetOut& set) {
        if (section.type == "beam_section") {
            return;
        }
        const bool reduced =
            section.properties.value("integration", std::string("full")) == std::string("reduced");
        const MeshOut& mesh = meshes_[set.mesh];
        for (const std::int32_t element : set.view->indices) {
            const std::optional<ElementKind>& kind = mesh.kinds[block_of(mesh, element)];
            if (kind && kind->reduced != reduced) {
                loss(Problem::Severity::warning, "loss.integration",
                     {{"name", section.name}, {"type", std::string(kind->abaqus)}});
                return;
            }
        }
    }

    static std::size_t block_of(const MeshOut& mesh, std::int32_t element) {
        const auto it = std::upper_bound(mesh.offsets.begin(), mesh.offsets.end(), element);
        return static_cast<std::size_t>(it - mesh.offsets.begin()) - 1;
    }

    // --- Names -----------------------------------------------------------------------------------

    /// The name a set has in the file: a set of the assembly the import renamed because a set of
    /// the part had its name ("Assembly.Set-1") gets its name back.
    [[nodiscard]] std::string written_name(const SetOut& set) const {
        const std::string& name = set.object->name;
        const std::string prefix = assembly_ + ".";
        if (!flat_ && !set.in_part && !assembly_.empty() && name.size() > prefix.size() &&
            upper(name.substr(0, prefix.size())) == upper(prefix)) {
            return name.substr(prefix.size());
        }
        return name;
    }

    /// The name of a set where loads and steps refer to it (model and history data).
    [[nodiscard]] std::string reference(const SetOut& set) const {
        if (set.in_part) {
            return as_name(meshes_[set.mesh].object->name + "." + set.object->name);
        }
        return as_name(written_name(set));
    }

    /// Distinct references of the sets an object depends on, of `kinds`, in order.
    [[nodiscard]] std::vector<std::pair<std::string, const SetOut*>> targets(
        const ModelObject& object, std::initializer_list<SetKind> kinds) const {
        std::vector<std::pair<std::string, const SetOut*>> out;
        for (const std::int64_t id : object.dependencies) {
            const auto it = sets_.find(id);
            if (it == sets_.end() ||
                std::find(kinds.begin(), kinds.end(), it->second.view->kind) == kinds.end()) {
                continue;
            }
            std::string name = reference(it->second);
            if (std::none_of(out.begin(), out.end(),
                             [&](const auto& t) { return t.first == name; })) {
                out.emplace_back(std::move(name), &it->second);
            }
        }
        return out;
    }

    // --- Meshes and sets -------------------------------------------------------------------------

    void heading() {
        for (const ModelObject* input : inputs_) {
            if (const std::string* text = text_property(*input, "heading")) {
                keyword("*HEADING");
                std::size_t start = 0;
                while (start <= text->size()) {
                    const std::size_t end = std::min(text->find('\n', start), text->size());
                    put(std::string_view(*text).substr(start, end - start));
                    start = end + 1;
                }
                return;
            }
        }
    }

    void mesh_content(std::size_t m) {
        const MeshOut& mesh = meshes_[m];
        const MeshView& view = *mesh.view;
        if (!view.node_ids.empty()) {
            keyword("*NODE");
            if (text_) {
                for (std::size_t i = 0; i < view.node_ids.size(); ++i) {
                    put(std::to_string(view.node_ids[i]) + ", " + format_number(view.x[i]) + ", " +
                        format_number(view.y[i]) + ", " + format_number(view.z[i]));
                }
            }
        }
        for (std::size_t b = 0; b < view.blocks.size(); ++b) {
            if (!mesh.kinds[b]) {
                continue;
            }
            const MeshBlockView& block = view.blocks[b];
            keyword("*ELEMENT, TYPE=" + std::string(mesh.kinds[b]->abaqus));
            if (!text_) {
                continue;
            }
            const auto n = static_cast<std::size_t>(mesh.kinds[b]->nodes);
            std::vector<std::string> fields;
            for (std::size_t e = 0; e < block.ids.size(); ++e) {
                fields.clear();
                fields.push_back(std::to_string(block.ids[e]));
                for (std::size_t k = 0; k < n; ++k) {
                    fields.push_back(std::to_string(
                        view.node_ids[static_cast<std::size_t>(block.connectivity[e * n + k])]));
                }
                values(fields, true);
            }
        }
    }

    /// Labels of the members of a node or element set, sorted; elements of blocks not written are
    /// left out.
    [[nodiscard]] std::vector<std::int64_t> labels(const SetOut& set) const {
        const MeshOut& mesh = meshes_[set.mesh];
        std::vector<std::int64_t> out;
        out.reserve(set.view->indices.size());
        for (const std::int32_t index : set.view->indices) {
            if (set.view->kind == SetKind::node) {
                out.push_back(mesh.view->node_ids[static_cast<std::size_t>(index)]);
                continue;
            }
            const std::size_t block = block_of(mesh, index);
            if (mesh.kinds[block]) {
                out.push_back(mesh.view->blocks[block]
                                  .ids[static_cast<std::size_t>(index - mesh.offsets[block])]);
            }
        }
        const std::vector<std::int64_t> kept = kept_members(set);
        out.insert(out.end(), kept.begin(), kept.end());
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    /// Members the import found in the file but not in the mesh: elements of types kept in
    /// passthrough. They go back into the set, so that the kept *ELEMENT is in it again.
    [[nodiscard]] std::vector<std::int64_t> kept_members(const SetOut& set) const {
        const MeshOut& mesh = meshes_[set.mesh];
        if (mesh.types == nullptr) {
            return {};
        }
        const auto all = mesh.types->properties.find("keptMembers");
        if (all == mesh.types->properties.end() || !all->is_object()) {
            return {};
        }
        const auto found = all->find(set.object->type + " " + set.object->name);
        std::vector<std::int64_t> out;
        if (found != all->end() && found->is_array()) {
            for (const Json& label : *found) {
                if (label.is_number_integer()) {
                    out.push_back(label.get<std::int64_t>());
                }
            }
        }
        return out;
    }

    /// Writes a set. `in_assembly`: a set of an instance mesh written in the assembly.
    void write_set(const SetOut& set, bool in_assembly) {
        const MeshOut& mesh = meshes_[set.mesh];
        const std::string name = as_name(written_name(set));
        if (set.view->indices.empty() &&
            (set.view->kind == SetKind::surface || kept_members(set).empty())) {
            loss(Problem::Severity::warning, "loss.emptySet", {{"name", set.object->name}});
            return;
        }
        const std::string instance =
            in_assembly && !mesh.assembly ? as_name(mesh.object->name) : std::string();
        if (set.view->kind == SetKind::surface) {
            write_surface(set, name, instance);
            return;
        }
        const bool nodes = set.view->kind == SetKind::node;
        std::string line = nodes ? "*NSET, NSET=" + name : "*ELSET, ELSET=" + name;
        if (!instance.empty()) {
            line += ", INSTANCE=" + instance;
        }
        if (!text_) {
            keyword(line);
            return;
        }
        const std::vector<std::int64_t> members = labels(set);
        // An arithmetic series of labels is written as GENERATE.
        const bool series =
            members.size() >= 3 && std::adjacent_find(members.begin() + 1, members.end(),
                                                      [&](std::int64_t a, std::int64_t b) {
                                                          return b - a != members[1] - members[0];
                                                      }) == members.end();
        if (series) {
            keyword(line + ", GENERATE");
            put(std::to_string(members.front()) + ", " + std::to_string(members.back()) + ", " +
                std::to_string(members[1] - members[0]));
            return;
        }
        keyword(line);
        std::vector<std::string> fields;
        fields.reserve(members.size());
        for (const std::int64_t label : members) {
            fields.push_back(std::to_string(label));
        }
        values(fields, false);
    }

    void write_surface(const SetOut& set, const std::string& name, const std::string& instance) {
        const MeshOut& mesh = meshes_[set.mesh];
        const std::span<const std::int32_t> pairs = set.view->indices;
        std::vector<std::string> lines;
        std::size_t lost = 0;
        for (std::size_t p = 0; p + 1 < pairs.size(); p += 2) {
            const std::size_t block = block_of(mesh, pairs[p]);
            std::optional<int> face;
            for (int f = 1; f <= 6 && mesh.kinds[block]; ++f) {
                if (neutral_side(mesh.view->blocks[block].shape, f) == pairs[p + 1]) {
                    face = f;
                    break;
                }
            }
            if (!face) {
                ++lost;
                continue;
            }
            const std::int64_t label =
                mesh.view->blocks[block]
                    .ids[static_cast<std::size_t>(pairs[p] - mesh.offsets[block])];
            lines.push_back((instance.empty() ? "" : instance + ".") + std::to_string(label) +
                            ", S" + std::to_string(*face));
        }
        if (lost > 0) {
            loss(Problem::Severity::warning, "loss.surfaceSide",
                 {{"name", set.object->name}, {"count", static_cast<std::int64_t>(lost)}});
        }
        if (lines.empty()) {
            return;
        }
        keyword("*SURFACE, TYPE=ELEMENT, NAME=" + name);
        for (const std::string& line : lines) {
            put(line);
        }
    }

    // --- Parts and the assembly ------------------------------------------------------------------

    void parts() {
        for (std::size_t m = 0; m < meshes_.size(); ++m) {
            const MeshOut& mesh = meshes_[m];
            if (mesh.assembly) {
                continue;
            }
            keyword("*PART, NAME=" + as_name(mesh.part));
            mesh_content(m);
            for (const ModelObject* object : mesh.sets) {
                const SetOut& set = sets_.at(object->id);
                if (set.in_part) {
                    write_set(set, false);
                }
            }
            for (const ModelObject* section : part_sections_[m]) {
                write_section(*section, Area::part);
            }
            if (mesh.first_of_part) {
                if (const auto it = part_keywords_.find(mesh.source_part);
                    it != part_keywords_.end()) {
                    keywords(it->second);
                }
            }
            keyword("*END PART");
        }
    }

    void assembly() {
        keyword("*ASSEMBLY, NAME=" + as_name(assembly_));
        for (const MeshOut& mesh : meshes_) {
            if (mesh.assembly) {
                continue;
            }
            // The coordinates are those of the assembly already: no translation or rotation.
            keyword("*INSTANCE, NAME=" + as_name(mesh.object->name) +
                    ", PART=" + as_name(mesh.part));
            if (const auto it = instance_keywords_.find(mesh.source_instance);
                it != instance_keywords_.end()) {
                keywords(it->second);
            }
            keyword("*END INSTANCE");
        }
        for (std::size_t m = 0; m < meshes_.size(); ++m) {
            if (meshes_[m].assembly) {
                mesh_content(m);
            }
        }
        for (const MeshOut& mesh : meshes_) {
            for (const ModelObject* object : mesh.sets) {
                const SetOut& set = sets_.at(object->id);
                if (!set.in_part) {
                    write_set(set, true);
                }
            }
        }
        for (const ModelObject* section : assembly_sections_) {
            write_section(*section, Area::assembly);
        }
        keywords(assembly_keywords_);
        keyword("*END ASSEMBLY");
    }

    // --- Kept keywords ---------------------------------------------------------------------------

    void kept(const ModelObject& object) {
        handled_.insert(object.id);
        const std::string* text = text_property(object, "text");
        if (text == nullptr) {
            return;
        }
        std::size_t start = 0;
        while (start <= text->size()) {
            const std::size_t end = std::min(text->find('\n', start), text->size());
            const std::string_view line = std::string_view(*text).substr(start, end - start);
            if (line.starts_with('*') && !line.starts_with("**")) {
                ++result_.keywords;
            }
            put(line);
            start = end + 1;
        }
    }

    void keywords(const std::vector<const ModelObject*>& list) {
        for (const ModelObject* object : list) {
            kept(*object);
        }
    }

    // --- Materials and sections ------------------------------------------------------------------

    void materials() {
        for (const ModelObject* object : roots_) {
            if (object->type != "material" || object->owner != structural) {
                continue;
            }
            handled_.insert(object->id);
            const Json& p = object->properties;
            keyword("*MATERIAL, NAME=" + as_name(object->name));
            if (p.contains("youngsModulus") || p.contains("poissonsRatio")) {
                keyword("*ELASTIC");
                put(number(p.value("youngsModulus", Json(0.0))) + ", " +
                    number(p.value("poissonsRatio", Json(0.0))));
            }
            if (p.contains("density")) {
                keyword("*DENSITY");
                put(number(p["density"]));
            }
            if (const auto it = owned_keywords_.find(object->id); it != owned_keywords_.end()) {
                keywords(it->second);
            }
        }
    }

    void write_section(const ModelObject& section, Area area) {
        handled_.insert(section.id);
        const SetOut& set = *element_set_of(section);
        const ModelObject& material = *material_of(section);
        // In a part its own name; in the assembly the name there.
        const std::string elset =
            area == Area::part ? as_name(set.object->name) : as_name(written_name(set));
        const std::string common = ", ELSET=" + elset + ", MATERIAL=" + as_name(material.name);
        const Json& p = section.properties;
        if (section.type == "solid_section") {
            keyword("*SOLID SECTION" + common);
        } else if (section.type == "shell_section") {
            std::string line = "*SHELL SECTION" + common;
            if (const auto it = p.find("offset"); it != p.end() && it->is_number()) {
                const double offset = it->get<double>();
                line += ", OFFSET=" + (offset == 0.5    ? std::string("SPOS")
                                       : offset == -0.5 ? std::string("SNEG")
                                       : offset == 0.0  ? std::string("MIDDLE")
                                                        : format_number(offset));
            }
            keyword(line);
            std::string data = number(p.value("thickness", Json(0.0)));
            if (p.contains("integrationPoints")) {
                data += ", " + std::to_string(p["integrationPoints"].get<std::int64_t>());
            }
            put(data);
        } else {
            const std::string shape = p.value("shape", std::string());
            std::string data;
            if (shape == "rectangle") {
                keyword("*BEAM SECTION" + common + ", SECTION=RECT");
                data = number(p.value("width", Json(0.0))) + ", " +
                       number(p.value("height", Json(0.0)));
            } else if (shape == "circle") {
                keyword("*BEAM SECTION" + common + ", SECTION=CIRC");
                data = number(p.value("radius", Json(0.0)));
            } else {
                keyword("*BEAM SECTION" + common + ", SECTION=PIPE");
                data = number(p.value("radius", Json(0.0))) + ", " +
                       number(p.value("wallThickness", Json(0.0)));
            }
            put(data);
            if (const auto it = p.find("direction"); it != p.end() && it->is_object()) {
                put(number(it->value("x", Json(0.0))) + ", " + number(it->value("y", Json(0.0))) +
                    ", " + number(it->value("z", Json(-1.0))));
            }
        }
    }

    // --- Loads, boundary conditions, output ------------------------------------------------------

    /// Writes an object of a step (or a boundary condition of the model). Returns how many objects
    /// the import makes of what was written, for the places of the kept keywords.
    std::size_t write_load(const ModelObject& object) {
        handled_.insert(object.id);
        const Json& p = object.properties;
        const auto no_target = [&] {
            loss(Problem::Severity::warning, "loss.noTarget", {{"name", object.name}});
            return std::size_t{0};
        };
        const auto split = [&](std::size_t count) {
            if (count > 1) {
                loss(Problem::Severity::info, "loss.split",
                     {{"name", object.name}, {"count", static_cast<std::int64_t>(count)}});
            }
            return count;
        };
        if (object.type == "boundary_condition") {
            const auto to = targets(object, {SetKind::node});
            if (to.empty()) {
                return no_target();
            }
            keyword("*BOUNDARY");
            for (const auto& target : to) {
                // Consecutive degrees of freedom with one value share a line.
                for (std::size_t d = 0; d < displacement_keys.size(); ++d) {
                    const auto value = p.find(displacement_keys[d]);
                    if (value == p.end() || !value->is_number()) {
                        continue;
                    }
                    std::size_t last = d;
                    while (last + 1 < displacement_keys.size()) {
                        const auto next = p.find(displacement_keys[last + 1]);
                        if (next == p.end() || !next->is_number() ||
                            next->get<double>() != value->get<double>()) {
                            break;
                        }
                        ++last;
                    }
                    std::string line = target.first + ", " + std::to_string(d + 1) + ", " +
                                       std::to_string(last + 1);
                    if (value->get<double>() != 0.0) {
                        line += ", " + number(*value);
                    }
                    put(line);
                    d = last;
                }
            }
            return split(to.size());
        }
        if (object.type == "concentrated_load") {
            const auto to = targets(object, {SetKind::node});
            if (to.empty()) {
                return no_target();
            }
            keyword("*CLOAD");
            for (const auto& target : to) {
                for (std::size_t d = 0; d < load_keys.size(); ++d) {
                    if (const auto value = p.find(load_keys[d]);
                        value != p.end() && value->is_number()) {
                        put(target.first + ", " + std::to_string(d + 1) + ", " + number(*value));
                    }
                }
            }
            return split(to.size());
        }
        if (object.type == "pressure") {
            const auto to = targets(object, {SetKind::surface, SetKind::element});
            if (to.empty()) {
                return no_target();
            }
            const std::string magnitude = number(p.value("magnitude", Json(0.0)));
            for (const bool surface : {true, false}) {
                bool started = false;
                for (const auto& target : to) {
                    if ((target.second->view->kind == SetKind::surface) != surface) {
                        continue;
                    }
                    if (!started) {
                        keyword(surface ? "*DSLOAD" : "*DLOAD");
                        started = true;
                    }
                    put(target.first + ", P, " + magnitude);
                }
            }
            return split(to.size());
        }
        if (object.type == "gravity") {
            const auto to = targets(object, {SetKind::element});
            const Json direction = p.value("direction", Json::object());
            const std::string data = ", GRAV, " + number(p.value("acceleration", Json(0.0))) +
                                     ", " + number(direction.value("x", Json(0.0))) + ", " +
                                     number(direction.value("y", Json(0.0))) + ", " +
                                     number(direction.value("z", Json(-1.0)));
            keyword("*DLOAD");
            if (to.empty()) {
                put(data);  // The whole model.
                return 1;
            }
            for (const auto& target : to) {
                put(target.first + data);
            }
            return split(to.size());
        }
        return write_output(object);
    }

    std::size_t write_output(const ModelObject& object) {
        const Json& p = object.properties;
        const bool u = p.value("displacement", true);
        const bool rf = p.value("reactionForce", true);
        const bool s = p.value("stress", true);
        const bool e = p.value("strain", false);
        const bool table = p.value("destination", std::string("results")) == std::string("table");
        const std::int64_t frequency = p.value("frequency", std::int64_t{1});
        const std::string every =
            frequency != 1 ? ", FREQUENCY=" + std::to_string(frequency) : std::string();
        const auto nodes = targets(object, {SetKind::node});
        const auto elements = targets(object, {SetKind::element});
        if (!u && !rf && !s && !e) {
            loss(Problem::Severity::warning, "loss.emptyOutput", {{"name", object.name}});
            return 0;
        }
        if (!table && u && rf && s && e && nodes.empty() && elements.empty()) {
            keyword("*OUTPUT, FIELD, VARIABLE=PRESELECT" + every);
            return 1;
        }
        const bool nodal = u || rf;
        const bool element = s || e;
        if (nodal && element) {
            loss(Problem::Severity::info, "loss.split", {{"name", object.name}, {"count", 2}});
        }
        if (nodes.size() > 1 || elements.size() > 1) {
            loss(Problem::Severity::warning, "loss.outputTargets", {{"name", object.name}});
        }
        const std::string nset = nodes.empty() ? std::string() : ", NSET=" + nodes.front().first;
        const std::string elset =
            elements.empty() ? std::string() : ", ELSET=" + elements.front().first;
        const std::string node_variables = u && rf ? "U, RF" : u ? "U" : "RF";
        const std::string element_variables = s && e ? "S, E" : s ? "S" : "E";
        if (table) {
            if (nodal) {
                keyword("*NODE PRINT" + nset + every);
                put(node_variables);
            }
            if (element) {
                keyword("*EL PRINT" + elset + every);
                put(element_variables);
            }
        } else {
            keyword("*OUTPUT, FIELD" + every);
            if (nodal) {
                keyword("*NODE OUTPUT" + nset);
                put(node_variables);
            }
            if (element) {
                keyword("*ELEMENT OUTPUT" + elset);
                put(element_variables);
            }
        }
        return (nodal ? 1U : 0U) + (element ? 1U : 0U);
    }

    // --- Steps -----------------------------------------------------------------------------------

    void steps() {
        std::vector<const ModelObject*> ir;
        for (const ModelObject* object : roots_) {
            if (object->type == "static_step" && object->owner == structural) {
                ir.push_back(object);
            } else if (object->owner == structural &&
                       (object->type == "concentrated_load" || object->type == "pressure" ||
                        object->type == "gravity" || object->type == "output_request")) {
                loss(Problem::Severity::warning, "loss.outsideStep", {{"name", object->name}});
                handled_.insert(object->id);
            }
        }
        std::set<const ModelObject*> done;
        // The order of the file first, kept steps among the others; then steps made here.
        for (const ModelObject* input : inputs_) {
            const Json& order = input->properties.value("steps", Json::array());
            for (const Json& name : order) {
                if (!name.is_string()) {
                    continue;
                }
                const std::string& wanted = name.get_ref<const std::string&>();
                if (const auto it = std::find_if(ir.begin(), ir.end(),
                                                 [&](const ModelObject* step) {
                                                     return !done.contains(step) &&
                                                            step->name == wanted;
                                                 });
                    it != ir.end()) {
                    done.insert(*it);
                    write_step(**it);
                } else if (const auto kept_step = std::find_if(
                               kept_steps_.begin(), kept_steps_.end(),
                               [&](const ModelObject* k) {
                                   return !done.contains(k) &&
                                          k->properties.value("step", std::string()) == wanted;
                               });
                           kept_step != kept_steps_.end()) {
                    done.insert(*kept_step);
                    kept(**kept_step);
                }
            }
        }
        for (const ModelObject* step : ir) {
            if (!done.contains(step)) {
                write_step(*step);
            }
        }
        for (const ModelObject* k : kept_steps_) {
            if (!done.contains(k)) {
                kept(*k);
            }
        }
    }

    void write_step(const ModelObject& step) {
        handled_.insert(step.id);
        const Json& p = step.properties;
        std::string line = "*STEP, NAME=" + as_name(step.name);
        if (p.contains("maxIncrements")) {
            line += ", INC=" + std::to_string(p["maxIncrements"].get<std::int64_t>());
        }
        keyword(line);
        keyword("*STATIC");
        if (p.contains("timePeriod") || p.contains("initialIncrement")) {
            const Json period = p.value("timePeriod", Json(1.0));
            put(number(p.value("initialIncrement", period)) + ", " + number(period));
        }
        static const std::set<std::string_view> loads{"boundary_condition", "concentrated_load",
                                                      "pressure", "gravity", "output_request"};
        const std::vector<const ModelObject*>& kept_here = owned_keywords_[step.id];
        std::size_t next = 0;
        std::int64_t written = 0;
        const auto kept_up_to = [&](std::int64_t count) {
            while (next < kept_here.size() &&
                   kept_here[next]->properties.value("position", std::int64_t{0}) <= count) {
                kept(*kept_here[next++]);
            }
        };
        if (const auto it = children_.find(step.id); it != children_.end()) {
            for (const ModelObject* child : it->second) {
                if (child->owner != structural || !loads.contains(child->type)) {
                    continue;
                }
                kept_up_to(written);
                written += static_cast<std::int64_t>(write_load(*child));
            }
        }
        while (next < kept_here.size()) {
            kept(*kept_here[next++]);
        }
        keyword("*END STEP");
    }

    // --- Losses ----------------------------------------------------------------------------------

    void finish() {
        std::map<std::string, std::int64_t> skipped;  // By type.
        std::int64_t results = 0;
        for (const ModelObject& object : model_.objects) {
            if (handled_.contains(object.id) || covered(object)) {
                continue;
            }
            if (object.type == "abaqus_keyword" && object.owner == module_owner) {
                loss(Problem::Severity::warning, "loss.droppedKeyword",
                     {{"keyword", object.properties.value("keyword", std::string())},
                      {"line", object.properties.value("line", std::int64_t{0})},
                      {"file", object.properties.value("file", std::string())}});
            } else if (object.owner == structural && object.type.starts_with("result_")) {
                ++results;
            } else {
                ++skipped[object.type];
            }
        }
        for (const auto& [type, count] : skipped) {
            loss(Problem::Severity::warning, "loss.notExported",
                 {{"type", type}, {"count", count}});
        }
        if (results > 0) {
            loss(Problem::Severity::info, "loss.results", {{"count", results}});
        }
        std::string units;
        for (const char* key : {"length", "mass", "time", "temperature"}) {
            units += (units.empty() ? "" : ", ") + model_.units.value(key, std::string("?"));
        }
        loss(Problem::Severity::info, "loss.units", {{"units", units}});
    }

    /// An object inside something the losses already name: its parent was not written.
    [[nodiscard]] bool covered(const ModelObject& object) const {
        for (std::int64_t parent = object.parent; parent != 0;) {
            const auto it = by_id_.find(parent);
            if (it == by_id_.end()) {
                return false;
            }
            if (!handled_.contains(parent)) {
                return true;
            }
            parent = it->second->parent;
        }
        return false;
    }

    const Model& model_;
    std::string_view file_stem_;
    bool text_;
    Export result_;
    const Json empty_ = Json::object();

    std::map<std::int64_t, const ModelObject*> by_id_;
    std::map<std::int64_t, std::vector<const ModelObject*>> children_;
    std::vector<const ModelObject*> roots_;
    std::vector<const ModelObject*> inputs_;
    std::set<std::int64_t> handled_;

    std::vector<MeshOut> meshes_;
    std::map<std::int64_t, std::size_t> mesh_index_;
    std::map<std::int64_t, SetOut> sets_;
    bool flat_ = true;
    std::string assembly_;

    std::vector<const ModelObject*> model_sections_;
    std::map<std::size_t, std::vector<const ModelObject*>> part_sections_;
    std::vector<const ModelObject*> assembly_sections_;

    std::vector<const ModelObject*> model_keywords_;
    std::vector<const ModelObject*> assembly_keywords_;
    std::vector<const ModelObject*> kept_steps_;
    std::map<std::string, std::vector<const ModelObject*>> part_keywords_;
    std::map<std::string, std::vector<const ModelObject*>> instance_keywords_;
    std::map<std::int64_t, std::vector<const ModelObject*>>
        owned_keywords_;  // Of steps, materials.
};

}  // namespace

Export export_model(const Model& model, std::string_view file_stem, bool text) {
    return Writer(model, file_stem, text).run();
}

std::vector<Problem> describe_losses(const Model& model, std::string_view file_stem) {
    return export_model(model, file_stem, false).losses;
}

}  // namespace fem::inp
