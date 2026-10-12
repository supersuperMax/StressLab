#pragma once

#include "fem/core/context/error.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace fem::context::testing {

// Text as UTF-8 bytes. GoogleTest from vcpkg is built as C++17 and cannot print char8_t strings.
inline std::string utf8(std::u8string_view text) {
    return {text.begin(), text.end()};
}

}  // namespace fem::context::testing

/// Checks that `statement` throws ContextError with the given code.
#define EXPECT_CONTEXT_ERROR(statement, expected_code)                                             \
    do {                                                                                           \
        try {                                                                                      \
            statement;                                                                             \
            ADD_FAILURE() << "Expected ContextError " << ::fem::context::to_string(expected_code); \
        } catch (const ::fem::context::ContextError& error) {                                      \
            EXPECT_EQ(::fem::context::to_string(error.code()),                                     \
                      ::fem::context::to_string(expected_code))                                    \
                << error.what();                                                                   \
        }                                                                                          \
    } while (false)
