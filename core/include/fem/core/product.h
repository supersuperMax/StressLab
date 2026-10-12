#pragma once

#include <string_view>

namespace fem {

/// Product name for user-facing text: window titles, the About dialog, logs, file metadata.
/// C++ code takes the name from here instead of spelling it out; CMake uses PROJECT_NAME, and
/// a unit test keeps the two equal (CLAUDE.md, "Name and brand").
inline constexpr std::string_view kProductName = "StressLab";

}  // namespace fem
