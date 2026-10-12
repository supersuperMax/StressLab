#pragma once

#include "fem/core/command/command_bus.h"
#include "fem/core/command/error.h"
#include "fem/core/command/journal.h"
#include "fem/core/model/document.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fem::command::testing {

inline constexpr const char* test_owner = "test.module";

// Text as UTF-8 bytes. GoogleTest from vcpkg is built as C++17 and cannot print char8_t strings.
inline std::string utf8(std::u8string_view text) {
    return {text.begin(), text.end()};
}

/// Everything observable about a document, including its lookup indices, as one JSON text.
/// Two documents with the same dump are the same model.
inline std::string dump(const model::Document& doc) {
    using model::Object;
    using model::ObjectId;
    const auto ids = [](auto&& range) {
        nlohmann::json result = nlohmann::json::array();
        for (const ObjectId id : range) {
            result.push_back(id.value());
        }
        return result;
    };

    nlohmann::json objects = nlohmann::json::array();
    nlohmann::json dependents = nlohmann::json::object();
    for (const Object& object : doc.objects()) {
        objects.push_back({
            {"id", object.id().value()},
            {"type", object.type()},
            {"owner", object.owner()},
            {"schema_version", object.schema_version()},
            {"name", object.name()},
            {"properties", object.properties()},
            {"parent", object.parent().value()},
            {"children", ids(object.children())},
            {"dependencies", ids(object.dependencies())},
            {"missing", ids(doc.missing_dependencies(object.id()))},
            {"has_data", object.data() != nullptr},
            {"found_by_name",
             object.name().empty()
                 ? 0U
                 : doc.find_child(object.parent(), object.type(), object.name()).value()},
        });
        dependents[std::to_string(object.id().value())] = ids(doc.dependents(object.id()));
        for (const ObjectId target : object.dependencies()) {
            dependents[std::to_string(target.value())] = ids(doc.dependents(target));
        }
    }
    const nlohmann::json result = {
        {"units", model::to_string(doc.units())},
        {"roots", ids(doc.roots())},
        {"objects", objects},
        {"dependents", dependents},
    };
    return result.dump(2);
}

/// Collects journal entries of a bus as journal text (without the header).
class JournalRecorder {
public:
    explicit JournalRecorder(CommandBus& bus)
        : subscription_(bus.on_journal([this](std::string_view entry) {
              entries_.emplace_back(entry);
              text_.append(entry);
              text_.push_back('\n');
          })) {}

    [[nodiscard]] const std::string& text() const { return text_; }
    [[nodiscard]] const std::vector<std::string>& entries() const { return entries_; }
    [[nodiscard]] std::string file() const { return journal_header() + text_; }

private:
    std::vector<std::string> entries_;
    std::string text_;
    Subscription subscription_;
};

}  // namespace fem::command::testing

/// Checks that `statement` throws CommandError with the given code.
#define EXPECT_COMMAND_ERROR(statement, expected_code)                                             \
    do {                                                                                           \
        try {                                                                                      \
            statement;                                                                             \
            ADD_FAILURE() << "Expected CommandError " << ::fem::command::to_string(expected_code); \
        } catch (const ::fem::command::CommandError& error) {                                      \
            EXPECT_EQ(::fem::command::to_string(error.code()),                                     \
                      ::fem::command::to_string(expected_code))                                    \
                << error.what();                                                                   \
        }                                                                                          \
    } while (false)
