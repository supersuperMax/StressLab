// What is wrong with an object of the model: the ⚠/✕ marks of the model tree.

#include "fem/core/form/status.h"

#include "fem/core/command/command_bus.h"
#include "fem/core/command/model_commands.h"
#include "fem/core/extension/contributions.h"
#include "fem/core/extension/registry.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

using fem::command::Value;
using fem::form::FormIndex;
using fem::form::Problem;
using fem::form::Severity;
using fem::model::ObjectId;
namespace mc = fem::command::model_commands;

namespace {

constexpr const char* owner = "test.structural";

class ObjectStatusTest : public ::testing::Test {
protected:
    void SetUp() override {
        doc.set_owner_enabled(owner, true);
        auto contributions = std::make_shared<fem::extension::Contributions>();
        contributions->owner = owner;
        fem::extension::PropertySchemaContribution material;
        material.owner = owner;
        material.type = "material";
        material.version = 2;
        material.schema = {{"type", "object"},
                           {"required", {"E"}},
                           {"properties",
                            {{"E", {{"type", "number"}, {"exclusiveMinimum", 0}}},
                             {"nu", {{"type", "number"}, {"maximum", 0.5}}}}}};
        contributions->property_schemas.push_back(material);
        registry.add(contributions);
        forms = FormIndex(registry);
    }

    ObjectId create(std::string type, Value::Dict properties, std::int64_t version = 2,
                    Value::List dependencies = {}) {
        return ObjectId(static_cast<std::uint64_t>(
            bus.execute(mc::create, {{"type", std::move(type)},
                                     {"owner", owner},
                                     {"schema_version", version},
                                     {"dependencies", std::move(dependencies)},
                                     {"properties", std::move(properties)}})
                .value.as_integer()));
    }

    std::vector<std::string> problems(ObjectId id) const {
        std::vector<std::string> out;
        for (const Problem& problem : fem::form::object_problems(doc, forms, id)) {
            out.push_back(std::string(fem::form::to_string(problem.severity)) + " " +
                          std::string(fem::form::to_string(problem.code)) + " " +
                          problem.detail.dump());
        }
        return out;
    }

    fem::model::Document doc;
    fem::command::CommandBus bus{doc};
    fem::extension::ContributionRegistry registry;
    FormIndex forms;
};

using Strings = std::vector<std::string>;

}  // namespace

TEST_F(ObjectStatusTest, IndexFindsTheFormOfAnOwnerAndType) {
    ASSERT_NE(forms.find(owner, "material"), nullptr);
    EXPECT_EQ(forms.find(owner, "material")->version, 2U);
    EXPECT_EQ(forms.find(owner, "material")->form.fields.size(), 2U);
    EXPECT_EQ(forms.find(owner, "section"), nullptr);
    EXPECT_EQ(forms.find("other.module", "material"), nullptr);
    EXPECT_EQ(FormIndex().find(owner, "material"), nullptr);
}

TEST_F(ObjectStatusTest, PropertiesAreCheckedAgainstTheForm) {
    EXPECT_TRUE(problems(create("material", {{"E", 2.1e11}})).empty());
    EXPECT_EQ(problems(create("material", {})), (Strings{"error required {}"}));
    EXPECT_EQ(problems(create("material", {{"E", 1.0}, {"nu", 0.7}})),
              (Strings{"error aboveMaximum {\"exclusive\":false,\"limit\":0.5}"}));
    // Without a form only the model itself is checked.
    EXPECT_TRUE(problems(create("section", {{"anything", true}})).empty());
}

TEST_F(ObjectStatusTest, AnotherSchemaVersionIsAWarning) {
    const ObjectId old = create("material", {{"E", 1.0}}, 1);
    EXPECT_EQ(problems(old), (Strings{"warning schemaVersion {\"object\":1,\"schema\":2}"}));
    EXPECT_EQ(fem::form::worst(fem::form::object_problems(doc, forms, old)), Severity::warning);
}

TEST_F(ObjectStatusTest, ADependencyOnARemovedObjectIsAnError) {
    const ObjectId target = create("section", {});
    const ObjectId material = create("material", {{"E", 1.0}}, 2, {target});
    EXPECT_TRUE(problems(material).empty());
    bus.execute(mc::remove, {{"object", target}});
    EXPECT_EQ(problems(material), (Strings{"error missingDependency {\"object\":\"" +
                                           fem::model::to_string(target) + "\"}"}));
    ASSERT_TRUE(bus.undo());
    EXPECT_TRUE(problems(material).empty());
}
