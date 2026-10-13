#include "log_slot.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>

namespace {

namespace fs = std::filesystem;
using fem::app::LogSlot;
using fem::app::take_log_slot;

class LogSlots : public ::testing::Test {
protected:
    void SetUp() override { fs::remove_all(folder_); }
    void TearDown() override { fs::remove_all(folder_); }

    const fs::path folder_ = fs::temp_directory_path() / fs::path(u8"stresslab журналы окон");
};

TEST_F(LogSlots, EachWindowGetsAFileOfItsOwn) {
    std::optional<LogSlot> first = take_log_slot(folder_);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->file_name, "stresslab.log");
    EXPECT_TRUE(fs::is_directory(folder_));
    std::optional<LogSlot> second = take_log_slot(folder_);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->file_name, "stresslab-2.log");
    std::optional<LogSlot> third = take_log_slot(folder_);
    ASSERT_TRUE(third);
    EXPECT_EQ(third->file_name, "stresslab-3.log");

    // A closed window frees its slot for the next one.
    first.reset();
    std::optional<LogSlot> next = take_log_slot(folder_);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->file_name, "stresslab.log");
}

TEST_F(LogSlots, NoFileWhenEverySlotIsTaken) {
    std::optional<LogSlot> first = take_log_slot(folder_, 2);
    std::optional<LogSlot> second = take_log_slot(folder_, 2);
    ASSERT_TRUE(first && second);
    EXPECT_FALSE(take_log_slot(folder_, 2));
}

}  // namespace
