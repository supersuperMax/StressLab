// The native test module (tests/extensions/native-hello/hello.c) through the C ABI. It is copied
// with its manifest into a folder with Cyrillic letters and spaces, as a module installed by a user
// would be.

#include "fem/core/command/value.h"
#include "fem/core/context/value.h"
#include "fem/core/extension/error.h"
#include "fem/core/extension/manager.h"
#include "fem/core/extension/manifest.h"
#include "fem/core/model/object_id.h"

#include "extension_test_support.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

using fem::command::Value;
using fem::context::ContextValue;
using fem::extension::ExtensionErrc;
using fem::extension::ExtensionError;
using fem::extension::ExtensionInfo;
using fem::extension::ExtensionSource;
using fem::extension::ExtensionState;
using fem::extension::manifest_file_name;
using fem::extension::testing::EventLog;
using fem::extension::testing::fixtures;
using fem::extension::testing::utf8;
using fem::extension::testing::Workspace;
using fem::model::ObjectId;

/// The folder of the copied module, made once per process. A loaded library cannot be deleted on
/// Windows, so copies of earlier runs are removed when they are an hour old.
const fs::path& module_folder() {
    static const fs::path folder = [] {
        const fs::path root = fs::temp_directory_path() / "stresslab-native-tests";
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
        const fs::path run = root / std::to_string(random()) / fs::path(u8"модуль с пробелами");
        fs::create_directories(run);
        for (const fs::path& library :
             {fs::path(FEM_TEST_NATIVE_HELLO), fs::path(FEM_TEST_NATIVE_BAD_ABI)}) {
            fs::copy_file(library, run / library.filename(), fs::copy_options::overwrite_existing);
        }
        fs::copy_file(fixtures() / "native-hello" / std::string(manifest_file_name),
                      run / std::string(manifest_file_name), fs::copy_options::overwrite_existing);
        return run;
    }();
    return folder;
}

class NativeModuleTest : public ::testing::Test {
protected:
    NativeModuleTest() : info(ws.manager.install(module_folder(), ExtensionSource::local)) {}

    [[nodiscard]] double key(std::string_view name) const {
        return ws.keys.value(name).as_number();
    }

    Workspace ws;
    const ExtensionInfo& info;
};

TEST_F(NativeModuleTest, CommandsRunThroughTheCAbi) {
    EventLog log(ws.manager);
    std::vector<std::string> journal;
    const fem::Subscription recording =
        ws.bus.on_journal([&](std::string_view entry) { journal.emplace_back(entry); });
    ws.manager.enable("test.native");
    EXPECT_FALSE(info.verified());
    EXPECT_EQ(info.state, ExtensionState::enabled);

    // The command loads the library, which creates an object with the arguments as properties.
    const Value result = ws.bus.execute("hello.greet", {{"name", utf8(u8"мир")}}).value;
    EXPECT_EQ(info.state, ExtensionState::active);
    EXPECT_TRUE(info.code_loaded);
    EXPECT_EQ(log.take(), (std::vector<std::string>{"extension.enabled test.native",
                                                    "extension.activated test.native"}));
    const ObjectId greeting(static_cast<std::uint64_t>(result.as_integer()));
    const auto& object = ws.document.get(greeting);
    EXPECT_EQ(object.owner(), "test.native");
    EXPECT_EQ(object.type(), "greeting");
    EXPECT_EQ(object.properties(), nlohmann::json({{"name", utf8(u8"мир")}}));
    EXPECT_EQ(journal,
              std::vector<std::string>{utf8(u8"stresslab.cmd(\"hello.greet\", name=\"мир\")")});
    EXPECT_GE(key("hello.activations"), 1.0);

    // The nested "model.create" is a part of the module's command: one step of undo.
    ASSERT_TRUE(ws.bus.undo());
    EXPECT_FALSE(ws.document.contains(greeting));
    ASSERT_TRUE(ws.bus.redo());
    EXPECT_TRUE(ws.document.contains(greeting));
    EXPECT_FALSE(ws.bus.can_redo());
}

