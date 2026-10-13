// Out-of-process modules (task 1.9): python modules in the extension host and a process module,
// through the Extension Manager and the Command Bus, with real processes. The fixtures
// (tests/extensions/host, tests/fixtures/process-module) are copied into a folder with Cyrillic
// letters and spaces. The host runs on the Python of the build, `python -m stresslab_host`.

#include "fem/core/extension/host_runtime.h"

#include "fem/core/command/command_bus.h"
#include "fem/core/command/value.h"
#include "fem/core/context/value.h"
#include "fem/core/extension/error.h"
#include "fem/core/extension/manager.h"
#include "fem/core/l10n/localization.h"
#include "fem/core/model/mesh.h"
#include "fem/core/model/object.h"
#include "fem/core/task/task.h"

#include "extension_test_support.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

using namespace std::chrono_literals;
using fem::command::Value;
using fem::extension::CrashReason;
using fem::extension::ExtensionErrc;
using fem::extension::ExtensionError;
using fem::extension::ExtensionEvent;
using fem::extension::ExtensionKind;
using fem::extension::ExtensionSource;
using fem::extension::ExtensionState;
using fem::extension::HostRuntime;
using fem::extension::HostRuntimeOptions;
using fem::extension::python_launcher;
using fem::extension::testing::EventLog;
using fem::extension::testing::fixtures;
using fem::extension::testing::utf8;
using fem::extension::testing::Workspace;
using fem::model::ObjectId;
using fem::task::TaskState;

/// The modules of tests/extensions/host and the process module, copied once per test process
/// into a folder with Cyrillic letters and spaces. Copies of earlier runs are removed when they
/// are an hour old.
const fs::path& modules() {
    static const fs::path folder = [] {
        const fs::path root = fs::temp_directory_path() / "stresslab-host-tests";
        std::error_code ignored;
        for (fs::directory_iterator it(root, ignored), end; !ignored && it != end;
             it.increment(ignored)) {
            std::error_code error;
            const auto age = fs::file_time_type::clock::now() - it->last_write_time(error);
            if (!error && age > std::chrono::hours(1)) {
                fs::remove_all(it->path(), error);
            }
        }
        std::random_device random;
        const fs::path run = root / std::to_string(random()) / fs::path(u8"модули с пробелами");
        fs::create_directories(run);
        fs::copy(fixtures() / "host", run, fs::copy_options::recursive);
        const fs::path program(FEM_TEST_PROCESS_MODULE);
        fs::copy_file(program, run / "process" / ("process_module" + program.extension().string()),
                      fs::copy_options::overwrite_existing);
        return run;
    }();
    return folder;
}

/// A path the module sent, UTF-8.
fs::path path_of(const Value& value) {
    const std::string& text = value.as_string();
    return fs::path(std::u8string(text.begin(), text.end()));
}

/// A workspace with the host runtime for python and process modules and every test module
/// installed.
class HostRuntimeTest : public ::testing::Test {
protected:
    HostRuntimeTest() { make_runtime({}); }

    /// Replaces the runtime; `adjust` changes the test options.
    void make_runtime(const std::function<void(HostRuntimeOptions&)>& adjust) {
        HostRuntimeOptions options;
        options.python = python_launcher(FEM_TEST_PYTHON, {"-m", "stresslab_host"},
                                         {{"PYTHONPATH", FEM_TEST_HOST_PACKAGE_DIR}});
        options.call_timeout = 30s;
        options.ping_interval = 200ms;
        options.ping_timeout = 10s;
        if (adjust) {
            adjust(options);
        }
        runtime = std::make_shared<HostRuntime>(ws.bus, options);
        ws.manager.set_runtime(ExtensionKind::python, runtime);
        ws.manager.set_runtime(ExtensionKind::process, runtime);
        for (const char* name : {"faulty", "dep-a", "dep-b", "broken", "process"}) {
            (void)ws.manager.install(modules() / name, ExtensionSource::local);
        }
    }

