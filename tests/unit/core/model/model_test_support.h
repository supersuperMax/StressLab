#pragma once

#include "fem/core/command/command_bus.h"
#include "fem/core/model/contents.h"
#include "fem/core/model/document.h"
#include "fem/core/model/document_editor.h"
#include "fem/core/model/error.h"
#include "fem/core/model/mesh.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fem::model::testing {

inline constexpr const char* module_a = "test.module-a";
inline constexpr const char* module_b = "test.module-b";

/// Changes a document with the calls of `DocumentEditor`, through the Command Bus like any code
/// outside the core: every call runs as a command of a test module.
class BusEditor {
public:
    explicit BusEditor(Document& doc) : bus_(doc) {
        bus_.register_command({.id = "test.edit", .owner = "test.harness"},
                              [this](command::CommandContext& context) {
                                  edit_(context.editor());
                                  return command::CommandResult{};
                              });
    }

    ObjectId create(NewObject init) {
        ObjectId id;
        run([&](DocumentEditor& editor) { id = editor.create(std::move(init)); });
        return id;
    }
    void restore(StoredObject stored) {
        run([&](DocumentEditor& editor) { editor.restore(std::move(stored)); });
    }
    void rename(ObjectId id, std::string name) {
        run([&](DocumentEditor& editor) { editor.rename(id, std::move(name)); });
    }
    void set_properties(ObjectId id, Properties properties) {
        run([&](DocumentEditor& editor) { editor.set_properties(id, std::move(properties)); });
    }
    void set_data(ObjectId id, std::shared_ptr<const ObjectData> data) {
        run([&](DocumentEditor& editor) { editor.set_data(id, std::move(data)); });
    }
    bool add_dependency(ObjectId dependent, ObjectId target) {
        bool added = false;
        run([&](DocumentEditor& editor) { added = editor.add_dependency(dependent, target); });
        return added;
    }
    bool remove_dependency(ObjectId dependent, ObjectId target) {
        bool removed = false;
        run([&](DocumentEditor& editor) { removed = editor.remove_dependency(dependent, target); });
        return removed;
    }
    RemoveResult remove(ObjectId id) {
        RemoveResult result;
        run([&](DocumentEditor& editor) { result = editor.remove(id); });
        return result;
    }
    void set_units(const UnitSystem& units) {
        run([&](DocumentEditor& editor) { editor.set_units(units); });
    }

    [[nodiscard]] command::CommandBus& bus() { return bus_; }

private:
    void run(const std::function<void(DocumentEditor&)>& edit) {
        edit_ = edit;
        bus_.execute("test.edit");
    }

    command::CommandBus bus_;
    std::function<void(DocumentEditor&)> edit_;
};

inline NewObject module_object(std::string owner, std::string type, std::string name = {},
                               ObjectId parent = {}, std::vector<ObjectId> dependencies = {}) {
    NewObject init;
    init.owner = std::move(owner);
    init.type = std::move(type);
    init.name = std::move(name);
    init.parent = parent;
    init.dependencies = std::move(dependencies);
    return init;
}

template <class... Blocks>
std::vector<ElementBlock> make_blocks(Blocks&&... blocks) {
    std::vector<ElementBlock> result;
    (result.push_back(std::forward<Blocks>(blocks)), ...);
    return result;
}

/// Five nodes, two tet4 elements (global indices 0, 1) and one tri3 element (global index 2).
inline std::shared_ptr<const Mesh> small_mesh() {
    NodeArrays nodes;
    nodes.ids = {1, 2, 3, 4, 5};
    nodes.x = {0, 1, 0, 0, 0};
    nodes.y = {0, 0, 1, 0, 0};
    nodes.z = {0, 0, 0, 1, -1};
    return std::make_shared<const Mesh>(
        std::move(nodes),
        make_blocks(ElementBlock(ElementType::tet4, {10, 11}, {0, 1, 2, 3, 0, 2, 1, 4}),
                    ElementBlock(ElementType::tri3, {20}, {0, 1, 2})));
}

/// Two nodes and no elements.
inline std::shared_ptr<const Mesh> tiny_mesh() {
    NodeArrays nodes;
    nodes.ids = {1, 2};
    nodes.x = {0, 1};
    nodes.y = {0, 0};
    nodes.z = {0, 0};
    return std::make_shared<const Mesh>(std::move(nodes), std::vector<ElementBlock>{});
}

/// The object as it is now, to restore it after it is removed.
inline StoredObject stored_copy(const Document& doc, ObjectId id) {
    const Object& object = doc.get(id);
    NewObject init;
    init.type = object.type();
    init.owner = object.owner();
    init.schema_version = object.schema_version();
    init.name = object.name();
    init.properties = object.properties();
    init.parent = object.parent();
    init.dependencies.assign(object.dependencies().begin(), object.dependencies().end());
    init.data = object.data();
    return {id, std::move(init)};
}

}  // namespace fem::model::testing

/// Checks that `statement` throws ModelError with the given code.
#define EXPECT_MODEL_ERROR(statement, expected_code)                                           \
    do {                                                                                       \
        try {                                                                                  \
            statement;                                                                         \
            ADD_FAILURE() << "Expected ModelError " << ::fem::model::to_string(expected_code); \
        } catch (const ::fem::model::ModelError& error) {                                      \
            EXPECT_EQ(::fem::model::to_string(error.code()),                                   \
                      ::fem::model::to_string(expected_code))                                  \
                << error.what();                                                               \
        }                                                                                      \
    } while (false)