TEST_F(NativeModuleTest, FailingCommandLeavesTheModelAsItWas) {
    ws.manager.enable("test.native");
    const std::size_t objects = ws.document.size();
    try {
        ws.bus.execute("hello.fail");
        FAIL();
    } catch (const ExtensionError& error) {
        EXPECT_EQ(error.code(), ExtensionErrc::command_failed);
        EXPECT_EQ(std::string(error.what()),
                  "command 'hello.fail' failed: hello.fail fails on purpose");
    }
    EXPECT_EQ(ws.document.size(), objects);
    EXPECT_FALSE(ws.bus.can_undo());
}

TEST_F(NativeModuleTest, LibraryStaysLoadedWhileTheModuleIsDisabled) {
    ws.manager.enable("test.native");
    ws.manager.activate("test.native");
    const double activations = key("hello.activations");
    const double deactivations = key("hello.deactivations");

    EventLog log(ws.manager);
    ws.manager.disable("test.native");
    EXPECT_EQ(log.take(), std::vector<std::string>{"extension.deactivated test.native"});
    EXPECT_EQ(info.state, ExtensionState::disabled);
    EXPECT_TRUE(info.code_loaded);
    EXPECT_EQ(ws.bus.find_command("hello.greet"), nullptr);
    EXPECT_EQ(ws.keys.find("hello.activations"), nullptr);

    // Enabled again: the same library, whose counters were kept, is activated again.
    ws.manager.enable("test.native");
    ws.manager.activate("test.native");
    EXPECT_EQ(key("hello.activations"), activations + 1);
    EXPECT_EQ(key("hello.deactivations"), deactivations + 1);
    ws.bus.execute("hello.greet", {{"name", "again"}});
}

TEST_F(NativeModuleTest, ObjectsOfTheModuleAreKeptWhileItIsDisabled) {
    ws.manager.enable("test.native");
    const ObjectId greeting(static_cast<std::uint64_t>(
        ws.bus.execute("hello.greet", {{"name", "x"}}).value.as_integer()));
    ws.manager.disable("test.native");
    EXPECT_TRUE(ws.document.is_opaque(greeting));
    ws.manager.enable("test.native");
    EXPECT_FALSE(ws.document.is_opaque(greeting));
}

/// Installs a native module of the copied folder whose `entry` is `entry`, and activates it.
std::string activation_error(Workspace& ws, std::string_view id, std::string_view entry) {
    ws.manager.install(fem::extension::testing::manifest(
                           "[extension]\nid = \"" + std::string(id) +
                           "\"\nname = \"N\"\nversion = \"1.0.0\"\nengine = \"^1.0\"\n"
                           "kind = \"native\"\nentry = \"" +
                           std::string(entry) + "\"\n"),
                       module_folder(), ExtensionSource::local);
    ws.manager.enable(id);
    try {
        ws.manager.activate(id);
    } catch (const ExtensionError& error) {
        EXPECT_EQ(error.code(), ExtensionErrc::activation_failed);
        EXPECT_FALSE(ws.manager.get(id).activation_error.empty());
        return error.what();
    }
    return "activated";
}

TEST_F(NativeModuleTest, LibraryOfAnotherAbiVersionIsRefused) {
    EXPECT_EQ(activation_error(ws, "test.bad-abi", "hello_bad_abi"),
              "module 'test.bad-abi': the module is built for ABI version 999, this platform "
              "supports version 1");
}

TEST_F(NativeModuleTest, MissingLibraryIsReported) {
    const std::string error = activation_error(ws, "test.missing", "bin/missing");
    EXPECT_NE(error.find("module 'test.missing': cannot load '"), std::string::npos) << error;
    EXPECT_NE(error.find(utf8(u8"модуль с пробелами/bin/missing")), std::string::npos) << error;
}

}  // namespace
