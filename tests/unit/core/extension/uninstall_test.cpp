// Removing modules from the index, reading their manifests again and the modules a model needs
// (task 1.14: the module manager).

#include "fem/core/command/command_bus.h"
#include "fem/core/extension/error.h"
#include "fem/core/extension/manager.h"
#include "fem/core/extension/manifest.h"
#include "fem/core/model/object_id.h"

#include "extension_test_support.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using fem::extension::ExtensionErrc;
using fem::extension::ExtensionError;
using fem::extension::ExtensionInfo;
using fem::extension::ExtensionSource;
using fem::extension::ExtensionState;
using fem::extension::ExtensionUpdate;
using fem::extension::manifest_file_name;
using fem::extension::ManifestError;
using fem::extension::NeededModule;
using fem::extension::ReloadReport;
using fem::extension::testing::EventLog;
using fem::extension::testing::fixtures;
using fem::extension::testing::Workspace;
using fem::model::ObjectId;
using Ids = std::vector<std::string>;

/// A folder of modules that the test writes, with Cyrillic and spaces in its path.
class ModuleFolders {
public:
    ModuleFolders() : root_(fs::temp_directory_path() / fs::path(u8"модули StressLab для теста")) {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        fs::create_directories(root_);
    }
    ~ModuleFolders() {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }
    ModuleFolders(const ModuleFolders&) = delete;
    ModuleFolders& operator=(const ModuleFolders&) = delete;

    /// Writes the manifest of a module into the folder `name` and returns the folder.
    fs::path write(const fs::path& name, std::string_view manifest) const {
        const fs::path folder = root_ / name;
        fs::create_directories(folder);
        std::ofstream out(folder / std::string(manifest_file_name), std::ios::binary);
        out << manifest;
        return folder;
    }

private:
    fs::path root_;
};

std::string module(std::string_view id, std::string_view version, std::string_view rest = {}) {
    return "[extension]\nid = \"" + std::string(id) + "\"\nname = \"" + std::string(id) +
           "\"\nversion = \"" + std::string(version) +
           "\"\nengine = \"^1.0\"\nkind = \"python\"\nentry = \"m:activate\"\n" + std::string(rest);
}

std::string command(std::string_view id) {
    return "[[contributes.commands]]\nid = \"" + std::string(id) + "\"\ntitle = \"T\"\n";
}

ObjectId create(Workspace& ws, std::string_view owner) {
    static int count = 0;
    return ObjectId(static_cast<std::uint64_t>(
        ws.bus
            .execute("model.create", {{"type", "item"},
                                      {"owner", std::string(owner)},
                                      {"name", "item " + std::to_string(++count)}})
            .value.as_integer()));
}

TEST(ExtensionManagerIndex, InstallingSendsAnEvent) {
    Workspace ws;
    EventLog log(ws.manager);
    ws.install(module("test.one", "1.0.0"));
    (void)ws.manager.scan(fixtures() / "installed", ExtensionSource::builtin);
    EXPECT_EQ(log.take(),
              (Ids{"extension.installed test.one", "extension.installed test.base",
                   "extension.installed test.future", "extension.installed test.l10n",
                   "extension.installed test.post", "extension.installed test.solver"}));
    // Installing a folder again sends nothing.
    (void)ws.manager.install(fixtures() / "installed" / "base", ExtensionSource::builtin);
    EXPECT_TRUE(log.take().empty());
}

TEST(ExtensionManagerIndex, UninstallingDisablesTheModuleAndForgetsIt) {
    Workspace ws;
    (void)ws.manager.scan(fixtures() / "installed", ExtensionSource::local);
    ws.manager.enable("test.post");
    const ObjectId item = create(ws, "test.solver");
    EventLog log(ws.manager);

    ws.manager.uninstall("test.solver");
    EXPECT_EQ(log.take(),
              (Ids{"extension.deactivated test.post", "extension.deactivated test.solver",
                   "extension.uninstalled test.solver"}));
    EXPECT_EQ(ws.manager.find("test.solver"), nullptr);
    EXPECT_EQ(ws.bus.find_command("solver.run"), nullptr);
    EXPECT_EQ(ws.manager.get("test.base").state, ExtensionState::enabled);
    // What needs it stays installed but cannot be enabled.
    EXPECT_EQ(ws.manager.get("test.post").state, ExtensionState::disabled);
    EXPECT_EXTENSION_ERROR((void)ws.manager.plan_enable("test.post"),
                           ExtensionErrc::unresolved_dependency);
    // Its objects stay in the model, opaque.
    EXPECT_TRUE(ws.document.is_opaque(item));
    const std::vector<NeededModule> needed = ws.manager.needed_modules();
    ASSERT_EQ(needed.size(), 1U);
    EXPECT_EQ(needed[0].id, "test.solver");
    EXPECT_EQ(needed[0].objects, 1U);
    EXPECT_EQ(needed[0].extension, nullptr);
    EXPECT_FALSE(needed[0].can_enable());

    // Installed again from its folder, it is the module the objects need.
    ws.manager.install(fixtures() / "installed" / "solver", ExtensionSource::local);
    ws.manager.enable("test.post");
    EXPECT_FALSE(ws.document.is_opaque(item));
    EXPECT_TRUE(ws.manager.needed_modules().empty());

    EXPECT_EXTENSION_ERROR(ws.manager.uninstall("test.none"), ExtensionErrc::unknown_extension);
}

