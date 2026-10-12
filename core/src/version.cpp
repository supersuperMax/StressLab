#include "fem/core/version.h"

namespace fem {

std::string_view version() noexcept {
    return FEM_VERSION_STRING;
}

}  // namespace fem
