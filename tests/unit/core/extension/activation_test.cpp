#include "fem/core/command/command.h"
#include "fem/core/command/command_bus.h"
#include "fem/core/command/error.h"
#include "fem/core/command/value.h"
#include "fem/core/context/error.h"
#include "fem/core/context/value.h"
#include "fem/core/extension/error.h"
#include "fem/core/extension/manager.h"
#include "fem/core/extension/manifest.h"
#include "fem/core/extension/runtime.h"

#include "extension_test_support.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fem::command::CommandContext;
using fem::command::Value;
using fem::context::ContextValue;
using fem::extension::ActivationEvent;
using fem::extension::ExtensionErrc;
using fem::extension::ExtensionError;
using fem::extension::ExtensionHost;
using fem::extension::ExtensionKind;
using fem::extension::ExtensionSource;
using fem::extension::ExtensionState;
using fem::extension::testing::EventLog;
using fem::extension::testing::FakeRuntime;
using fem::extension::testing::fixtures;
using fem::extension::testing::utf8;
using fem::extension::testing::Workspace;
using Ids = std::vector<std::string>;

/// The modules of tests/extensions/installed, with a runtime for their python code.
class ActivationTest : public ::testing::Test {
protected:
    ActivationTest() {
        ws.manager.scan(fixtures() / "installed", ExtensionSource::builtin);
        ws.manager.set_runtime(ExtensionKind::python, runtime);
    }

    [[nodiscard]] ExtensionState state(std::string_view id) const {
        return ws.manager.get(id).state;
    }

    Workspace ws;
    std::shared_ptr<FakeRuntime> runtime = std::make_shared<FakeRuntime>();
};

TEST_F(ActivationTest, CodeIsLoadedOnlyWhenAnEventComes) {
    EventLog log(ws.manager);
    ws.manager.enable("test.post");
    EXPECT_TRUE(runtime->take().empty());
    EXPECT_EQ(state("test.base"), ExtensionState::enabled);
    log.take();

    // test.base waits for the start, test.solver for its solver, test.post for "solver.run".
    ws.manager.startup();
    EXPECT_TRUE(ws.manager.started());
    EXPECT_EQ(runtime->take(), Ids{"activate test.base"});
    EXPECT_EQ(log.take(), Ids{"extension.activated test.base"});
    EXPECT_EQ(state("test.base"), ExtensionState::active);

    EXPECT_TRUE(ws.manager.activate_on(ActivationEvent::solver_requested("other")).empty());
    EXPECT_EQ(ws.manager.activate_on(ActivationEvent::solver_requested("demo")),
              Ids{"test.solver"});
    EXPECT_EQ(runtime->take(), Ids{"activate test.solver"});
    EXPECT_EQ(state("test.post"), ExtensionState::enabled);
    // Active modules are not activated again.
    EXPECT_TRUE(ws.manager.activate_on(ActivationEvent::solver_requested("demo")).empty());
}

TEST_F(ActivationTest, OpeningAFileActivatesItsModuleAfterWhatItRequires) {
    ws.manager.enable("test.solver");
    const std::filesystem::path file =
        std::filesystem::path(u8"папка с пробелами") / std::filesystem::path(u8"Модель.RES");
    EXPECT_EQ(ws.manager.activate_on(ActivationEvent::open_file(file)),
              (Ids{"test.base", "test.solver"}));
    EXPECT_EQ(runtime->take(), (Ids{"activate test.base", "activate test.solver"}));
}

