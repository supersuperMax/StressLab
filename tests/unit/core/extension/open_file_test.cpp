// Finding the module that reads a model file (task 3.2). Running its command is tested with the
// real module, fem.format.abaqus-inp (tests/unit/extensions/abaqus_inp/module_test.cpp).

#include "fem/core/extension/open_file.h"

#include "fem/core/extension/error.h"

#include "extension_test_support.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace {

using fem::extension::ExtensionErrc;
using fem::extension::ExtensionError;
using fem::extension::find_import_format;
using fem::extension::testing::utf8;
using fem::extension::testing::Workspace;

/// A module "test.<name>" with the command "<name>.read" and the format "<name>" (format ids are
/// global, like command ids).
std::string reader(std::string_view id, std::string_view extension, std::string_view role) {
    const std::string name(id.substr(id.find('.') + 1));
    return "[extension]\nid = \"" + std::string(id) +
           "\"\nname = \"R\"\nversion = \"1.0.0\"\nengine = \"^1.0\"\nkind = \"native\"\n"
           "entry = \"reader\"\n\n[[contributes.commands]]\nid = \"" +
           name +
           ".read\"\ntitle = \"Read\"\nargs = [{ name = \"path\", type = \"path\" }]\n\n"
           "[[contributes.fileFormats]]\nid = \"" +
           name + "\"\nextensions = [\"" + std::string(extension) + "\"]\nrole = \"" +
           std::string(role) + "\"\ncommand = \"" + name + ".read\"\n";
}

TEST(OpenFile, FindsTheImportFormatOfAnEnabledModuleByExtension) {
    Workspace ws;
    ws.install(reader("test.mesh", ".msh", "mesh-import"));
    ws.install(reader("test.model", ".res", "model-import"));
    EXPECT_EQ(find_import_format(ws.manager.registry(), "a.res"), nullptr);  // Not enabled.
    ws.manager.enable("test.mesh");
    ws.manager.enable("test.model");

    const auto* format =
        find_import_format(ws.manager.registry(), std::filesystem::path(u8"папка/Модель.RES"));
    ASSERT_NE(format, nullptr);
    EXPECT_EQ(format->owner, "test.model");
    EXPECT_EQ(format->command, "model.read");
    // A mesh format does not open a model; a file without an extension matches nothing.
    EXPECT_EQ(find_import_format(ws.manager.registry(), "a.msh"), nullptr);
    EXPECT_EQ(find_import_format(ws.manager.registry(), "res"), nullptr);
}

TEST(OpenFile, FileNoModuleReadsIsRefused) {
    Workspace ws;
    try {
        (void)fem::extension::open_model_file(ws.manager, ws.bus,
                                              std::filesystem::path(u8"деталь.step"));
        FAIL();
    } catch (const ExtensionError& error) {
        EXPECT_EQ(error.code(), ExtensionErrc::unsupported_file);
        EXPECT_NE(std::string(error.what()).find(utf8(u8"деталь.step")), std::string::npos)
            << error.what();
    }
    EXPECT_FALSE(ws.bus.can_undo());
}

}  // namespace