    Value run(std::string_view command, Value::Dict args = {}) {
        return ws.bus.execute(command, std::move(args)).value;
    }

    /// The command fails; returns its message.
    std::string failure(std::string_view command, Value::Dict args = {}) {
        try {
            (void)run(command, std::move(args));
        } catch (const ExtensionError& e) {
            EXPECT_EQ(e.code(), ExtensionErrc::command_failed) << e.what();
            return e.what();
        }
        ADD_FAILURE() << command << " did not fail";
        return {};
    }

    std::int64_t pid() { return run("faulty.info").as_dict().at("pid").as_integer(); }

    /// The crash events so far.
    std::vector<ExtensionEvent> crashes() const {
        std::vector<ExtensionEvent> found;
        for (const ExtensionEvent& event : events.events()) {
            if (event.kind == ExtensionEvent::Kind::crashed) {
                found.push_back(event);
            }
        }
        return found;
    }

    /// Calls `poll` until `condition` holds.
    bool poll_until(const std::function<bool()>& condition, std::chrono::seconds limit = 30s) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            runtime->poll();
            if (condition()) {
                return true;
            }
            std::this_thread::sleep_for(10ms);
        }
        return condition();
    }

    std::size_t things() const { return ws.document.find_by_type("thing").size(); }

    Workspace ws;
    EventLog events{ws.manager};
    std::shared_ptr<HostRuntime> runtime;
};

class FaultyModuleTest : public HostRuntimeTest {
protected:
    FaultyModuleTest() { ws.manager.enable("test.faulty"); }
};

// --- Working normally ----------------------------------------------------------------------------

TEST_F(FaultyModuleTest, RunsCommandsInProcessOfItsOwn) {
    const Value echo = run("faulty.echo", {{"value", Value::List{1, "два", 3.5}}});
    EXPECT_EQ(echo.as_dict().at("value"), (Value(Value::List{1, "два", 3.5})));
    EXPECT_EQ(echo.as_dict().at("command").as_string(), "faulty.echo");
    EXPECT_EQ(ws.manager.get("test.faulty").state, ExtensionState::active);
    EXPECT_EQ(runtime->running(), 1U);

    const Value::Dict info = run("faulty.info").as_dict();
    EXPECT_TRUE(info.at("stdout").as_boolean()) << "print() must not reach the protocol";
    const Value::List& path = info.at("path").as_list();
    ASSERT_GE(path.size(), 2U);
    EXPECT_EQ(path_of(path[0]), modules() / "faulty");
    EXPECT_EQ(path_of(path[1]), modules() / "faulty" / "site-packages");
}

TEST_F(FaultyModuleTest, ChangesOfModuleAreOneUndoStepAndOneJournalLine) {
    std::vector<std::string> journal;
    const auto subscription =
        ws.bus.on_journal([&](std::string_view entry) { journal.emplace_back(entry); });
    const Value id = run("faulty.create", {{"name", "Сталь"}});
    const fem::model::Object& object = ws.document.get(ObjectId(id.as_integer()));
    EXPECT_EQ(object.owner(), "test.faulty");
    EXPECT_EQ(object.name(), "Сталь");
    EXPECT_EQ(object.properties()["made"], "here");
    EXPECT_EQ(journal, std::vector<std::string>{R"(stresslab.cmd("faulty.create", name="Сталь"))"});
    EXPECT_TRUE(ws.bus.undo());
    EXPECT_EQ(ws.document.size(), 0U);
}

TEST_F(FaultyModuleTest, NestedCommandsMayCallTheModuleAgain) {
    const Value::List ids = run("faulty.nested", {{"name", "a"}}).as_list();
    ASSERT_EQ(ids.size(), 2U);
    EXPECT_EQ(ws.document.get(ObjectId(ids[0].as_integer())).name(), "a");
    EXPECT_EQ(ws.document.get(ObjectId(ids[1].as_integer())).name(), "a 2");
    EXPECT_EQ(ws.bus.undo_count(), 1U);
    EXPECT_TRUE(ws.bus.undo());
    EXPECT_EQ(things(), 0U);
}

