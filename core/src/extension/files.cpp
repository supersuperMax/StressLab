#include "files.h"

#include "fem/core/extension/error.h"

#include "../io/files.h"

#include <utility>

namespace fem::extension::detail {

std::string read_file(const std::filesystem::path& file) {
    std::optional<std::string> bytes = io::read_file(file);
    if (!bytes) {
        throw ExtensionError(ExtensionErrc::io, "cannot read '" + display(file) + "'");
    }
    return std::move(*bytes);
}

void write_file(const std::filesystem::path& file, std::string_view bytes) {
    if (!io::write_file(file, bytes)) {
        throw ExtensionError(ExtensionErrc::io, "cannot write '" + display(file) + "'");
    }
}

std::string display(const std::filesystem::path& path) {
    return io::display(path);
}

}  // namespace fem::extension::detail
