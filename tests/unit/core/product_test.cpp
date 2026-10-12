#include "fem/core/product.h"

#include <gtest/gtest.h>

// The product name is spelled out in two places only: project() in the root CMakeLists.txt and
// fem::kProductName. This test keeps them equal (CLAUDE.md, "Name and brand").
TEST(CoreProduct, NameMatchesCMakeProject) {
    EXPECT_EQ(fem::kProductName, FEM_EXPECTED_PRODUCT_NAME);
}