TEST_F(FaultyModuleTest, ExceptionOfModuleFailsTheCommandOnly) {
    const std::int64_t before = pid();
    const std::string message = failure("faulty.raise");
    EXPECT_NE(message.find("ValueError: broken on purpose"), std::string::npos) << message;
    EXPECT_EQ(things(), 0U) << "the object it created is reverted";
    EXPECT_EQ(ws.bus.undo_count(), 0U);
    EXPECT_EQ(pid(), before) << "an exception does not restart the host";
    EXPECT_TRUE(crashes().empty());
}

TEST_F(FaultyModuleTest, CommandWithoutCodeFails) {
    const std::string message = failure("faulty.undeclared");
    EXPECT_NE(message.find("gave no code"), std::string::npos) << message;
}

TEST_F(FaultyModuleTest, BulkDataGoesThroughSharedMemoryBothWays) {
    constexpr std::int64_t nodes = 1'000'000;
    const ObjectId id(run("faulty.mesh_in", {{"nodes", nodes}}).as_integer());
    const auto mesh = ws.document.data<fem::model::Mesh>(id);
    ASSERT_TRUE(mesh);
    ASSERT_EQ(mesh->node_count(), static_cast<std::size_t>(nodes));
    EXPECT_EQ(mesh->node_ids()[nodes - 1], nodes);
    EXPECT_EQ(mesh->x()[123456], 123456.0);
    EXPECT_EQ(mesh->y()[123456], 2 * 123456.0);
    ASSERT_EQ(mesh->blocks().size(), 1U);
    EXPECT_EQ(mesh->blocks()[0].size(), static_cast<std::size_t>(nodes - 1));
    EXPECT_EQ(mesh->blocks()[0].element_nodes(nodes - 2)[1], nodes - 1);

    const Value::Dict read = run("faulty.mesh_out", {{"object", Value(id)}}).as_dict();
    EXPECT_EQ(read.at("nodes").as_integer(), nodes);
    EXPECT_EQ(read.at("sum_x").as_number(), static_cast<double>(nodes * (nodes - 1) / 2));
    EXPECT_EQ(read.at("sum_y").as_number(), static_cast<double>(nodes * (nodes - 1)));
    EXPECT_EQ(read.at("last_id").as_integer(), nodes);
    EXPECT_EQ(read.at("elements").as_integer(), nodes - 1);
    EXPECT_EQ(read.at("last_element"), (Value(Value::List{nodes - 2, nodes - 1})));
    EXPECT_FALSE(read.at("writeable").as_boolean()) << "the model is not changed past the bus";
    EXPECT_EQ(read.at("info").as_string(), "mesh");
}

TEST_F(FaultyModuleTest, SetsAndWatchesContextKeys) {
    run("faulty.set_key", {{"value", 5.0}});
    EXPECT_EQ(ws.keys.value("faulty.value").as_number(), 5.0);
    EXPECT_FALSE(run("faulty.watch", {{"when", "faulty.value == 7"}}).as_boolean());
    ws.keys.set("faulty.value", 7.0);
    ASSERT_TRUE(
        poll_until([&] { return !run("faulty.seen").as_dict().at("watched").as_list().empty(); }));
    EXPECT_EQ(run("faulty.seen").as_dict().at("watched"), (Value(Value::List{true})));
}

TEST_F(FaultyModuleTest, ModelChangesReachTheModule) {
    run("faulty.create", {{"name", "x"}});
    (void)ws.bus.execute("model.create", {{"type", "other"}, {"owner", "test.faulty"}});
    ASSERT_TRUE(poll_until(
        [&] { return run("faulty.seen").as_dict().at("changes").as_list().size() >= 2; }));
    const Value::List changes = run("faulty.seen").as_dict().at("changes").as_list();
    EXPECT_EQ(changes[0].as_string(), "faulty.create");
    EXPECT_EQ(changes[1].as_string(), "model.create");
}

