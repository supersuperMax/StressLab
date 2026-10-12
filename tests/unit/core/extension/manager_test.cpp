#include "fem/core/extension/manager.h"

#include "fem/core/command/command_bus.h"
#include "fem/core/command/value.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/context/value.h"
#include "fem/core/extension/error.h"
#include "fem/core/model/document.h"
#include "fem/core/model/error.h"
#include "fem/core/model/object_id.h"
#include "fem/core/version.h"

#include "extension_test_support.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

using fem::command::Value;
using fem::context::ContextValue;
using fem::extension::ExtensionErrc;
using fem::extension::ExtensionError;
using fem::extension::ExtensionInfo;
using fem::extension::ExtensionManager;
using fem::extension::ExtensionSource;
using fem::extension::ExtensionState;
using fem::extension::IndexedCommand;
using fem::extension::ScanReport;
using fem::extension::testing::engine;
using fem::extension::testing::EventLog;
using fem::extension::testing::fixtures;
using fem::extension::testing::Workspace;
using fem::model::ModelErrc;
using fem::model::ModelError;
using fem::model::ObjectId;
using Ids = std::vector<std::string>;

/// The modules of tests/extensions/installed: base <- solver <- post, l10n without code, future
/// for another platform, and a broken manifest.
class ExtensionManagerTest : public ::testing::Test {
protected:
    ExtensionManagerTest() { ws.manager.scan(fixtures() / "installed", ExtensionSource::builtin); }

    [[nodiscard]] ExtensionState state(std::string_view id) const {
        return ws.manager.get(id).state;
    }

    Workspace ws;
};

/// A module written in a test: `id`, with `rest` after the header.
std::string module(std::string_view id, std::string_view rest = {},
                   std::string_view version = "1.0.0") {
    return "[extension]\nid = \"" + std::string(id) + "\"\nname = \"" + std::string(id) +
           "\"\nversion = \"" + std::string(version) +
           "\"\nengine = \"^1.0\"\nkind = \"python\"\nentry = \"m:activate\"\n" + std::string(rest);
}

TEST(ExtensionManagerScan, InstallsModulesAndReportsBadManifests) {
    Workspace ws;
    const ScanReport report = ws.manager.scan(fixtures() / "installed", ExtensionSource::builtin);
    EXPECT_EQ(report.installed,
              (Ids{"test.base", "test.future", "test.l10n", "test.post", "test.solver"}));
    ASSERT_EQ(report.problems.size(), 1U);
    EXPECT_EQ(report.problems[0].folder.filename(), "broken");
    ASSERT_EQ(report.problems[0].diagnostics.size(), 1U);
    EXPECT_EQ(report.problems[0].diagnostics[0].line, 10U);
    EXPECT_NE(report.problems[0].message.find(":10:1: unknown key 'tittle'"), std::string::npos)
        << report.problems[0].message;

    Ids ids;
    for (const ExtensionInfo* info : ws.manager.extensions()) {
        ids.push_back(info->id());
        EXPECT_EQ(info->state, ExtensionState::disabled);
        EXPECT_TRUE(info->verified());
        EXPECT_EQ(info->folder.parent_path().filename(), "installed");
    }
    EXPECT_EQ(ids, (Ids{"test.base", "test.future", "test.l10n", "test.post", "test.solver"}));

    const ExtensionInfo& future = ws.manager.get("test.future");
    EXPECT_FALSE(future.compatible());
    EXPECT_EQ(future.incompatibility, "it needs StressLab ^99.0, this is 1.4.0");
    EXPECT_TRUE(ws.manager.get("test.base").compatible());

    // Installed folders are skipped quietly; the broken one is tried again.
    const ScanReport again = ws.manager.scan(fixtures() / "installed", ExtensionSource::builtin);
    EXPECT_TRUE(again.installed.empty());
    EXPECT_EQ(again.problems.size(), 1U);

    const ScanReport missing =
        ws.manager.scan(fixtures() / "no-such-folder", ExtensionSource::user);
    EXPECT_TRUE(missing.installed.empty());
    EXPECT_EQ(missing.problems.size(), 1U);
}

