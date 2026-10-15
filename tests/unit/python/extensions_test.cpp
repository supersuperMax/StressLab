// `stresslab.extensions` in the Python console of the window (task 1.15): the module fem.demo the
// application ships goes away and comes back from the console, its note stays in the model as an
// opaque object, and the calls reach the journal of the session. The code of the module runs in a
// runtime of the test here; the real extension host is tested by python/tests/test_demo.py.

#include "fem/core/command/command.h"
#include "fem/core/command/command_bus.h"
#include "fem/core/command/journal.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/context/core_keys.h"
#include "fem/core/extension/manager.h"
#include "fem/core/extension/runtime.h"
#include "fem/core/model/document.h"
#include "fem/python/console.h"

#include "python_test_support.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using fem::command::CommandContext;
using fem::command::CommandResult;
using fem::extension::ExtensionHost;
using fem::extension::ExtensionInfo;
using fem::extension::ExtensionInstance;
using fem::extension::ExtensionKind;
using fem::extension::ExtensionSource;
using fem::extension::ExtensionState;
using fem::python::Console;
using fem::python::ConsoleReply;
using fem::python::testing::interpreter;

/// Stands in for the extension host: "demo.add_note" creates a note, as the module's code does.
class DemoRuntime final : public fem::extension::ExtensionRuntime {
public:
    [[nodiscard]] std::unique_ptr<ExtensionInstance> activate(const ExtensionInfo& /*extension*/,
                                                              ExtensionHost& /*host*/) override {
        return std::make_unique<Instance>();
    }

private:
    class Instance final : public ExtensionInstance {
    public:
        CommandResult execute(CommandContext& context) override {
            return context.execute("model.create",
                                   {{"type", "demo_note"}, {"owner", "fem.demo"}, {"name", "#1"}});
        }
        void deactivate() noexcept override {}
    };
};

class ExtensionsFromTheConsole : public ::testing::Test {
protected:
    ExtensionsFromTheConsole() {
        manager.set_runtime(ExtensionKind::python, std::make_shared<DemoRuntime>());
        (void)manager.install(std::filesystem::path(FEM_BUILTIN_EXTENSIONS_DIR) / "fem.demo",
                              ExtensionSource::builtin);
        (void)manager.enable("fem.demo");
        interpreter().attach(bus, keys, &manager);
    }
    ~ExtensionsFromTheConsole() override { interpreter().detach(); }
    ExtensionsFromTheConsole(const ExtensionsFromTheConsole&) = delete;
    ExtensionsFromTheConsole& operator=(const ExtensionsFromTheConsole&) = delete;
    ExtensionsFromTheConsole(ExtensionsFromTheConsole&&) = delete;
    ExtensionsFromTheConsole& operator=(ExtensionsFromTheConsole&&) = delete;

    /// Runs a line in the console; returns what it printed, errors included.
    std::string push(std::string_view line) {
        const ConsoleReply reply = console.push(line);
        std::string text;
        for (const auto& output : reply.output) {
            text += output.text;
        }
        return text;
    }

    [[nodiscard]] ExtensionState state() const { return manager.get("fem.demo").state; }

    fem::model::Document document;
    fem::command::CommandBus bus{document};
    fem::context::ContextKeys keys;
    fem::context::CoreKeys core_keys{bus, keys};
    fem::extension::ExtensionManager manager{document, bus, keys};
    fem::command::SessionJournal journal{bus};
    Console console;
};

TEST_F(ExtensionsFromTheConsole, TheModuleGoesAwayAndComesBack) {
    EXPECT_EQ(push("note = stresslab.demo.add_note()"), "");
    ASSERT_EQ(document.size(), 1U);
    const fem::model::Object& note = document.get(document.find_by_type("demo_note").at(0));

    EXPECT_EQ(push("stresslab.extensions.disable('fem.demo')"), "['fem.demo']\n");
    EXPECT_EQ(state(), ExtensionState::disabled);
    EXPECT_EQ(bus.find_command("demo.add_note"), nullptr);
    EXPECT_EQ(manager.registry().find("fem.demo"), nullptr) << "its contributions are gone";
    ASSERT_EQ(document.size(), 1U) << "its note stays in the model";
    EXPECT_TRUE(document.is_opaque(note));
    EXPECT_EQ(push("stresslab.active_document()[note].is_opaque"), "True\n");
    EXPECT_NE(push("stresslab.demo.add_note()").find("AttributeError"), std::string::npos);

    EXPECT_EQ(push("stresslab.extensions.enable('fem.demo')"), "['fem.demo']\n");
    EXPECT_EQ(state(), ExtensionState::enabled);
    EXPECT_NE(bus.find_command("demo.add_note"), nullptr);
    EXPECT_NE(manager.registry().find("fem.demo"), nullptr);
    EXPECT_FALSE(document.is_opaque(note));
    EXPECT_EQ(push("stresslab.extensions.get('fem.demo')"),
              "<stresslab.ExtensionInfo fem.demo 1.0.0 enabled>\n");

    const std::vector<std::string> expected{"stresslab.cmd(\"demo.add_note\")",
                                            "stresslab.extensions.disable(\"fem.demo\")",
                                            "stresslab.extensions.enable(\"fem.demo\")"};
    EXPECT_EQ(journal.entries(), expected);
}

TEST_F(ExtensionsFromTheConsole, DescribesTheInstalledModules) {
    EXPECT_EQ(push("[m.id for m in stresslab.extensions.installed()]"), "['fem.demo']\n");
    EXPECT_EQ(push("m = stresslab.extensions.get('fem.demo')"), "");
    EXPECT_EQ(push("(m.name, m.version, m.kind, m.source, m.state, m.enabled)"),
              "('%name%', '1.0.0', 'python', 'builtin', 'enabled', True)\n");
    EXPECT_EQ(push("m.folder.name"), "'fem.demo'\n");
}

TEST_F(ExtensionsFromTheConsole, ErrorsAreExtensionErrorsAndChangeNothing) {
    EXPECT_EQ(push("try:"), "");
    EXPECT_EQ(push("    stresslab.extensions.enable('fem.nothing')"), "");
    EXPECT_EQ(push("except stresslab.ExtensionError as error:"), "");
    EXPECT_EQ(push("    print(error.code)"), "");
    EXPECT_EQ(push(""), "unknown_extension\n");

    EXPECT_EQ(push("with stresslab.transaction('t'):"), "");
    EXPECT_EQ(push("    stresslab.extensions.disable('fem.demo')"), "");
    const std::string refused = push("");
    EXPECT_NE(refused.find("ExtensionError"), std::string::npos) << refused;
    EXPECT_NE(refused.find("inside a command or a transaction"), std::string::npos) << refused;
    EXPECT_EQ(state(), ExtensionState::enabled);
    EXPECT_TRUE(journal.entries().empty());
}

TEST_F(ExtensionsFromTheConsole, AfterDetachModulesAreNotManaged) {
    interpreter().detach();
    const std::string refused = push("stresslab.extensions.installed()");
    EXPECT_NE(refused.find("managed by the application"), std::string::npos) << refused;
    interpreter().attach(bus, keys, &manager);
}

}  // namespace