TEST_F(FaultyModuleTest, RequestsFromBackgroundWaitForPoll) {
    std::atomic<int> pending = 0;
    const auto subscription = runtime->on_pending([&] { ++pending; });
    run("faulty.background", {{"name", "фон"}});
    ASSERT_TRUE(poll_until([&] { return things() == 1; }));
    EXPECT_GT(pending.load(), 0);
    EXPECT_EQ(ws.document.get(ws.document.find_by_type("thing")[0]).name(), "фон");
}

TEST_F(FaultyModuleTest, DisablingTheModuleEndsItsProcess) {
    const std::int64_t first = pid();
    EXPECT_EQ(runtime->running(), 1U);
    ws.manager.disable("test.faulty");
    EXPECT_EQ(runtime->running(), 0U);
    ws.manager.enable("test.faulty");
    EXPECT_NE(pid(), first);
    EXPECT_TRUE(crashes().empty());
}

// --- Tasks ---------------------------------------------------------------------------------------

TEST_F(FaultyModuleTest, TaskReportsProgressAndResult) {
    const auto result = ws.bus.execute("faulty.task", {{"steps", std::int64_t{20}}});
    ASSERT_TRUE(result.task);
    EXPECT_EQ(result.task->title(), "Counting");
    ASSERT_TRUE(result.task->wait_for(30s));
    EXPECT_EQ(result.task->state(), TaskState::succeeded);
    EXPECT_EQ(result.task->result(), Value(std::int64_t{40}));
    EXPECT_EQ(result.task->progress(), 1.0);
    ASSERT_FALSE(result.task->messages().empty());
    EXPECT_EQ(result.task->messages()[0].text, "starting");
}

TEST_F(FaultyModuleTest, CancellingTaskReachesTheModule) {
    const auto result = ws.bus.execute("faulty.wait_cancel");
    ASSERT_TRUE(result.task);
    EXPECT_FALSE(result.task->wait_for(100ms));
    EXPECT_TRUE(result.task->cancel());
    ASSERT_TRUE(result.task->wait_for(30s));
    EXPECT_EQ(result.task->state(), TaskState::cancelled);
}

TEST_F(FaultyModuleTest, CrashDuringTaskFailsTaskAndRestartsHost) {
    const std::int64_t before = pid();
    const auto result = ws.bus.execute("faulty.task_crash");
    ASSERT_TRUE(result.task);
    ASSERT_TRUE(result.task->wait_for(30s));
    EXPECT_EQ(result.task->state(), TaskState::failed);
    EXPECT_NE(result.task->error_message().find("stopped"), std::string::npos);
    ASSERT_TRUE(poll_until([&] { return !crashes().empty(); }));
    EXPECT_TRUE(crashes()[0].crash->restarted);
    EXPECT_NE(pid(), before);
}

// --- Crashes and hangs ---------------------------------------------------------------------------

TEST_F(FaultyModuleTest, CrashRevertsTheCommandRestartsHostAndReports) {
    run("faulty.create", {{"name", "kept"}});
    const std::int64_t before = pid();
    const std::string message = failure("faulty.crash");
    EXPECT_NE(message.find("crashed while running 'faulty.crash'"), std::string::npos) << message;
    EXPECT_NE(message.find("restarted"), std::string::npos) << message;
    // The model is intact: the object of the crashed command is gone, the earlier one is there.
    EXPECT_EQ(things(), 1U);
    EXPECT_EQ(ws.bus.undo_count(), 1U);

    const std::vector<ExtensionEvent> found = crashes();
    ASSERT_EQ(found.size(), 1U);
    EXPECT_EQ(found[0].id, "test.faulty");
    ASSERT_TRUE(found[0].crash);
    EXPECT_EQ(found[0].crash->reason, CrashReason::crashed);
    EXPECT_TRUE(found[0].crash->restarted);
    EXPECT_EQ(found[0].crash->count, 1);
    EXPECT_NE(found[0].crash->details.find("exit code 3"), std::string::npos)
        << found[0].crash->details;
    EXPECT_NE(found[0].crash->details.find("faulty: crashing now"), std::string::npos)
        << found[0].crash->details;

    // The module works again, in a new process, with its context keys and handlers back.
    EXPECT_NE(pid(), before);
    EXPECT_EQ(run("faulty.echo", {{"value", "again"}}).as_dict().at("value").as_string(), "again");
    EXPECT_EQ(ws.manager.get("test.faulty").state, ExtensionState::active);
}

