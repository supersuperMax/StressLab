#include "fem/shell/toolkit_info.h"

#include <gtest/gtest.h>

TEST(ShellToolkit, LinksAgainstQt6) {
    EXPECT_TRUE(fem::shell::qt_runtime_version().starts_with("6."));
}
