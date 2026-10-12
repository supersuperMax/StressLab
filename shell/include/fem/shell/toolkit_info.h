#pragma once

#include <string>

namespace fem::shell {

/// Runtime version of the Qt library the shell is linked against.
[[nodiscard]] std::string qt_runtime_version();

}  // namespace fem::shell