TEST_F(ActivationTest, RunningACommandActivatesItsModule) {
    std::vector<std::string> journal;
    const fem::Subscription recording =
        ws.bus.on_journal([&](std::string_view entry) { journal.emplace_back(entry); });
    runtime->handlers["solver.run"] = [](CommandContext& context) {
        return fem::command::CommandResult(Value("ran " + context.args().string("job")));
    };
    ws.manager.enable("test.post");

    const Value result = ws.bus.execute("solver.run", {{"job", utf8(u8"крыло")}}).value;
    EXPECT_EQ(result, Value(utf8(u8"ran крыло")));
    // test.post waits for "solver.run" too; required modules come first.
    EXPECT_EQ(runtime->take(), (Ids{"activate test.base", "activate test.solver",
                                    "activate test.post", "execute solver.run"}));
    EXPECT_EQ(journal, Ids{utf8(u8"stresslab.cmd(\"solver.run\", job=\"крыло\")")});

    // Arguments are checked against the manifest before any code runs.
    try {
        ws.bus.execute("solver.run", {});
        FAIL();
    } catch (const fem::command::CommandError& error) {
        EXPECT_EQ(error.code(), fem::command::CommandErrc::invalid_arguments);
    }
    EXPECT_TRUE(runtime->take().empty());
}

TEST_F(ActivationTest, FailedActivationIsReportedAndNotRepeatedByEvents) {
    EventLog log(ws.manager);
    runtime->broken.insert("test.base");
    ws.manager.enable("test.solver");
    log.take();

    ws.manager.startup();
    EXPECT_EQ(runtime->take(), Ids{"activate test.base"});
    ASSERT_EQ(log.events().size(), 3U);
    EXPECT_EQ(log.take(), Ids{"extension.activationFailed test.base"});
    EXPECT_EQ(log.events().back().message, "broken on purpose");
    EXPECT_EQ(state("test.base"), ExtensionState::enabled);
    EXPECT_EQ(ws.manager.get("test.base").activation_error, "broken on purpose");

    // Not tried again by events or by its commands...
    ws.manager.startup();
    EXPECT_EXTENSION_ERROR(ws.bus.execute("base.hello"), ExtensionErrc::activation_failed);
    EXPECT_TRUE(runtime->take().empty());
    // ...but a module that needs it reports why it cannot start.
    try {
        ws.bus.execute("solver.run", {{"job", "j"}});
        FAIL();
    } catch (const ExtensionError& error) {
        EXPECT_EQ(error.code(), ExtensionErrc::activation_failed);
        EXPECT_EQ(std::string(error.what()),
                  "module 'test.solver': required module 'test.base' failed to activate: broken "
                  "on purpose");
    }
    runtime->take();

    // An explicit activation tries again.
    runtime->broken.clear();
    ws.manager.activate("test.base");
    EXPECT_EQ(state("test.base"), ExtensionState::active);
    EXPECT_TRUE(ws.manager.get("test.base").activation_error.empty());
}

TEST_F(ActivationTest, ModulesOfAKindWithoutRuntimeStayEnabled) {
    ws.manager.set_runtime(ExtensionKind::python, nullptr);
    EventLog log(ws.manager);
    ws.manager.enable("test.base");
    ws.manager.startup();
    EXPECT_EQ(log.take(),
              (Ids{"extension.enabled test.base", "extension.activationFailed test.base"}));
    EXPECT_EQ(log.events().back().message, "no runtime for python modules is available");
    EXPECT_EQ(state("test.base"), ExtensionState::enabled);
    // Its contributions are there: the shell shows them, and running one says why it fails.
    EXPECT_EQ(ws.manager.registry().commands().size(), 1U);
    EXPECT_EXTENSION_ERROR(ws.bus.execute("base.hello"), ExtensionErrc::activation_failed);
}

TEST_F(ActivationTest, DisablingDeactivatesTheCodeAndEnablingStartsOver) {
    ws.manager.enable("test.post");
    ws.bus.execute("solver.run", {{"job", "j"}});
    runtime->take();
    EventLog log(ws.manager);

    EXPECT_EQ(ws.manager.disable("test.base"), (Ids{"test.post", "test.solver", "test.base"}));
    EXPECT_EQ(runtime->take(),
              (Ids{"deactivate test.post", "deactivate test.solver", "deactivate test.base"}));
    EXPECT_EQ(log.take(),
              (Ids{"extension.deactivated test.post", "extension.deactivated test.solver",
                   "extension.deactivated test.base"}));
    // Python code is not kept loaded.
    EXPECT_FALSE(ws.manager.get("test.base").code_loaded);

    ws.manager.enable("test.post");
    EXPECT_EQ(state("test.solver"), ExtensionState::enabled);
    ws.bus.execute("solver.run", {{"job", "j"}});
    EXPECT_EQ(runtime->take(), (Ids{"activate test.base", "activate test.solver",
                                    "activate test.post", "execute solver.run"}));
}