TEST(ExtensionManagerScan, ModulesFromAFolderAreNotVerified) {
    Workspace ws;
    const std::filesystem::path folder = fixtures() / "installed" / "base";
    const ExtensionInfo& info = ws.manager.install(folder, ExtensionSource::local);
    EXPECT_EQ(info.id(), "test.base");
    EXPECT_EQ(info.source, ExtensionSource::local);
    EXPECT_FALSE(info.verified());
    EXPECT_EQ(&ws.manager.install(folder, ExtensionSource::local), &info);
    EXPECT_EQ(ws.manager.find("test.base"), &info);
    EXPECT_EQ(ws.manager.find("test.none"), nullptr);
    EXPECT_EXTENSION_ERROR(static_cast<void>(ws.manager.get("test.none")),
                           ExtensionErrc::unknown_extension);
    // The same id from another folder.
    EXPECT_EXTENSION_ERROR(ws.install(module("test.base")), ExtensionErrc::conflict);
}

TEST(ExtensionManagerScan, EngineIsThePlatformVersion) {
    EXPECT_EQ(fem::extension::engine_version().to_string(), fem::version());
}

TEST(ExtensionManagerScan, ManagerNeedsTheDocumentOfTheBus) {
    fem::model::Document edited;
    fem::model::Document other;
    fem::command::CommandBus bus(edited);
    fem::context::ContextKeys keys;
    EXPECT_EXTENSION_ERROR([&] { const ExtensionManager manager(other, bus, keys); }(),
                           ExtensionErrc::invalid_state);
}

TEST_F(ExtensionManagerTest, EnablingAddsContributionsAndDisablingRemovesThem) {
    EventLog log(ws.manager);
    EXPECT_EQ(ws.manager.enable("test.base"), Ids{"test.base"});
    EXPECT_EQ(state("test.base"), ExtensionState::enabled);
    EXPECT_EQ(log.take(), Ids{"extension.enabled test.base"});

    // The registry, the Command Bus, the context keys and the document know the module.
    EXPECT_EQ(ws.manager.registry().owners(), Ids{"test.base"});
    EXPECT_EQ(ws.manager.registry().menus().size(), 1U);
    EXPECT_EQ(ws.manager.registry().settings().size(), 1U);
    const fem::command::CommandSpec* spec = ws.bus.find_command("base.hello");
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->owner, "test.base");
    ASSERT_EQ(spec->args.size(), 1U);
    EXPECT_EQ(spec->args[0].default_value, Value("world"));
    ASSERT_NE(ws.keys.find("base.ready"), nullptr);
    EXPECT_EQ(ws.keys.find("base.ready")->owner, "test.base");
    EXPECT_EQ(ws.keys.value("base.ready"), ContextValue(false));
    EXPECT_TRUE(ws.document.is_owner_enabled("test.base"));

    // Enabling again changes nothing.
    EXPECT_TRUE(ws.manager.enable("test.base").empty());
    EXPECT_TRUE(log.take().empty());

    EXPECT_EQ(ws.manager.disable("test.base"), Ids{"test.base"});
    EXPECT_EQ(log.take(), Ids{"extension.deactivated test.base"});
    EXPECT_EQ(state("test.base"), ExtensionState::disabled);
    EXPECT_TRUE(ws.manager.registry().owners().empty());
    EXPECT_TRUE(ws.manager.registry().menus().empty());
    EXPECT_EQ(ws.bus.find_command("base.hello"), nullptr);
    EXPECT_EQ(ws.keys.find("base.ready"), nullptr);
    EXPECT_FALSE(ws.document.is_owner_enabled("test.base"));
    EXPECT_TRUE(ws.manager.disable("test.base").empty());

    // And back, without a restart.
    ws.manager.enable("test.base");
    EXPECT_NE(ws.bus.find_command("base.hello"), nullptr);
    EXPECT_EQ(ws.manager.registry().owners(), Ids{"test.base"});
}

TEST_F(ExtensionManagerTest, ObjectsOfADisabledModuleBecomeOpaque) {
    ws.manager.enable("test.base");
    const ObjectId item(static_cast<std::uint64_t>(
        ws.bus
            .execute("model.create", {{"type", "item"}, {"owner", "test.base"}, {"name", "first"}})
            .value.as_integer()));

    ws.manager.disable("test.base");
    EXPECT_TRUE(ws.document.contains(item));
    EXPECT_TRUE(ws.document.is_opaque(item));
    try {
        ws.bus.execute("model.rename", {{"object", item}, {"name", "second"}});
        FAIL();
    } catch (const ModelError& error) {
        EXPECT_EQ(error.code(), ModelErrc::opaque_object);
    }
    EXPECT_EQ(ws.document.get(item).name(), "first");

    ws.manager.enable("test.base");
    EXPECT_FALSE(ws.document.is_opaque(item));
    ws.bus.execute("model.rename", {{"object", item}, {"name", "second"}});
    EXPECT_EQ(ws.document.get(item).name(), "second");
}

