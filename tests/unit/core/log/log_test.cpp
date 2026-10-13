// Logging to rotating files in a folder with Cyrillic letters and spaces (task 1.7).

#include "fem/core/log/log.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using fem::log::Level;
using fem::log::Logging;
using fem::log::LogOptions;

std::string read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string utf8(std::u8string_view text) {
    return {text.begin(), text.end()};
}

std::size_t count(const std::string& text, const std::string& what) {
    std::size_t n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) {
        ++n;
    }
    return n;
}

class LogTest : public ::testing::Test {
protected:
    LogTest() : folder(fs::temp_directory_path() / fs::path(u8"stresslab журнал тест") / "logs") {
        fs::remove_all(folder.parent_path());
    }
    ~LogTest() override {
        std::error_code ignored;
        fs::remove_all(folder.parent_path(), ignored);
    }
    LogTest(const LogTest&) = delete;
    LogTest& operator=(const LogTest&) = delete;
    LogTest(LogTest&&) = delete;
    LogTest& operator=(LogTest&&) = delete;

    [[nodiscard]] fs::path file(int index = 0) const {
        return folder / (index == 0 ? std::string("stresslab.log")
                                    : "stresslab." + std::to_string(index) + ".log");
    }

    fs::path folder;
};

TEST_F(LogTest, WritesCategoriesAndLevelsToAFileInTheFolder) {
    {
        Logging logging({.folder = folder});
        EXPECT_EQ(logging.file(), file());
        fem::log::logger("settings")->info("загружено {} настроек", 3);
        fem::log::logger("l10n")->warn("no catalog for '{}'", "de");
        fem::log::logger("settings")->debug("not written at info");
    }
    const std::string text = read(file());
    EXPECT_NE(text.find("[info] [settings] ["), std::string::npos) << text;
    EXPECT_NE(text.find("загружено 3 настроек\n"), std::string::npos) << text;
    EXPECT_NE(text.find("[warning] [l10n] ["), std::string::npos) << text;
    EXPECT_EQ(text.find("not written"), std::string::npos) << text;
    // "2026-10-12 14:03:51.207 [info] ..."
    ASSERT_GT(text.size(), 24U);
    EXPECT_EQ(text[4], '-');
    EXPECT_EQ(text[10], ' ');
    EXPECT_EQ(text[19], '.');
}

TEST_F(LogTest, NothingIsWrittenWithoutLogging) {
    fem::log::logger("quiet")->error("nobody listens");
    { Logging logging({.folder = folder}); }
    fem::log::logger("quiet")->error("after the file was closed");
    EXPECT_EQ(read(file()), "");
}

TEST_F(LogTest, WithoutAFolderThereIsNoFile) {
    {
        Logging logging({.level = Level::debug});
        EXPECT_TRUE(logging.file().empty());
        fem::log::logger("test")->info("goes nowhere");
        logging.flush();
    }
    EXPECT_FALSE(fs::exists(folder.parent_path()));
}

TEST_F(LogTest, WritesToTheStandardOutput) {
    ::testing::internal::CaptureStdout();
    {
        Logging logging({.level = Level::info, .console = fem::log::Console::standard_output});
        fem::log::logger("test")->debug("too detailed");
        fem::log::logger("test")->warn(utf8(u8"в консоль"));
    }
    fem::log::logger("test")->warn("after the console was closed");
    const std::string text = ::testing::internal::GetCapturedStdout();
    // Date and time, the level and the category, without the thread; '\n' on every OS.
    const std::string line = " [warning] [test] " + utf8(u8"в консоль") + "\n";
    ASSERT_GT(text.size(), line.size());
    EXPECT_EQ(text.substr(23), line) << text;
    EXPECT_EQ(text[4], '-');
}

