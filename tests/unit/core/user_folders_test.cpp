// Folders of the user's settings and logs by the conventions of each OS (task 1.7).

#include "fem/core/user_folders.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>

namespace {

namespace fs = std::filesystem;
using fem::Platform;
using fem::user_folders;
using fem::UserFolderInputs;

TEST(UserFolders, Windows) {
    const UserFolderInputs inputs{
        .home = fs::path(u8"C:/Users/Дмитрий"),
        .roaming_appdata = fs::path(u8"C:/Users/Дмитрий/AppData/Roaming"),
        .local_appdata = fs::path(u8"C:/Users/Дмитрий/AppData/Local"),
    };
    const fem::UserFolders folders = user_folders(Platform::windows, inputs);
    EXPECT_EQ(folders.config, fs::path(u8"C:/Users/Дмитрий/AppData/Roaming") / "StressLab");
    EXPECT_EQ(folders.logs, fs::path(u8"C:/Users/Дмитрий/AppData/Local") / "StressLab" / "logs");
    EXPECT_THROW((void)user_folders(Platform::windows, {.home = inputs.home}), std::runtime_error);
}

TEST(UserFolders, MacOS) {
    const fem::UserFolders folders = user_folders(Platform::macos, {.home = "/Users/anna"});
    EXPECT_EQ(folders.config, fs::path("/Users/anna/Library/Application Support/StressLab"));
    EXPECT_EQ(folders.logs, fs::path("/Users/anna/Library/Logs/StressLab"));
    EXPECT_THROW((void)user_folders(Platform::macos, {}), std::runtime_error);
}

TEST(UserFolders, PosixWithoutXdgVariables) {
    const fem::UserFolders folders = user_folders(Platform::posix, {.home = "/home/anna"});
    EXPECT_EQ(folders.config, fs::path("/home/anna/.config/stresslab"));
    EXPECT_EQ(folders.logs, fs::path("/home/anna/.local/state/stresslab/logs"));
}

TEST(UserFolders, PosixWithXdgVariables) {
    const fem::UserFolders folders = user_folders(
        Platform::posix,
        {.home = "/home/anna", .xdg_config_home = "/data/config", .xdg_state_home = "/data/state"});
    EXPECT_EQ(folders.config, fs::path("/data/config/stresslab"));
    EXPECT_EQ(folders.logs, fs::path("/data/state/stresslab/logs"));
    // The specification says relative values are ignored; without a home folder there is none.
    const fem::UserFolders relative = user_folders(
        Platform::posix,
        {.home = "/home/anna", .xdg_config_home = "config", .xdg_state_home = "state"});
    EXPECT_EQ(relative.config, fs::path("/home/anna/.config/stresslab"));
    EXPECT_EQ(relative.logs, fs::path("/home/anna/.local/state/stresslab/logs"));
    const fem::UserFolders without_home =
        user_folders(Platform::posix, {.xdg_config_home = "/c", .xdg_state_home = "/s"});
    EXPECT_EQ(without_home.config, fs::path("/c/stresslab"));
    EXPECT_THROW((void)user_folders(Platform::posix, {}), std::runtime_error);
}

TEST(UserFolders, OfThisProcessAreAbsolute) {
    const fem::UserFolders folders = user_folders();
    EXPECT_TRUE(folders.config.is_absolute()) << folders.config;
    EXPECT_TRUE(folders.logs.is_absolute()) << folders.logs;
    EXPECT_NE(folders.config, folders.logs);
}

}  // namespace
