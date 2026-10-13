#pragma once

#include <filesystem>

namespace fem::app {

/// The executable of this process, absolute. Throws std::runtime_error if the OS does not say.
[[nodiscard]] std::filesystem::path executable_path();

}  // namespace fem::app
