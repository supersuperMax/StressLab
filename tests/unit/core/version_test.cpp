#include "fem/core/version.h"

#include <gtest/gtest.h>

TEST(CoreVersion, MatchesProjectVersion) {
    EXPECT_EQ(fem::version(), FEM_EXPECTED_VERSION);
}