TEST_F(ActivationTest, AfterStartupEnabledModulesStartAtOnce) {
    ws.manager.startup();
    EXPECT_TRUE(runtime->take().empty());
    ws.manager.enable("test.base");
    EXPECT_EQ(runtime->take(), Ids{"activate test.base"});
    EXPECT_EQ(state("test.base"), ExtensionState::active);
}

TEST_F(ActivationTest, CodeSetsTheContextKeysOfItsModule) {
    ws.keys.declare({.name = "harness.key", .owner = "test.harness"});
    runtime->on_activate = [](ExtensionHost& host) {
        EXPECT_EQ(host.extension().id(), "test.base");
        host.set_context_key("base.ready", true);
        host.declare_context_key("base.count", ContextValue::Kind::number, 3);
    };
    ws.manager.enable("test.base");
    ws.manager.activate("test.base");
    EXPECT_EQ(ws.keys.value("base.ready"), ContextValue(true));
    EXPECT_EQ(ws.keys.value("base.count"), ContextValue(3));
    ASSERT_NE(ws.keys.find("base.count"), nullptr);
    EXPECT_EQ(ws.keys.find("base.count")->owner, "test.base");

    // Keys of other owners are not the module's to set.
    ws.manager.disable("test.base");
    EXPECT_EQ(ws.keys.find("base.count"), nullptr);
    runtime->on_activate = [](ExtensionHost& host) { host.set_context_key("harness.key", true); };
    ws.manager.enable("test.base");
    try {
        ws.manager.activate("test.base");
        FAIL();
    } catch (const ExtensionError& error) {
        EXPECT_EQ(error.code(), ExtensionErrc::activation_failed);
        EXPECT_NE(std::string(error.what()).find("belongs to 'test.harness'"), std::string::npos)
            << error.what();
    }
    EXPECT_EQ(ws.keys.value("harness.key"), ContextValue());
}

TEST_F(ActivationTest, ModuleWithoutCodeHasNothingToActivate) {
    ws.manager.enable("test.l10n");
    ws.manager.activate("test.l10n");
    ws.manager.startup();
    EXPECT_EQ(state("test.l10n"), ExtensionState::enabled);
    EXPECT_TRUE(runtime->take().empty());
    EXPECT_EQ(ws.manager.registry().keybindings().size(), 1U);
    EXPECT_EXTENSION_ERROR(ws.manager.activate("test.base"), ExtensionErrc::invalid_state);
}

TEST(ActivationLifetime, ModuleCannotBeDisabledWhileItsCommandRuns) {
    Workspace ws;
    auto runtime = std::make_shared<FakeRuntime>();
    ws.manager.set_runtime(ExtensionKind::python, runtime);
    ws.install(R"([extension]
id = "test.self"
name = "Self"
version = "1.0.0"
engine = "^1.0"
kind = "python"
entry = "self:activate"

[[contributes.commands]]
id = "self.off"
title = "Disable myself"
)");
    runtime->handlers["self.off"] = [&ws](CommandContext&) {
        ws.manager.disable("test.self");
        return fem::command::CommandResult{};
    };
    ws.manager.enable("test.self");
    EXPECT_EXTENSION_ERROR(ws.bus.execute("self.off"), ExtensionErrc::invalid_state);
    EXPECT_EQ(ws.manager.get("test.self").state, ExtensionState::active);
    // Once the command is over, it can.
    EXPECT_EQ(ws.manager.disable("test.self"), Ids{"test.self"});
}

}  // namespace
