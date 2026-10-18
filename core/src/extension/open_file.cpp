#include "fem/core/extension/open_file.h"

#include "fem/core/extension/error.h"

#include "files.h"

#include <string>

namespace fem::extension {

namespace {

std::string ascii_lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

}  // namespace

const FileFormatContribution* find_import_format(const ContributionRegistry& registry,
                                                 const std::filesystem::path& file) {
    const std::u8string suffix = file.extension().u8string();
    const std::string extension = ascii_lower(std::string(suffix.begin(), suffix.end()));
    if (extension.empty()) {
        return nullptr;
    }
    for (const FileFormatContribution* format : registry.file_formats()) {
        if (format->role != FileFormatContribution::Role::model_import || format->command.empty()) {
            continue;
        }
        for (const std::string& candidate : format->extensions) {
            if (ascii_lower(candidate) == extension) {
                return format;
            }
        }
    }
    return nullptr;
}

command::CommandResult open_model_file(ExtensionManager& manager, command::CommandBus& bus,
                                       const std::filesystem::path& file) {
    const FileFormatContribution* format = find_import_format(manager.registry(), file);
    if (format == nullptr) {
        throw ExtensionError(ExtensionErrc::unsupported_file,
                             "no enabled module reads '" + detail::display(file) + "'");
    }
    // The command may go away with a module disabled while activating: keep what is needed.
    const std::string command = format->command;
    (void)manager.activate_on(ActivationEvent::open_file(file));
    return bus.execute(command, {{"path", command::Value(file)}});
}

}  // namespace fem::extension
