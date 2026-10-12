#include "files.h"

#include "fem/core/extension/error.h"

#include <fstream>
#include <iterator>

namespace fem::extension::detail {

std::string read_file(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw ExtensionError(ExtensionErrc::io, "cannot open '" + display(file) + "'");
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        throw ExtensionError(ExtensionErrc::io, "cannot read '" + display(file) + "'");
    }
    return bytes;
}

void write_file(const std::filesystem::path& file, std::string_view bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (out) {
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
    }
    if (!out) {
        throw ExtensionError(ExtensionErrc::io, "cannot write '" + display(file) + "'");
    }
}

std::string display(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

}  // namespace fem::extension::detail