TEST_F(FaultyModuleTest, UserSeesTranslatedMessage) {
    (void)failure("faulty.crash");
    ASSERT_EQ(crashes().size(), 1U);
    const auto& info = ws.manager.get("test.faulty");
    fem::l10n::Localization localization;
    localization.set_language("ru");
    EXPECT_EQ(fem::extension::crash_message(info, *crashes()[0].crash, localization),
              "Модуль «Faulty test module» аварийно завершился и перезапущен. Модель не "
              "пострадала: выполнявшаяся команда отменена.");
    localization.set_language("en");
    EXPECT_NE(fem::extension::crash_message(info, *crashes()[0].crash, localization)
                  .find("Module “Faulty test module” stopped working"),
              std::string::npos);
}

TEST_F(HostRuntimeTest, HungCommandIsEndedAndRestarted) {
    make_runtime([](HostRuntimeOptions& options) { options.call_timeout = 1s; });
    ws.manager.enable("test.faulty");
    const std::int64_t before = pid();
    const auto start = std::chrono::steady_clock::now();
    const std::string message = failure("faulty.hang");
    EXPECT_LT(std::chrono::steady_clock::now() - start, 20s);
    EXPECT_NE(message.find("stopped responding"), std::string::npos) << message;
    ASSERT_EQ(crashes().size(), 1U);
    EXPECT_EQ(crashes()[0].crash->reason, CrashReason::hung);
    EXPECT_TRUE(crashes()[0].crash->restarted);
    EXPECT_NE(pid(), before);
}

TEST_F(HostRuntimeTest, GivesUpAfterTooManyCrashes) {
    make_runtime([](HostRuntimeOptions& options) { options.restart_limit = 2; });
    ws.manager.enable("test.faulty");
    (void)failure("faulty.crash");
    (void)failure("faulty.crash");
    const std::string message = failure("faulty.crash");
    EXPECT_NE(message.find("not restarted any more"), std::string::npos) << message;
    const std::vector<ExtensionEvent> found = crashes();
    ASSERT_EQ(found.size(), 3U);
    EXPECT_TRUE(found[1].crash->restarted);
    EXPECT_FALSE(found[2].crash->restarted);
    EXPECT_EQ(found[2].crash->count, 3);
    EXPECT_NE(failure("faulty.echo").find("disable and enable it"), std::string::npos);
    EXPECT_EQ(runtime->running(), 0U);
    EXPECT_EQ(things(), 0U);

    ws.manager.disable("test.faulty");
    ws.manager.enable("test.faulty");
    EXPECT_EQ(run("faulty.echo", {{"value", 1}}).as_dict().at("value").as_integer(), 1);
}

// --- Activation and isolation --------------------------------------------------------------------

TEST_F(HostRuntimeTest, FailedActivationIsReported) {
    ws.manager.enable("test.broken");
    try {
        (void)run("broken.run");
        FAIL() << "activated";
    } catch (const ExtensionError& e) {
        EXPECT_EQ(e.code(), ExtensionErrc::activation_failed);
        EXPECT_NE(std::string(e.what()).find("cannot activate on purpose"), std::string::npos)
            << e.what();
    }
    EXPECT_EQ(events.events().back().kind, ExtensionEvent::Kind::activation_failed);
    EXPECT_EQ(runtime->running(), 0U);
}

