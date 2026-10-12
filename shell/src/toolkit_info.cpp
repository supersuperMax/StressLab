#include "fem/shell/toolkit_info.h"

#include <DockManager.h>

#include <QtGlobal>

namespace fem::shell {

std::string qt_runtime_version() {
    return qVersion();
}

}  // namespace fem::shell