TEST_F(ExtensionManagerTest, CommandIndexKeepsCommandsOfDisabledModules) {
    ws.manager.enable("test.base");
    Ids commands;
    std::vector<bool> enabled;
    for (const IndexedCommand& entry : ws.manager.command_index()) {
        commands.push_back(entry.command->id + " " + entry.extension->id());
        enabled.push_back(entry.enabled());
    }
    EXPECT_EQ(commands,
              (Ids{"base.hello test.base", "post.show test.post", "solver.run test.solver"}));
    EXPECT_EQ(enabled, (std::vector<bool>{true, false, false}));
}

TEST_F(ExtensionManagerTest, EnablingAModuleEnablesWhatItRequires) {
    EventLog log(ws.manager);
    EXPECT_EQ(ws.manager.plan_enable("test.post"), (Ids{"test.base", "test.solver", "test.post"}));
    EXPECT_EQ(ws.manager.enable("test.post"), (Ids{"test.base", "test.solver", "test.post"}));
    EXPECT_EQ(log.take(), (Ids{"extension.enabled test.base", "extension.enabled test.solver",
                               "extension.enabled test.post"}));
    EXPECT_TRUE(ws.manager.plan_enable("test.post").empty());

    // Disabling a module disables the modules that need it first.
    EXPECT_EQ(ws.manager.plan_disable("test.base"), (Ids{"test.post", "test.solver", "test.base"}));
    EXPECT_EQ(ws.manager.disable("test.solver"), (Ids{"test.post", "test.solver"}));
    EXPECT_EQ(log.take(),
              (Ids{"extension.deactivated test.post", "extension.deactivated test.solver"}));
    EXPECT_EQ(state("test.base"), ExtensionState::enabled);
    EXPECT_EQ(ws.manager.registry().owners(), Ids{"test.base"});

    // Only what is missing is enabled.
    EXPECT_EQ(ws.manager.plan_enable("test.post"), (Ids{"test.solver", "test.post"}));
}

TEST_F(ExtensionManagerTest, ModuleForAnotherPlatformIsNotEnabled) {
    try {
        ws.manager.enable("test.future");
        FAIL();
    } catch (const ExtensionError& error) {
        EXPECT_EQ(error.code(), ExtensionErrc::incompatible);
        EXPECT_EQ(std::string(error.what()),
                  "cannot enable 'test.future': it needs StressLab ^99.0, this is 1.4.0");
    }
    EXPECT_EQ(state("test.future"), ExtensionState::disabled);
    EXPECT_EXTENSION_ERROR(ws.manager.enable("test.none"), ExtensionErrc::unknown_extension);
}

/// What `enable` reports, with the code of the error.
std::string enable_error(ExtensionManager& manager, std::string_view id) {
    try {
        manager.enable(id);
    } catch (const ExtensionError& error) {
        return std::string(to_string(error.code())) + ": " + error.what();
    }
    return "enabled";
}

