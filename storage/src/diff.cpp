#include "fem/storage/diff.h"

#include "fem/core/form/status.h"
#include "fem/core/model/object.h"
#include "fem/core/model/quantity.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>

namespace fem::storage {

namespace {

using Json = nlohmann::json;
using model::ObjectId;

/// The same value, telling 1 from 1.0 and -0.0 from 0.0 as the files do.
bool same(const Json& a, const Json& b) {
    return a.dump() == b.dump();
}

std::string escape(std::string_view key) {
    std::string result;
    for (const char c : key) {
        if (c == '~') {
            result += "~0";
        } else if (c == '/') {
            result += "~1";
        } else {
            result += c;
        }
    }
    return result;
}

void diff_properties(const Json& before, const Json& after, const std::string& prefix,
                     std::vector<PropertyChange>& out) {
    std::set<std::string> keys;
    for (auto it = before.begin(); it != before.end(); ++it) {
        keys.insert(it.key());
    }
    for (auto it = after.begin(); it != after.end(); ++it) {
        keys.insert(it.key());
    }
    for (const std::string& key : keys) {
        const std::string pointer = prefix.empty() ? escape(key) : prefix + '/' + escape(key);
        const auto old = before.find(key);
        const auto now = after.find(key);
        if (old != before.end() && now != after.end()) {
            if (old->is_object() && now->is_object()) {
                diff_properties(*old, *now, pointer, out);
            } else if (!same(*old, *now)) {
                out.push_back(PropertyChange{.pointer = pointer,
                                             .before = std::optional<Json>(*old),
                                             .after = std::optional<Json>(*now)});
            }
        } else if (old != before.end()) {
            out.push_back(PropertyChange{.pointer = pointer, .before = std::optional<Json>(*old)});
        } else {
            out.push_back(PropertyChange{.pointer = pointer, .after = std::optional<Json>(*now)});
        }
    }
}

std::vector<ObjectId> missing_from(const std::vector<ObjectId>& from,
                                   const std::vector<ObjectId>& in) {
    std::vector<ObjectId> a = from;
    std::vector<ObjectId> b = in;
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    std::vector<ObjectId> result;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(result));
    return result;
}

ObjectChange compare(const ObjectRecord& before, const ObjectRecord& after) {
    const model::NewObject& old = before.object.init;
    const model::NewObject& now = after.object.init;
    ObjectChange change;
    change.id = after.object.id;
    change.type = old.owner != now.owner || old.type != now.type;
    change.name = old.name != now.name;
    change.parent = old.parent != now.parent;
    change.schema_version = old.schema_version != now.schema_version;
    change.data = before.blob != after.blob || !same(before.data, after.data);
    diff_properties(old.properties, now.properties, {}, change.properties);
    change.dependencies_added = missing_from(now.dependencies, old.dependencies);
    change.dependencies_removed = missing_from(old.dependencies, now.dependencies);
    return change;
}

bool differs(const ObjectChange& change) {
    return change.type || change.name || change.parent || change.schema_version || change.data ||
           !change.properties.empty() || !change.dependencies_added.empty() ||
           !change.dependencies_removed.empty();
}

ObjectLabel label_of(const ObjectRecord& record) {
    const model::NewObject& init = record.object.init;
    return {init.owner, init.type, init.name};
}

Json id_or_null(ObjectId id) {
    return id.is_valid() ? Json(id.value()) : Json();
}

Json ids(const std::vector<ObjectId>& list) {
    Json result = Json::array();
    for (const ObjectId id : list) {
        result.push_back(id.value());
    }
    return result;
}

/// The members of an object file but its id, owner and type.
Json record_json(const ObjectRecord& record) {
    const model::NewObject& init = record.object.init;
    Json result = {
        {"name", init.name},
        {"parent", id_or_null(init.parent)},
        {"schemaVersion", init.schema_version},
        {"dependencies", ids(init.dependencies)},
        {"properties", init.properties},
    };
    if (!record.data.is_null()) {
        result["data"] = record.data;
    }
    return result;
}

Json before_after(Json before, Json after) {
    return {{"before", std::move(before)}, {"after", std::move(after)}};
}

}  // namespace

std::string_view to_string(ChangeKind kind) noexcept {
    switch (kind) {
        case ChangeKind::added:
            return "added";
        case ChangeKind::removed:
            return "removed";
        case ChangeKind::changed:
            break;
    }
    return "changed";
}

const ObjectChange* ModelDiff::find(ObjectId id) const noexcept {
    const auto it = std::lower_bound(
        objects.begin(), objects.end(), id,
        [](const ObjectChange& change, ObjectId target) { return change.id < target; });
    return it != objects.end() && it->id == id ? &*it : nullptr;
}

