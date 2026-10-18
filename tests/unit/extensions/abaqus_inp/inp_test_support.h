#pragma once

// Helpers of the tests of fem.format.abaqus-inp (task 3.2): the fixtures of tests/data/inp and the
// items of an import found by type and name.

#include "inp/import.h"
#include "inp/problem.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fem::inp::testing {

inline std::filesystem::path data() {
    return std::filesystem::path(FEM_TEST_DATA_DIR) / "inp";
}

inline std::string utf8(std::u8string_view text) {
    return {text.begin(), text.end()};
}

/// Imports a fixture; fails the test if it cannot be imported.
inline Import import_fixture(const std::filesystem::path& file,
                             Encoding encoding = Encoding::automatic) {
    try {
        return import_file(data() / file, encoding);
    } catch (const InpError& error) {
        ADD_FAILURE() << error.problem().key << " at " << error.problem().file << ":"
                      << error.problem().line << " " << error.problem().args.dump();
        return {};
    }
}

/// The reason a fixture cannot be imported; an empty problem if it was imported.
inline Problem refusal(const std::filesystem::path& file, Encoding encoding = Encoding::automatic) {
    try {
        (void)import_file(data() / file, encoding);
    } catch (const InpError& error) {
        return error.problem();
    }
    ADD_FAILURE() << file.string() << " was imported";
    return {};
}

/// The item of a type and name, with `parent` if given; no_item if there is none.
inline ItemRef find(const Import& result, std::string_view type, std::string_view name,
                    ItemRef parent = no_item) {
    for (ItemRef i = 0; i < result.items.size(); ++i) {
        const Item& item = result.items[i];
        if (item.type == type && item.name == name &&
            (parent == no_item || item.parent == parent)) {
            return i;
        }
    }
    return no_item;
}

/// The items of a type, in creation order.
inline std::vector<ItemRef> of_type(const Import& result, std::string_view type,
                                    ItemRef parent = no_item) {
    std::vector<ItemRef> found;
    for (ItemRef i = 0; i < result.items.size(); ++i) {
        if (result.items[i].type == type &&
            (parent == no_item || result.items[i].parent == parent)) {
            found.push_back(i);
        }
    }
    return found;
}

/// The passthrough block of a keyword ("*CONTACT CONTROLS"), the first one if several.
inline ItemRef keyword_item(const Import& result, std::string_view keyword) {
    for (ItemRef i = 0; i < result.items.size(); ++i) {
        const Item& item = result.items[i];
        if (item.type == "abaqus_keyword" &&
            item.properties.at("keyword") == std::string(keyword)) {
            return i;
        }
    }
    return no_item;
}

/// The indices of a set item.
inline std::vector<std::int32_t> indices(const Import& result, ItemRef set) {
    if (set == no_item) {
        ADD_FAILURE() << "no such set";
        return {};
    }
    return std::get<SetData>(result.items[set].data).indices;
}

/// The keys of the problems, in order, as "key" or "key keyword" when it has one.
inline std::vector<std::string> problem_keys(const Import& result) {
    std::vector<std::string> keys;
    for (const Problem& problem : result.problems) {
        keys.push_back(problem.key);
    }
    return keys;
}

inline bool has_problem(const Import& result, std::string_view key, std::string_view arg,
                        std::string_view value) {
    return std::any_of(result.problems.begin(), result.problems.end(), [&](const Problem& p) {
        return p.key == key && p.args.contains(arg) && p.args.at(arg) == std::string(value);
    });
}

}  // namespace fem::inp::testing