TEST(ExtensionManagerDependencies, ReportsWhyAModuleCannotBeEnabled) {
    Workspace ws;
    ws.install(module("test.app", "requires = [\"test.lib ^2.0\"]\n"));
    ws.install(module("test.lib"));
    ws.install(module("test.orphan", "requires = [\"test.gone\"]\n"));
    ws.install(module("test.top", "requires = [\"test.orphan\"]\n"));
    ws.install(module("test.old", "requires = [\"test.new\"]\n"));
    ws.install(
        "[extension]\nid = \"test.new\"\nname = \"New\"\nversion = \"1.0.0\"\nengine = \"^9.0\"\n");
    ws.install(module("test.x", "requires = [\"test.y\"]\n"));
    ws.install(module("test.y", "requires = [\"test.z\"]\n"));
    ws.install(module("test.z", "requires = [\"test.x >=1\"]\n"));

    EXPECT_EQ(enable_error(ws.manager, "test.app"),
              "unresolved_dependency: cannot enable 'test.app': 'test.app' needs 'test.lib' ^2.0, "
              "but version 1.0.0 is installed (test.app → test.lib)");
    EXPECT_EQ(enable_error(ws.manager, "test.top"),
              "unresolved_dependency: cannot enable 'test.top': module 'test.gone' is not "
              "installed (test.top → test.orphan → test.gone)");
    EXPECT_EQ(enable_error(ws.manager, "test.old"),
              "unresolved_dependency: cannot enable 'test.old': it needs 'test.new' (test.old → "
              "test.new), and it needs StressLab ^9.0, this is 1.4.0");
    EXPECT_EQ(enable_error(ws.manager, "test.y"),
              "dependency_cycle: cannot enable 'test.y': modules require each other (test.y → "
              "test.z → test.x → test.y)");
    EXPECT_TRUE(ws.manager.registry().owners().empty());
    for (const ExtensionInfo* info : ws.manager.extensions()) {
        EXPECT_EQ(info->state, ExtensionState::disabled) << info->id();
    }
    EXPECT_EQ(enable_error(ws.manager, "test.lib"), "enabled");
}

TEST(ExtensionManagerConflicts, ModuleWhoseIdsAreTakenIsNotEnabled) {
    Workspace ws;
    constexpr std::string_view command =
        "[[contributes.commands]]\nid = \"shared.run\"\ntitle = \"R\"\n";
    ws.install(module("test.one", command));
    ws.install(module("test.two", command));
    ws.install(
        module("test.three", std::string("requires = [\"test.four\"]\n") + std::string(command)));
    ws.install(module("test.four", "[[contributes.solvers]]\nid = \"four\"\nlabel = \"F\"\n"));
    ws.install(
        module("test.outside", "[[contributes.commands]]\nid = \"outside.run\"\ntitle = \"R\"\n"));
    ws.install(module("test.key",
                      "[[contributes.contextKeys]]\nname = \"taken.key\"\n"
                      "type = \"boolean\"\n"));

    ws.manager.enable("test.one");
    EXPECT_EQ(enable_error(ws.manager, "test.two"),
              "conflict: command 'shared.run' of module 'test.two' is already contributed by "
              "module 'test.one'");
    // Nothing of a plan is enabled when a part of it conflicts.
    EXPECT_EQ(enable_error(ws.manager, "test.three"),
              "conflict: command 'shared.run' of module 'test.three' is already contributed by "
              "module 'test.one'");
    EXPECT_EQ(ws.manager.get("test.four").state, ExtensionState::disabled);
    EXPECT_EQ(ws.manager.registry().owners(), Ids{"test.one"});
    EXPECT_EQ(ws.bus.find_command("shared.run")->owner, "test.one");

    // Commands and keys that others registered directly.
    ws.bus.register_command(
        {.id = "outside.run", .owner = "test.harness"},
        [](fem::command::CommandContext&) { return fem::command::CommandResult{}; });
    EXPECT_EQ(enable_error(ws.manager, "test.outside"),
              "conflict: command 'outside.run' of module 'test.outside' is already registered "
              "by 'test.harness'");
    ws.keys.declare({.name = "taken.key", .owner = "test.harness"});
    EXPECT_EQ(enable_error(ws.manager, "test.key"),
              "conflict: context key 'taken.key' of module 'test.key' is already declared by "
              "'test.harness'");
    EXPECT_EQ(ws.manager.registry().owners(), Ids{"test.one"});
}

TEST(ExtensionManagerLifetime, DestroyingTheManagerRemovesCommandsAndKeys) {
    fem::model::Document document;
    fem::command::CommandBus bus(document);
    fem::context::ContextKeys keys;
    {
        ExtensionManager manager(document, bus, keys, {.engine = engine});
        manager.scan(fixtures() / "installed", ExtensionSource::builtin);
        manager.enable("test.base");
        ASSERT_NE(bus.find_command("base.hello"), nullptr);
    }
    EXPECT_EQ(bus.find_command("base.hello"), nullptr);
    EXPECT_EQ(keys.find("base.ready"), nullptr);
    EXPECT_FALSE(document.is_owner_enabled("test.base"));
}

}  // namespace
