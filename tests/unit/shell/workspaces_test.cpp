#include "fem/shell/workspaces.h"

#include <gtest/gtest.h>

#include <QFile>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;
using fem::shell::Layout;
using fem::shell::WorkspaceStore;

Layout sample(const char* name) {
    return {.docks = QByteArray("<QtAdvancedDockingSystem Version=\"3\" name=\"") + name + "\"/>",
            .bars = QByteArray("\x00\x01\xff bars of ", 12) + name};
}

WorkspaceStore filled() {
    WorkspaceStore store;
    store.set(QStringLiteral("Препроцессинг"), sample("pre"));
    store.set(QStringLiteral("Two views"), sample("two"));
    store.set_current(QStringLiteral("Препроцессинг"));
    store.set_session(sample("session"), QByteArray("\x01\x02\x03 geometry", 12));
    return store;
}

QByteArray read(const fs::path& file) {
    QFile input(QString::fromStdU16String(file.generic_u16string()));
    EXPECT_TRUE(input.open(QIODevice::ReadOnly));
    return input.readAll();
}

class WorkspaceFile : public ::testing::Test {
protected:
    void SetUp() override {
        fs::remove_all(folder_);
        fs::create_directories(folder_);
    }
    void TearDown() override { fs::remove_all(folder_); }

    // Cyrillic letters and spaces, as in the profiles of users.
    const fs::path folder_ =
        fs::temp_directory_path() / fs::path(u8"stresslab рабочие пространства");
    const fs::path file_ = folder_ / fs::path(u8"настройки пользователя") / "workspaces.json";
};

TEST(Workspaces, KeepNamedLayouts) {
    WorkspaceStore store = filled();
    EXPECT_EQ(store.names(),
              QStringList({QStringLiteral("Two views"), QStringLiteral("Препроцессинг")}));
    EXPECT_EQ(store.find(QStringLiteral("Two views")), sample("two"));
    EXPECT_FALSE(store.find(QStringLiteral("Нет такого")));
    store.set(QStringLiteral("Two views"), sample("other"));
    EXPECT_EQ(store.find(QStringLiteral("Two views")), sample("other"));
    EXPECT_FALSE(store.remove(QStringLiteral("Нет такого")));
    // Removing the current workspace goes back to the default layout.
    EXPECT_TRUE(store.remove(QStringLiteral("Препроцессинг")));
    EXPECT_TRUE(store.current().isEmpty());
    EXPECT_EQ(store.names(), QStringList({QStringLiteral("Two views")}));
}

TEST(Workspaces, SavingWhatWasLoadedGivesTheSameBytes) {
    const QByteArray json = filled().to_json();
    const WorkspaceStore loaded = WorkspaceStore::from_json(json);
    EXPECT_EQ(loaded.to_json(), json);
    EXPECT_EQ(loaded.find(QStringLiteral("Препроцессинг")), sample("pre"));
    EXPECT_EQ(loaded.current(), QStringLiteral("Препроцессинг"));
    ASSERT_TRUE(loaded.session());
    EXPECT_EQ(*loaded.session(), sample("session"));
    EXPECT_EQ(loaded.geometry(), QByteArray("\x01\x02\x03 geometry", 12));
    // '\n' line ends on every OS; keys sorted.
    EXPECT_FALSE(json.contains('\r'));
    EXPECT_LT(json.indexOf("\"current\""), json.indexOf("\"format\""));
    EXPECT_LT(json.indexOf("\"format\""), json.indexOf("\"session\""));
    EXPECT_LT(json.indexOf("\"session\""), json.indexOf("\"workspaces\""));
    // An empty store too.
    const QByteArray empty = WorkspaceStore().to_json();
    EXPECT_EQ(WorkspaceStore::from_json(empty).to_json(), empty);
    EXPECT_FALSE(WorkspaceStore::from_json(empty).session());
}

TEST(Workspaces, RejectWhatIsNotAStoreOfThisFormat) {
    EXPECT_THROW((void)WorkspaceStore::from_json("not json"), std::runtime_error);
    EXPECT_THROW((void)WorkspaceStore::from_json(R"({"format": "stresslab.settings"})"),
                 std::runtime_error);
    EXPECT_THROW((void)WorkspaceStore::from_json(
                     R"({"format": "stresslab.workspaces", "formatVersion": 2})"),
                 std::runtime_error);
    EXPECT_THROW((void)WorkspaceStore::from_json(
                     R"({"format": "stresslab.workspaces", "formatVersion": 1,
                         "workspaces": {"a": {"docks": 1, "bars": ""}}})"),
                 std::runtime_error);
}

TEST(Workspaces, ACurrentWorkspaceThatIsGoneIsTheDefault) {
    const WorkspaceStore store = WorkspaceStore::from_json(
        R"({"current": "gone", "format": "stresslab.workspaces", "formatVersion": 1})");
    EXPECT_TRUE(store.current().isEmpty());
}

TEST_F(WorkspaceFile, IsSavedAndLoadedInAFolderWithCyrillicLetters) {
    {
        WorkspaceStore store(file_);
        EXPECT_TRUE(store.names().isEmpty());  // No file yet.
        store.set(QStringLiteral("Постобработка"), sample("post"));
        store.set_current(QStringLiteral("Постобработка"));
        ASSERT_TRUE(store.save());
    }
    const QByteArray saved = read(file_);
    WorkspaceStore store(file_);
    EXPECT_EQ(store.find(QStringLiteral("Постобработка")), sample("post"));
    EXPECT_EQ(store.current(), QStringLiteral("Постобработка"));
    ASSERT_TRUE(store.save());
    EXPECT_EQ(read(file_), saved);
}

TEST_F(WorkspaceFile, ABrokenFileGivesAnEmptyStore) {
    fs::create_directories(file_.parent_path());
    std::ofstream(file_) << "{ not json";
    const WorkspaceStore store(file_);
    EXPECT_TRUE(store.names().isEmpty());
    EXPECT_FALSE(store.session());
    EXPECT_EQ(store.file(), file_);
}

TEST(Workspaces, WithoutAFileSavingDoesNothing) {
    EXPECT_TRUE(filled().save());
}

}  // namespace
