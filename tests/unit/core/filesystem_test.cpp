// Guards the toolchain setup: UTF-8 source encoding and Unicode paths must work on every OS
// (CLAUDE.md, "Cross-platform"). Later tasks rely on this for project folders and job workdirs.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

// GoogleTest from vcpkg is compiled as C++17 and has no printers for char8_t strings,
// so UTF-8 text is compared as plain bytes.
std::string bytes(std::u8string_view text) {
    return {text.begin(), text.end()};
}

class TempDir {
public:
    explicit TempDir(const fs::path& name) : path_(fs::temp_directory_path() / name) {
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

}  // namespace

TEST(Filesystem, ReadsAndWritesFilesWithCyrillicAndSpacesInPath) {
    constexpr std::u8string_view file_name = u8"модель 1.txt";
    const TempDir dir(fs::path(u8"тестовый каталог с пробелами"));
    const fs::path file = dir.path() / fs::path(file_name);

    {
        std::ofstream out(file, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out << "ok\n";
    }

    ASSERT_TRUE(fs::exists(file));
    EXPECT_EQ(bytes(file.filename().u8string()), bytes(file_name));

    std::ifstream in(file, std::ios::binary);
    std::string content;
    std::getline(in, content);
    EXPECT_EQ(content, "ok");
}
