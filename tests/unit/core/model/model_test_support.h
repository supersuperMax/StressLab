#pragma once

#include "fem/core/model/document.h"
#include "fem/core/model/error.h"
#include "fem/core/model/mesh.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fem::model::testing {

inline constexpr const char* module_a = "test.module-a";
inline constexpr const char* module_b = "test.module-b";

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