ModelDiff diff_files(const ProjectFiles& before, const ProjectFiles& after) {
    ProjectRecords old = read_project_records(before);
    ProjectRecords now = read_project_records(after);
    ModelDiff diff;
    diff.units_before = old.units;
    diff.units_after = now.units;

    std::set<ObjectId> ids;
    for (const auto& [id, record] : old.objects) {
        ids.insert(id);
    }
    for (const auto& [id, record] : now.objects) {
        ids.insert(id);
    }
    std::set<ObjectId> referenced;
    for (const ObjectId id : ids) {
        const auto was = old.objects.find(id);
        const auto is = now.objects.find(id);
        ObjectChange change;
        if (was == old.objects.end()) {
            change.kind = ChangeKind::added;
            change.id = id;
        } else if (is == now.objects.end()) {
            change.kind = ChangeKind::removed;
            change.id = id;
        } else {
            change = compare(was->second, is->second);
            if (!differs(change)) {
                continue;
            }
        }
        if (was != old.objects.end()) {
            change.before = was->second;
        }
        if (is != now.objects.end()) {
            change.after = is->second;
        }
        if (change.parent) {
            referenced.insert(change.before->object.init.parent);
            referenced.insert(change.after->object.init.parent);
        }
        referenced.insert(change.dependencies_added.begin(), change.dependencies_added.end());
        referenced.insert(change.dependencies_removed.begin(), change.dependencies_removed.end());
        diff.objects.push_back(std::move(change));
    }
    for (const ObjectId id : referenced) {
        if (!id.is_valid()) {
            continue;
        }
        if (const auto is = now.objects.find(id); is != now.objects.end()) {
            diff.referenced.emplace(id, label_of(is->second));
        } else if (const auto was = old.objects.find(id); was != old.objects.end()) {
            diff.referenced.emplace(id, label_of(was->second));
        }
    }
    return diff;
}

Json to_json(const ModelDiff& diff) {
    Json result = Json::object();
    if (diff.units_changed()) {
        result["units"] =
            before_after(model::to_string(diff.units_before), model::to_string(diff.units_after));
    }
    Json objects = Json::array();
    for (const ObjectChange& change : diff.objects) {
        const ObjectRecord& current = change.current();
        Json value = {
            {"id", change.id.value()},
            {"kind", to_string(change.kind)},
            {"owner", current.object.init.owner},
            {"type", current.object.init.type},
        };
        if (change.kind != ChangeKind::changed) {
            value.update(record_json(current));
            objects.push_back(std::move(value));
            continue;
        }
        const model::NewObject& old = change.before->object.init;
        const model::NewObject& now = change.after->object.init;
        value["name"] = now.name;
        if (change.type) {
            value["typeChanged"] =
                before_after(old.owner + '/' + old.type, now.owner + '/' + now.type);
        }
        if (change.name) {
            value["renamed"] = before_after(old.name, now.name);
        }
        if (change.parent) {
            value["parent"] = before_after(id_or_null(old.parent), id_or_null(now.parent));
        }
        if (change.schema_version) {
            value["schemaVersion"] = before_after(old.schema_version, now.schema_version);
        }
        if (!change.properties.empty()) {
            Json properties = Json::array();
            for (const PropertyChange& property : change.properties) {
                Json item = {{"pointer", property.pointer}};
                if (property.before) {
                    item["before"] = *property.before;
                }
                if (property.after) {
                    item["after"] = *property.after;
                }
                properties.push_back(std::move(item));
            }
            value["properties"] = std::move(properties);
        }
        if (!change.dependencies_added.empty() || !change.dependencies_removed.empty()) {
            value["dependencies"] = {{"added", ids(change.dependencies_added)},
                                     {"removed", ids(change.dependencies_removed)}};
        }
        if (change.data) {
            value["data"] = before_after(change.before->data, change.after->data);
        }
        objects.push_back(std::move(value));
    }
    result["objects"] = std::move(objects);
    return result;
}

// --- In words ------------------------------------------------------------------------------------

namespace {

class Describer {
public:
    Describer(const ModelDiff& diff, const DiffContext& context)
        : diff_(diff), context_(context), l10n_(context.localization) {
        if (context.registry != nullptr) {
            forms_ = form::FormIndex(*context.registry);
        }
    }

    std::vector<std::string> lines() const {
        std::vector<std::string> out;
        if (diff_.units_changed()) {
            out.push_back(core("diff.units", {{"before", model::to_string(diff_.units_before)},
                                              {"after", model::to_string(diff_.units_after)}}));
        }
        for (const ObjectChange& change : diff_.objects) {
            out.push_back(line(change));
        }
        return out;
    }

private:
    [[nodiscard]] std::string core(std::string_view key, const l10n::Args& args = {}) const {
        return l10n_.text(model::core_owner, key, args);
    }