TEST_F(LogTest, TheLevelCanChange) {
    {
        Logging logging({.folder = folder, .level = Level::warn});
        EXPECT_EQ(logging.level(), Level::warn);
        fem::log::logger("test")->info("first info");
        logging.set_level(Level::debug);
        fem::log::logger("test")->debug("first debug");
        logging.set_level(Level::off);
        fem::log::logger("test")->critical("first critical");
    }
    const std::string text = read(file());
    EXPECT_EQ(text.find("first info"), std::string::npos);
    EXPECT_NE(text.find("first debug"), std::string::npos);
    EXPECT_EQ(text.find("first critical"), std::string::npos);
}

TEST_F(LogTest, RotatesBySizeKeepingAFixedNumberOfFiles) {
    {
        Logging logging(
            {.folder = folder, .max_file_size = 1000, .max_files = 3, .rotate_on_open = false});
        for (int i = 0; i < 100; ++i) {
            fem::log::logger("rotation")->info("message {:03} {}", i, std::string(40, 'x'));
        }
    }
    EXPECT_TRUE(fs::exists(file(0)));
    EXPECT_TRUE(fs::exists(file(1)));
    EXPECT_TRUE(fs::exists(file(2)));
    EXPECT_FALSE(fs::exists(file(3)));
    for (int index = 0; index < 3; ++index) {
        EXPECT_LE(fs::file_size(file(index)), 1000U) << index;
        EXPECT_GT(fs::file_size(file(index)), 0U) << index;
    }
    // The newest messages are in the current file, older ones in .1, then .2.
    EXPECT_NE(read(file(0)).find("message 099"), std::string::npos);
    const std::string previous = read(file(1));
    const std::string oldest = read(file(2));
    EXPECT_EQ(previous.find("message 099"), std::string::npos);
    EXPECT_LT(oldest.substr(oldest.find("message ") + 8, 3),
              previous.substr(previous.find("message ") + 8, 3));
    EXPECT_EQ(read(file(0)).find("message 000"), std::string::npos) << "the oldest are dropped";
}

TEST_F(LogTest, EachRunStartsANewFile) {
    for (const char* run : {"first run", "second run", "third run"}) {
        Logging logging({.folder = folder, .max_files = 2});
        fem::log::logger("runs")->info(run);
    }
    EXPECT_NE(read(file(0)).find("third run"), std::string::npos);
    EXPECT_EQ(read(file(0)).find("second run"), std::string::npos);
    EXPECT_NE(read(file(1)).find("second run"), std::string::npos);
    EXPECT_FALSE(fs::exists(file(2)));
}

TEST_F(LogTest, KeepsAppendingWithoutRotationOnOpen) {
    for (const char* run : {"first run", "second run"}) {
        Logging logging({.folder = folder, .rotate_on_open = false});
        fem::log::logger("runs")->info(run);
    }
    const std::string text = read(file());
    EXPECT_NE(text.find("first run"), std::string::npos);
    EXPECT_NE(text.find("second run"), std::string::npos);
    EXPECT_FALSE(fs::exists(file(1)));
}

TEST_F(LogTest, ThreadsCanLogAtOnce) {
    {
        Logging logging({.folder = folder, .max_file_size = 1 << 20});
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([t] {
                for (int i = 0; i < 50; ++i) {
                    fem::log::logger("thread." + std::to_string(t))->info("line {}", i);
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
    }
    const std::string text = read(file());
    EXPECT_EQ(count(text, "\n"), 200U);
    EXPECT_EQ(count(text, "[thread.2]"), 50U);
}

TEST(LogLevels, HaveTheNamesOfSpdlog) {
    for (const Level level : {Level::trace, Level::debug, Level::info, Level::warn, Level::err,
                              Level::critical, Level::off}) {
        EXPECT_EQ(fem::log::parse_level(fem::log::to_string(level)), level);
    }
    EXPECT_EQ(fem::log::to_string(Level::warn), "warning");
    EXPECT_EQ(fem::log::parse_level("loud"), std::nullopt);
}

}  // namespace
