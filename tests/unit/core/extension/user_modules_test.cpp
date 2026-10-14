// What the user keeps between sessions in the module manager (task 1.14): the state of the
// modules and the folder of profiles.

#include "fem/core/extension/user_modules.h"

#include "fem/core/extension/error.h"
#include "fem/core/extension/manager.h"
#include "fem/core/extension/profile.h"
#include "fem/core/extension/semver.h"

#include "extension_test_support.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using fem::extension::ExtensionErrc;
using fem::extension::ExtensionSource;
using fem::extension::load_user_modules;
using fem::extension::parse_user_modules;
using fem::extension::Profile;
using fem::extension::ProfileLibrary;
using fem::extension::save_user_modules;
using fem::extension::to_json;
using fem::extension::user_modules;
using fem::extension::UserModules;
using fem::extension::Version;
using fem::extension::testing::fixtures;
using fem::extension::testing::utf8;
using fem::extension::testing::Workspace;
using Ids = std::vector<std::string>;

class TempFolder {
public:
    explicit TempFolder(std::u8string_view name)
        : path_(fs::temp_directory_path() / fs::path(name)) {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    ~TempFolder() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

std::string read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// An absolute folder with Cyrillic and spaces on every OS.
fs::path absolute_folder(std::u8string_view name) {
    return (fs::temp_directory_path() / fs::path(name)).lexically_normal();
}

UserModules state() {
    UserModules modules;
    modules.disabled = {"test.solver", "test.base", "test.base"};
    modules.local = {absolute_folder(u8"Мои модули/fem.second"),
                     absolute_folder(u8"Мои модули/fem.first")};
    return modules;
}

TEST(UserModules, IsWrittenDeterministically) {
    const std::string text = to_json(state());
    EXPECT_EQ(text,
              "{\n"
              "  \"disabled\": [\n"
              "    \"test.base\",\n"
              "    \"test.solver\"\n"
              "  ],\n"
              "  \"format\": \"stresslab.modules\",\n"
              "  \"formatVersion\": 1,\n"
              "  \"local\": [\n"
              "    \"" +
                  utf8(absolute_folder(u8"Мои модули/fem.first").generic_u8string()) +
                  "\",\n"
                  "    \"" +
                  utf8(absolute_folder(u8"Мои модули/fem.second").generic_u8string()) +
                  "\"\n"
                  "  ]\n"
                  "}\n");
    // Round trip: read and written again, the same bytes.
    const UserModules parsed = parse_user_modules(text);
    EXPECT_EQ(parsed.disabled, (Ids{"test.base", "test.solver"}));
    ASSERT_EQ(parsed.local.size(), 2U);
    EXPECT_EQ(parsed.local[0], absolute_folder(u8"Мои модули/fem.first"));
    EXPECT_EQ(to_json(parsed), text);
    EXPECT_EQ(to_json(UserModules{}),
              "{\n  \"disabled\": [],\n  \"format\": \"stresslab.modules\",\n"
              "  \"formatVersion\": 1,\n  \"local\": []\n}\n");
}

TEST(UserModules, IsSavedToAPathWithCyrillicLettersAndSpaces) {
    const TempFolder folder(u8"настройки StressLab с пробелами");
    // The folder does not exist yet.
    const fs::path file = folder.path() / "вложенная папка" / "modules.json";
    save_user_modules(file, state());
    EXPECT_EQ(read(file), to_json(state()));
    UserModules expected = state();
    expected.disabled = {"test.base", "test.solver"};
    std::swap(expected.local[0], expected.local[1]);
    EXPECT_EQ(load_user_modules(file), expected);
    EXPECT_EXTENSION_ERROR((void)load_user_modules(folder.path() / "missing.json"),
                           ExtensionErrc::io);
}

TEST(UserModules, RejectsWhatTheCoreDoesNotWrite) {
    const std::string valid = to_json(state());
    const auto changed = [&](std::string_view from, std::string_view to) {
        std::string text = valid;
        text.replace(text.find(from), from.size(), to);
        return text;
    };
    for (const std::string& text :
         {std::string("not json"), std::string("[]"),
          changed("\"stresslab.modules\"", "\"stresslab.profile\""),
          changed("\"formatVersion\": 1", "\"formatVersion\": 2"),
          changed("\"test.base\"", "\"Test\""), changed("\"test.base\"", "5"),
          changed("\"disabled\": [", "\"disabled\": {\"a\": ["),
          changed("\"local\": [", "\"local\": [\"relative/folder\", "),
          changed("\"format\"", "\"extra\": 1, \"format\"")}) {
        SCOPED_TRACE(text);
        EXPECT_EXTENSION_ERROR((void)parse_user_modules(text), ExtensionErrc::invalid_profile);
    }
    UserModules relative;
    relative.local = {fs::path("relative")};
    EXPECT_EXTENSION_ERROR((void)to_json(relative), ExtensionErrc::invalid_profile);
}

TEST(UserModules, StateOfTheManager) {
    Workspace ws;
    (void)ws.manager.scan(fixtures() / "installed", ExtensionSource::builtin);
    (void)ws.manager.install(fixtures() / "full", ExtensionSource::local);
    ws.manager.enable("test.solver");
    const UserModules modules = user_modules(ws.manager);
    // Not test.future: this platform cannot run it, so it was not the user who disabled it.
    EXPECT_EQ(modules.disabled, (Ids{"test.full", "test.l10n", "test.post"}));
    EXPECT_EQ(modules.local, std::vector<fs::path>{(fixtures() / "full").lexically_normal()});
}

// --- ProfileLibrary ------------------------------------------------------------------------------

Profile profile(std::u8string_view name, std::string_view version = "1.0.0") {
    Profile result;
    result.name = utf8(name);
    result.extensions = {{"test.base", Version::parse(version)}};
    return result;
}

TEST(ProfileLibrary, KeepsProfilesInFilesNamedAfterThem) {
    const TempFolder folder(u8"профили StressLab");
    ProfileLibrary library(folder.path() / "profiles");
    EXPECT_TRUE(library.names().empty());
    EXPECT_FALSE(library.find("x").has_value());
    EXPECT_FALSE(library.remove("x"));

    const fs::path file = library.save(profile(u8"Прочность стали"));
    EXPECT_EQ(file, folder.path() / "profiles" / fs::path(u8"Прочность стали.json"));
    EXPECT_EQ(read(file), to_json(profile(u8"Прочность стали")));
    library.save(profile(u8"Тепло: «быстро»/черновик"));
    EXPECT_TRUE(
        fs::exists(folder.path() / "profiles" / fs::path(u8"Тепло_ «быстро»_черновик.json")));
    // A file that is not a profile is skipped.
    std::ofstream(folder.path() / "profiles" / "notes.json", std::ios::binary).write("{}", 2);
    EXPECT_EQ(library.names(),
              (Ids{utf8(u8"Прочность стали"), utf8(u8"Тепло: «быстро»/черновик")}));

    // Saving a profile of the same name replaces it in its file.
    EXPECT_EQ(library.save(profile(u8"Прочность стали", "2.0.0")), file);
    EXPECT_EQ(library.find(utf8(u8"Прочность стали"))->extensions[0].version.to_string(), "2.0.0");
    EXPECT_EQ(library.file_of(utf8(u8"Прочность стали")), file);

    // Two names for one file name: the second gets a number.
    library.save(profile(u8"Тепло: «быстро»/черновик"));
    const fs::path other = library.save(profile(u8"Тепло: «быстро»|черновик"));
    EXPECT_EQ(other.filename(), fs::path(u8"Тепло_ «быстро»_черновик (2).json"));
    EXPECT_EQ(library.names().size(), 3U);

    EXPECT_TRUE(library.remove(utf8(u8"Прочность стали")));
    EXPECT_FALSE(fs::exists(file));
    EXPECT_FALSE(library.find(utf8(u8"Прочность стали")).has_value());
}

TEST(ProfileLibrary, FileNamesAreValidOnEveryOs) {
    EXPECT_EQ(ProfileLibrary::file_name("Minimal"), "Minimal.json");
    EXPECT_EQ(ProfileLibrary::file_name(utf8(u8"Сталь 20")), utf8(u8"Сталь 20.json"));
    EXPECT_EQ(ProfileLibrary::file_name("a<b>c:d\"e/f\\g|h?i*j\tk"), "a_b_c_d_e_f_g_h_i_j_k.json");
    EXPECT_EQ(ProfileLibrary::file_name("  end. . "), "end.json");
    EXPECT_EQ(ProfileLibrary::file_name(""), "profile.json");
    EXPECT_EQ(ProfileLibrary::file_name("..."), "profile.json");
    EXPECT_EQ(ProfileLibrary::file_name("con"), "_con.json");
    EXPECT_EQ(ProfileLibrary::file_name("Nul.old"), "_Nul.old.json");
    EXPECT_EQ(ProfileLibrary::file_name("COM1"), "_COM1.json");
    EXPECT_EQ(ProfileLibrary::file_name("COM10"), "COM10.json");
    EXPECT_EQ(ProfileLibrary::file_name("Console"), "Console.json");
}

}  // namespace