TEST_F(HostRuntimeTest, EachPythonModuleHasItsOwnSitePackages) {
    ws.manager.enable("test.dep-a");
    ws.manager.enable("test.dep-b");
    EXPECT_EQ(run("depa.version").as_string(), "1.0");
    EXPECT_EQ(run("depb.version").as_string(), "2.0");
    EXPECT_EQ(runtime->running(), 2U);
}

TEST_F(HostRuntimeTest, UserSitePackagesFollowThoseOfTheModule) {
    const fs::path root = modules().parent_path() / fs::path(u8"пакеты пользователя");
    make_runtime([&](HostRuntimeOptions& options) { options.user_site_packages = root; });
    ws.manager.enable("test.faulty");
    const Value::List path = run("faulty.info").as_dict().at("path").as_list();
    ASSERT_GE(path.size(), 3U);
    EXPECT_EQ(path_of(path[2]), root / "test.faulty" / "site-packages");
}

// --- Process modules -----------------------------------------------------------------------------

TEST_F(HostRuntimeTest, ProcessModuleSpeaksTheSameProtocol) {
    ws.manager.enable("test.process");
    EXPECT_EQ(run("proc.echo", {{"text", "привет"}}), (Value(Value::Dict{{"text", "привет"}})));
    // No shared memory: its arrays come inline.
    const ObjectId mesh(run("proc.mesh").as_integer());
    ASSERT_EQ(ws.document.data<fem::model::Mesh>(mesh)->node_count(), 3U);
    EXPECT_EQ(ws.document.data<fem::model::Mesh>(mesh)->x()[2], 2.0);

    const std::string message = failure("proc.crash");
    EXPECT_NE(message.find("crashed"), std::string::npos) << message;
    ASSERT_EQ(crashes().size(), 1U);
    EXPECT_NE(crashes()[0].crash->details.find("exit code 9"), std::string::npos);
    EXPECT_EQ(run("proc.echo"), Value(Value::Dict{}));
}

TEST_F(HostRuntimeTest, HungProcessModuleTimesOut) {
    make_runtime([](HostRuntimeOptions& options) { options.call_timeout = 1s; });
    ws.manager.enable("test.process");
    EXPECT_NE(failure("proc.hang").find("stopped responding"), std::string::npos);
    EXPECT_EQ(crashes().at(0).crash->reason, CrashReason::hung);
    EXPECT_EQ(run("proc.echo", {{"text", "a"}}), (Value(Value::Dict{{"text", "a"}})));
}

TEST_F(HostRuntimeTest, WatchdogEndsModuleThatStopsAnswering) {
    make_runtime([](HostRuntimeOptions& options) {
        options.ping_interval = 50ms;
        options.ping_timeout = 500ms;
    });
    ws.manager.enable("test.process");
    EXPECT_TRUE(run("proc.stop_answering").as_boolean());
    ASSERT_TRUE(poll_until([&] { return !crashes().empty(); }));
    EXPECT_EQ(crashes()[0].crash->reason, CrashReason::hung);
    EXPECT_TRUE(crashes()[0].crash->restarted);
    EXPECT_EQ(run("proc.echo", {{"text", "b"}}), (Value(Value::Dict{{"text", "b"}})));
}

// --- In the application --------------------------------------------------------------------------

TEST_F(HostRuntimeTest, HostRunsOnTheInterpreterOfTheApplication) {
    make_runtime([](HostRuntimeOptions& options) {
        options.python = python_launcher(FEM_TEST_STRESSLAB, {"--python", "-m", "stresslab_host"});
    });
    ws.manager.enable("test.faulty");
    EXPECT_EQ(run("faulty.echo", {{"value", "встроенный"}}).as_dict().at("value").as_string(),
              "встроенный");
    const std::int64_t id = run("faulty.mesh_in", {{"nodes", std::int64_t{10000}}}).as_integer();
    EXPECT_EQ(ws.document.data<fem::model::Mesh>(ObjectId(id))->node_count(), 10000U);
}

}  // namespace
