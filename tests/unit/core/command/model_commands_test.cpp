// Commands of the core ("model.*"): the Model IR changes available to scripts and the shell.

#include "fem/core/command/model_commands.h"

#include "fem/core/command/command_bus.h"
#include "fem/core/command/error.h"
#include "fem/core/model/error.h"
#include "fem/core/model/units.h"

#include "command_test_support.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace fem::command;
using fem::command::testing::JournalRecorder;
using fem::command::testing::test_owner;
using fem::command::testing::utf8;
using fem::model::Document;
using fem::model::ModelErrc;
using fem::model::ModelError;
using fem::model::ObjectId;
using fem::model::Properties;
using fem::model::UnitSystem;
namespace mc = fem::command::model_commands;

namespace {

constexpr const char* disabled_owner = "test.disabled";

class ModelCommandsTest : public ::testing::Test {
protected:
    void SetUp() override { doc.set_owner_enabled(test_owner, true); }

    ObjectId create(Value::Dict args) {
        args.try_emplace("type", "item");
        args.try_emplace("owner", test_owner);
        return ObjectId(
            static_cast<std::uint64_t>(bus.execute(mc::create, args).value.as_integer()));
    }

    void expect_model_error(std::string_view command, Value::Dict args, ModelErrc expected) {
        try {
            bus.execute(command, std::move(args));
            ADD_FAILURE() << command << " did not throw";
        } catch (const ModelError& error) {
            EXPECT_EQ(fem::model::to_string(error.code()), fem::model::to_string(expected))
                << error.what();
        }
    }

    Document doc;
    CommandBus bus{doc};
    JournalRecorder journal{bus};
};

}  // namespace

TEST_F(ModelCommandsTest, CreateSetsEveryField) {
    const ObjectId parent = create({{"name", "group"}});
    const ObjectId target = create({{"name", "target"}});
    const ObjectId id = create({{"type", "material"},
                                {"name", utf8(u8"Сталь")},
                                {"parent", parent},
                                {"dependencies", Value::List{target, target}},
                                {"schema_version", 3},
                                {"properties", Value::Dict{{"E", 2.1e5},
                                                           {"n", 2},
                                                           {"ok", true},
                                                           {"none", nullptr},
                                                           {"t", Value::List{1, "a"}},
                                                           {"d", Value::Dict{{"x", 0.5}}}}}});
    const fem::model::Object& object = doc.get(id);
    EXPECT_EQ(object.type(), "material");
    EXPECT_EQ(object.owner(), test_owner);
    EXPECT_EQ(object.name(), utf8(u8"Сталь"));
    EXPECT_EQ(object.parent(), parent);
    EXPECT_EQ(std::vector<ObjectId>(object.dependencies().begin(), object.dependencies().end()),
              std::vector<ObjectId>{target});
    EXPECT_EQ(object.schema_version(), 3U);
    const Properties expected = {{"E", 2.1e5},      {"n", 2},        {"ok", true},
                                 {"none", nullptr}, {"t", {1, "a"}}, {"d", {{"x", 0.5}}}};
    EXPECT_EQ(object.properties(), expected);
    EXPECT_TRUE(object.properties().at("E").is_number_float());
    EXPECT_TRUE(object.properties().at("n").is_number_integer());
}

TEST_F(ModelCommandsTest, CreateDefaultsToAnUnnamedRootObject) {
    const ObjectId id = create({});
    EXPECT_EQ(doc.get(id).name(), "");
    EXPECT_FALSE(doc.get(id).parent().is_valid());
    EXPECT_EQ(doc.get(id).schema_version(), 1U);
    EXPECT_EQ(doc.get(id).properties(), Properties::object());
    EXPECT_EQ(journal.entries(),
              std::vector<std::string>{
                  "stresslab.cmd(\"model.create\", type=\"item\", owner=\"test.module\")"});
}

TEST_F(ModelCommandsTest, CreateRefusesWhatTheModelRefuses) {
    expect_model_error(mc::create, {{"type", ""}, {"owner", test_owner}},
                       ModelErrc::invalid_argument);
    expect_model_error(mc::create, {{"type", "mesh"}, {"owner", "fem.core"}},
                       ModelErrc::invalid_data);
    expect_model_error(mc::create, {{"type", "item"}, {"owner", test_owner}, {"parent", 9}},
                       ModelErrc::unknown_object);
    expect_model_error(mc::create, {{"type", "item"}, {"owner", test_owner}, {"schema_version", 0}},
                       ModelErrc::invalid_argument);
    EXPECT_COMMAND_ERROR(
        bus.execute(mc::create,
                    {{"type", "item"}, {"owner", test_owner}, {"schema_version", 1LL << 40}}),
        CommandErrc::invalid_arguments);
    EXPECT_COMMAND_ERROR(
        bus.execute(mc::create,
                    {{"type", "item"}, {"owner", test_owner}, {"dependencies", Value::List{"x"}}}),
        CommandErrc::invalid_arguments);
    // Properties hold JSON: a path must be passed as text.
    EXPECT_COMMAND_ERROR(
        bus.execute(mc::create, {{"type", "item"},
                                 {"owner", test_owner},
                                 {"properties", Value::Dict{{"f", std::filesystem::path("a")}}}}),
        CommandErrc::invalid_arguments);
    EXPECT_EQ(doc.size(), 0U);
    EXPECT_TRUE(journal.entries().empty());
}