TEST(ExtensionManagerIndex, ModulesOfTheApplicationCannotBeUninstalled) {
    Workspace ws;
    (void)ws.manager.scan(fixtures() / "installed", ExtensionSource::builtin);
    ws.manager.enable("test.base");
    EXPECT_EXTENSION_ERROR(ws.manager.uninstall("test.base"), ExtensionErrc::invalid_state);
    EXPECT_EQ(ws.manager.get("test.base").state, ExtensionState::enabled);
}

TEST(ExtensionManagerIndex, NeededModulesAreTheOwnersOfOpaqueObjects) {
    Workspace ws;
    (void)ws.manager.scan(fixtures() / "installed", ExtensionSource::builtin);
    ws.manager.enable("test.solver");
    (void)create(ws, "test.base");
    (void)create(ws, "test.base");
    (void)create(ws, "test.solver");
    EXPECT_TRUE(ws.manager.needed_modules().empty());

    ws.manager.disable("test.base");
    const std::vector<NeededModule> needed = ws.manager.needed_modules();
    ASSERT_EQ(needed.size(), 2U);
    EXPECT_EQ(needed[0].id, "test.base");
    EXPECT_EQ(needed[0].objects, 2U);
    EXPECT_EQ(needed[0].extension, &ws.manager.get("test.base"));
    EXPECT_TRUE(needed[0].can_enable());
    EXPECT_EQ(needed[1].id, "test.solver");
    EXPECT_EQ(needed[1].objects, 1U);
}

TEST(ExtensionManagerIndex, ReloadingReadsTheChangedManifestAndEnablesAgain) {
    Workspace ws;
    const ModuleFolders folders;
    const fs::path dev =
        folders.write(u8"мой модуль", module("test.dev", "1.0.0", command("dev.one")));
    folders.write("user", module("test.user", "1.0.0", "requires = [\"test.dev ^1.0\"]\n"));
    const ExtensionInfo& info = ws.manager.install(dev, ExtensionSource::local);
    (void)ws.manager.install(dev.parent_path() / "user", ExtensionSource::local);
    ws.manager.enable("test.user");
    EXPECT_TRUE(ws.manager.updates().empty());

    folders.write(u8"мой модуль", module("test.dev", "1.1.0", command("dev.two")));
    const std::vector<ExtensionUpdate> updates = ws.manager.updates();
    ASSERT_EQ(updates.size(), 1U);
    EXPECT_EQ(updates[0].id, "test.dev");
    EXPECT_EQ(updates[0].installed.to_string(), "1.0.0");
    EXPECT_EQ(updates[0].available.to_string(), "1.1.0");

    EventLog log(ws.manager);
    const ReloadReport report = ws.manager.reload("test.dev");
    EXPECT_EQ(log.take(), (Ids{"extension.deactivated test.user", "extension.deactivated test.dev",
                               "extension.updated test.dev", "extension.enabled test.dev",
                               "extension.enabled test.user"}));
    EXPECT_EQ(report.enabled, (Ids{"test.dev", "test.user"}));
    EXPECT_TRUE(report.failed.empty());
    EXPECT_EQ(&ws.manager.get("test.dev"), &info);
    EXPECT_EQ(info.manifest->version.to_string(), "1.1.0");
    EXPECT_EQ(ws.bus.find_command("dev.one"), nullptr);
    EXPECT_NE(ws.bus.find_command("dev.two"), nullptr);
    EXPECT_TRUE(ws.manager.updates().empty());

    // A version the module that needs it does not accept: that one stays disabled.
    folders.write(u8"мой модуль", module("test.dev", "2.0.0"));
    const ReloadReport major = ws.manager.reload("test.dev");
    EXPECT_EQ(major.enabled, Ids{"test.dev"});
    ASSERT_EQ(major.failed.size(), 1U);
    EXPECT_EQ(major.failed[0].id, "test.user");
    EXPECT_EQ(ws.manager.get("test.user").state, ExtensionState::disabled);

    // A broken manifest or another module in the folder changes nothing.
    folders.write(u8"мой модуль", "[extension]\nid = \"test.dev\"\n");
    EXPECT_THROW((void)ws.manager.reload("test.dev"), ManifestError);
    folders.write(u8"мой модуль", module("test.other", "1.0.0"));
    EXPECT_EXTENSION_ERROR((void)ws.manager.reload("test.dev"), ExtensionErrc::conflict);
    EXPECT_EQ(info.manifest->version.to_string(), "2.0.0");
    EXPECT_EQ(info.state, ExtensionState::enabled);
    // Unreadable manifests offer no update.
    EXPECT_TRUE(ws.manager.updates().empty());
}

TEST(ExtensionManagerIndex, ReloadingADisabledModuleKeepsItDisabled) {
    Workspace ws;
    const ModuleFolders folders;
    const fs::path folder = folders.write("dev", module("test.dev", "1.0.0"));
    const ExtensionInfo& info = ws.manager.install(folder, ExtensionSource::local);
    folders.write("dev", module("test.dev", "1.0.1", command("dev.one")));
    const ReloadReport report = ws.manager.reload("test.dev");
    EXPECT_TRUE(report.enabled.empty());
    EXPECT_EQ(info.state, ExtensionState::disabled);
    EXPECT_EQ(info.manifest->version.to_string(), "1.0.1");
    EXPECT_EQ(ws.manager.command_index().size(), 1U);

    // A manifest for another platform makes the module incompatible.
    folders.write("dev",
                  "[extension]\nid = \"test.dev\"\nname = \"D\"\nversion = \"9.0.0\"\n"
                  "engine = \"^99.0\"\n");
    (void)ws.manager.reload("test.dev");
    EXPECT_FALSE(info.compatible());
    EXPECT_EXTENSION_ERROR((void)ws.manager.enable("test.dev"), ExtensionErrc::incompatible);
}

}  // namespace
