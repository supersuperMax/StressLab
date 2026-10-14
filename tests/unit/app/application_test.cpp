// What the user changed in the module manager comes back at the next start (task 1.14).

#include "application.h"

#include "fem/core/extension/manager.h"
#include "fem/core/extension/manifest.h"
#include "fem/core/extension/user_modules.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;
using fem::app::Application;
using fem::app::Environment;
using fem::extension::ExtensionSource;

namespace {

/// A module without code for any version of the platform.
void write_module(const fs::path& folder, std::string_view id) {
    fs::create_directories(folder);
    std::ofstream(folder / std::string(fem::extension::manifest_file_name), std::ios::binary)
        << "[extension]\nid = \"" << id << "\"\nname = \"" << id
        << "\"\nversion = \"1.0.0\"\nengine = \">=0.0.0\"\n";
}

TEST(ApplicationModules, TheStateOfTheUserComesBack) {
    const fs::path root = fs::temp_directory_path() / fs::path(u8"приложение StressLab модули");
    std::error_code ignored;
    fs::remove_all(root, ignored);
    write_module(root / "builtin" / "a", "test.a");
    write_module(root / "builtin" / "b", "test.b");
    write_module(root / fs::path(u8"мои модули") / "c", "test.c");

    {
        Application application(Environment{.builtin_extensions = root / "builtin"}, "test");
        application.install_modules();
        fem::extension::UserModules state;
        state.disabled = {"test.b"};
        state.local = {root / fs::path(u8"мои модули") / "c", root / "missing"};
        application.enable_user_modules(state);

        const fem::extension::ExtensionManager& manager = application.extensions();
        EXPECT_TRUE(manager.get("test.a").enabled());
        EXPECT_FALSE(manager.get("test.b").enabled());
        ASSERT_NE(manager.find("test.c"), nullptr);
        EXPECT_EQ(manager.get("test.c").source, ExtensionSource::local);
        EXPECT_TRUE(manager.get("test.c").enabled());
        EXPECT_EQ(manager.extensions().size(), 3U);

        // What is saved is what was loaded, but the folder that is gone.
        const fem::extension::UserModules saved = fem::extension::user_modules(manager);
        EXPECT_EQ(saved.disabled, std::vector<std::string>{"test.b"});
        EXPECT_EQ(saved.local, std::vector<fs::path>{
                                   (root / fs::path(u8"мои модули") / "c").lexically_normal()});
    }
    fs::remove_all(root, ignored);
}

}  // namespace