TEST_F(ModelCommandsTest, CreateMayAddObjectsOfDisabledModules) {
    // As an importer meets objects of a module that is not installed.
    const ObjectId id = create({{"owner", disabled_owner}});
    EXPECT_TRUE(doc.is_opaque(id));
    expect_model_error(mc::create, {{"parent", id}, {"type", "item"}, {"owner", test_owner}},
                       ModelErrc::opaque_object);
}

TEST_F(ModelCommandsTest, RenameAndSetProperties) {
    const ObjectId id = create({{"name", "a"}});
    bus.execute(mc::rename, {{"object", id}, {"name", utf8(u8"б")}});
    EXPECT_EQ(doc.get(id).name(), utf8(u8"б"));
    bus.execute(mc::set_properties, {{"object", id}, {"properties", Value::Dict{{"x", -0.0}}}});
    EXPECT_EQ(doc.get(id).properties(), (Properties{{"x", -0.0}}));
    bus.execute(mc::set_properties, {{"object", id}, {"properties", Value::Dict{}}});
    EXPECT_EQ(doc.get(id).properties(), Properties::object());

    create({{"name", "taken"}});
    expect_model_error(mc::rename, {{"object", id}, {"name", "taken"}}, ModelErrc::duplicate_name);
    expect_model_error(mc::rename, {{"object", 99}, {"name", "x"}}, ModelErrc::unknown_object);
    EXPECT_COMMAND_ERROR(bus.execute(mc::rename, {{"object", id}}), CommandErrc::invalid_arguments);
}

TEST_F(ModelCommandsTest, DependenciesReportWhetherTheyChanged) {
    const ObjectId a = create({{"name", "a"}});
    const ObjectId b = create({{"name", "b"}});
    EXPECT_EQ(bus.execute(mc::add_dependency, {{"object", a}, {"target", b}}).value, Value(true));
    EXPECT_EQ(bus.execute(mc::add_dependency, {{"object", a}, {"target", b}}).value, Value(false));
    expect_model_error(mc::add_dependency, {{"object", b}, {"target", a}},
                       ModelErrc::dependency_cycle);
    EXPECT_EQ(bus.execute(mc::remove_dependency, {{"object", a}, {"target", b}}).value,
              Value(true));
    EXPECT_EQ(bus.execute(mc::remove_dependency, {{"object", a}, {"target", b}}).value,
              Value(false));
    EXPECT_FALSE(doc.get(a).depends_on(b));
}

TEST_F(ModelCommandsTest, RemoveReturnsTheRemovedIds) {
    const ObjectId group = create({{"name", "g"}});
    const ObjectId child = create({{"name", "c"}, {"parent", group}});
    EXPECT_EQ(bus.execute(mc::remove, {{"object", group}}).value, Value(Value::List{group, child}));
    EXPECT_EQ(doc.size(), 0U);
    expect_model_error(mc::remove, {{"object", group}}, ModelErrc::unknown_object);
}

TEST_F(ModelCommandsTest, BlockedRemovalThrowsAndChangesNothing) {
    doc.set_owner_enabled(disabled_owner, true);
    const ObjectId target = create({{"name", "target"}});
    const ObjectId opaque =
        create({{"owner", disabled_owner}, {"dependencies", Value::List{target}}});
    doc.set_owner_enabled(disabled_owner, false);

    try {
        bus.execute(mc::remove, {{"object", target}});
        ADD_FAILURE() << "removed what an opaque object needs";
    } catch (const ModelError& error) {
        EXPECT_EQ(error.code(), ModelErrc::removal_blocked);
        EXPECT_EQ(error.object(), target);
        EXPECT_NE(std::string(error.what()).find(fem::model::to_string(opaque)), std::string::npos)
            << error.what();
    }
    EXPECT_TRUE(doc.contains(target));
    EXPECT_EQ(journal.entries().size(), 2U);
}

TEST_F(ModelCommandsTest, SetUnitsTakesUnitSymbols) {
    bus.execute(mc::set_units,
                {{"length", "mm"}, {"mass", "t"}, {"time", "s"}, {"temperature", "degC"}});
    EXPECT_EQ(doc.units(), UnitSystem::mm_t_s());
    EXPECT_COMMAND_ERROR(
        bus.execute(mc::set_units,
                    {{"length", "mile"}, {"mass", "t"}, {"time", "s"}, {"temperature", "K"}}),
        CommandErrc::invalid_arguments);
    EXPECT_COMMAND_ERROR(bus.execute(mc::set_units, {{"length", "mm"}}),
                         CommandErrc::invalid_arguments);
    EXPECT_EQ(doc.units(), UnitSystem::mm_t_s());
}
