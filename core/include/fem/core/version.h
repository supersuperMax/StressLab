#pragma once

#include <string_view>

namespace fem {

/// Version of the platform core in "major.minor.patch" form.
[[nodiscard]] std::string_view version() noexcept;

}  // namespace fem