    /// The module of the owner is disabled or not installed: its objects are raw JSON.
    [[nodiscard]] bool is_raw(const std::string& owner) const {
        return context_.registry != nullptr && owner != model::core_owner &&
               !context_.registry->contains(owner);
    }

    [[nodiscard]] std::string type_title(const std::string& owner, const std::string& type) const {
        if (owner == model::core_owner) {
            if (const std::optional<std::string> title =
                    l10n_.find(model::core_owner, "model.type." + type)) {
                return *title;
            }
            return type;
        }
        if (context_.registry != nullptr && !is_raw(owner)) {
            for (const extension::TreeNodeTypeContribution* node :
                 context_.registry->tree_node_types()) {
                if (node->owner == owner && node->type == type && !node->title.empty()) {
                    return l10n_.manifest_text(owner, node->title);
                }
            }
        }
        return type;
    }

    [[nodiscard]] std::string object_name(const ObjectLabel& label, ObjectId id) const {
        const std::string type = type_title(label.owner, label.type);
        if (label.name.empty()) {
            return core("diff.object.unnamed",
                        {{"type", type}, {"id", static_cast<std::int64_t>(id.value())}});
        }
        return core("diff.object", {{"type", type}, {"name", label.name}});
    }

    [[nodiscard]] std::string referenced_name(ObjectId id) const {
        if (!id.is_valid()) {
            return core("diff.absent");
        }
        if (const auto it = diff_.referenced.find(id); it != diff_.referenced.end()) {
            return object_name(it->second, id);
        }
        return "#" + std::to_string(id.value());
    }

    [[nodiscard]] std::string line(const ObjectChange& change) const {
        const ObjectRecord& current = change.current();
        const std::string object = object_name(label_of(current), change.id);
        const std::string& owner = current.object.init.owner;
        if (is_raw(owner) || (change.type && is_raw(change.before->object.init.owner))) {
            return raw_line(change, object, owner);
        }
        switch (change.kind) {
            case ChangeKind::added:
                return core("diff.added", {{"object", object}});
            case ChangeKind::removed:
                return core("diff.removed", {{"object", object}});
            case ChangeKind::changed:
                break;
        }
        return core("diff.changed", {{"object", object}, {"changes", changes(change)}});
    }

    [[nodiscard]] std::string raw_line(const ObjectChange& change, const std::string& object,
                                       const std::string& owner) const {
        if (change.kind == ChangeKind::added) {
            return core("diff.raw.added", {{"object", object},
                                           {"module", owner},
                                           {"after", record_json(*change.after).dump()}});
        }
        if (change.kind == ChangeKind::removed) {
            return core("diff.raw.removed", {{"object", object},
                                             {"module", owner},
                                             {"before", record_json(*change.before).dump()}});
        }
        // Only the members that differ.
        const Json old = record_json(*change.before);
        const Json now = record_json(*change.after);
        Json before = Json::object();
        Json after = Json::object();
        for (auto it = now.begin(); it != now.end(); ++it) {
            if (const auto was = old.find(it.key()); was == old.end() || !same(*was, *it)) {
                after[it.key()] = *it;
                if (was != old.end()) {
                    before[it.key()] = *was;
                }
            }
        }
        for (auto it = old.begin(); it != old.end(); ++it) {
            if (!now.contains(it.key())) {
                before[it.key()] = *it;
            }
        }
        if (change.type) {
            before["type"] =
                change.before->object.init.owner + '/' + change.before->object.init.type;
            after["type"] = change.after->object.init.owner + '/' + change.after->object.init.type;
        }
        if (change.data && before.find("data") == before.end()) {
            // The same description of another block.
            before["data"] = change.before->blob;
            after["data"] = change.after->blob;
        }
        return core("diff.raw", {{"object", object},
                                 {"module", owner},
                                 {"before", before.dump()},
                                 {"after", after.dump()}});
    }

    [[nodiscard]] std::string changes(const ObjectChange& change) const {
        const model::NewObject& old = change.before->object.init;
        const model::NewObject& now = change.after->object.init;
        std::vector<std::string> parts;
        if (change.type) {
            parts.push_back(core("diff.type", {{"before", old.owner + '/' + old.type},
                                               {"after", now.owner + '/' + now.type}}));
        }
        if (change.name) {
            parts.push_back(core("diff.renamed", {{"before", shown_name(old.name)},
                                                  {"after", shown_name(now.name)}}));
        }
        if (change.parent) {
            parts.push_back(core("diff.parent", {{"before", referenced_name(old.parent)},
                                                 {"after", referenced_name(now.parent)}}));
        }
        if (change.schema_version) {
            parts.push_back(core("diff.schemaVersion",
                                 {{"before", static_cast<std::int64_t>(old.schema_version)},
                                  {"after", static_cast<std::int64_t>(now.schema_version)}}));
        }
        const form::PropertyForm* form = forms_.find(now.owner, now.type);
        for (const PropertyChange& property : change.properties) {
            parts.push_back(field_change(property, form, now.owner));
        }
        for (const ObjectId id : change.dependencies_added) {
            parts.push_back(core("diff.dependencyAdded", {{"object", referenced_name(id)}}));
        }
        for (const ObjectId id : change.dependencies_removed) {
            parts.push_back(core("diff.dependencyRemoved", {{"object", referenced_name(id)}}));
        }
        if (change.data) {
            parts.push_back(data_change(change.before->data, change.after->data));
        }
        std::string result;
        for (const std::string& part : parts) {
            if (!result.empty()) {
                result += ", ";
            }
            result += part;
        }
        return result;
    }

    [[nodiscard]] std::string shown_name(const std::string& name) const {
        return name.empty() ? core("diff.absent") : name;
    }

    [[nodiscard]] std::string field_change(const PropertyChange& property,
                                           const form::PropertyForm* form,
                                           const std::string& owner) const {
        const form::Field* field =
            form != nullptr ? form::find_field(form->form, property.pointer) : nullptr;
        std::string title = property.pointer;
        if (field != nullptr && !field->title.empty()) {
            title = l10n_.manifest_text(owner, field->title);
        }
        const std::string before = value(property.before, field, owner);
        const std::string after = value(property.after, field, owner);
        const bool numbers = (!property.before || property.before->is_number()) &&
                             (!property.after || property.after->is_number());
        if (field != nullptr && field->quantity != nullptr && numbers) {
            const std::string unit_before = unit(*field->quantity, diff_.units_before);
            const std::string unit_after = unit(*field->quantity, diff_.units_after);
            if (unit_before == unit_after) {
                return core(
                    "diff.field.unit",
                    {{"field", title}, {"before", before}, {"after", after}, {"unit", unit_after}});
            }
            return core("diff.field.units", {{"field", title},
                                             {"before", before},
                                             {"unitBefore", unit_before},
                                             {"after", after},
                                             {"unitAfter", unit_after}});
        }
        return core("diff.field", {{"field", title}, {"before", before}, {"after", after}});
    }

    [[nodiscard]] std::string unit(const model::Quantity& quantity,
                                   const model::UnitSystem& units) const {
        if (const model::DisplayUnit* display = model::model_unit(quantity, units)) {
            return core("unit." + std::string(display->id));
        }
        return model::compose_symbol(quantity.dimension, units);
    }

    [[nodiscard]] std::string value(const std::optional<Json>& value, const form::Field* field,
                                    const std::string& owner) const {
        if (!value) {
            return core("diff.absent");
        }
        if (field != nullptr && field->kind == form::FieldKind::choice) {
            for (const form::Choice& choice : field->choices) {
                if (same(choice.value, *value) && !choice.title.empty()) {
                    return l10n_.manifest_text(owner, choice.title);
                }
            }
        }
        if (value->is_number_float()) {
            return l10n::to_text(value->get<double>());
        }
        if (value->is_boolean()) {
            return core(value->get<bool>() ? "diff.true" : "diff.false");
        }
        return value->dump();
    }

    [[nodiscard]] std::string data_change(const Json& before, const Json& after) const {
        if (before.is_null()) {
            return core("diff.data.added");
        }
        if (after.is_null()) {
            return core("diff.data.removed");
        }
        const std::string kind = after.value("kind", "");
        if (kind != before.value("kind", "")) {
            return core("diff.data.changed");
        }
        const auto count = [](const Json& value, const char* key) {
            return value.value(key, std::int64_t{0});
        };
        if (kind == "mesh") {
            if (count(before, "elements") != count(after, "elements")) {
                return core("diff.mesh.elements", {{"before", count(before, "elements")},
                                                   {"count", count(after, "elements")}});
            }
            if (count(before, "nodes") != count(after, "nodes")) {
                return core("diff.mesh.nodes",
                            {{"before", count(before, "nodes")}, {"count", count(after, "nodes")}});
            }
            return core("diff.mesh.same");
        }
        if (count(before, "size") != count(after, "size")) {
            const std::string key = kind == "node_set"      ? "diff.nodeSet.size"
                                    : kind == "element_set" ? "diff.elementSet.size"
                                    : kind == "surface"     ? "diff.surface.size"
                                                            : "diff.data.size";
            return core(key, {{"before", count(before, "size")}, {"count", count(after, "size")}});
        }
        return core("diff.data.changed");
    }

    const ModelDiff& diff_;
    const DiffContext& context_;
    const l10n::Localization& l10n_;
    form::FormIndex forms_;
};

}  // namespace

std::vector<std::string> describe(const ModelDiff& diff, const DiffContext& context) {
    return Describer(diff, context).lines();
}

}  // namespace fem::storage
