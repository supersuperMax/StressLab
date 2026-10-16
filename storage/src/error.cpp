#include "fem/storage/error.h"

#include <utility>

namespace fem::storage {

namespace {

std::string full_message(const std::string& message, const std::string& file, int line) {
    if (file.empty()) {
        return message;
    }
    return file + (line > 0 ? ":" + std::to_string(line) : std::string()) + ": " + message;
}

}  // namespace

std::string_view to_string(ProjectErrc code) noexcept {
    switch (code) {
        case ProjectErrc::io:
            return "io";
        case ProjectErrc::not_a_project:
            return "not_a_project";
        case ProjectErrc::unsupported_format:
            return "unsupported_format";
        case ProjectErrc::invalid_file:
            return "invalid_file";
        case ProjectErrc::invalid_model:
            return "invalid_model";
        case ProjectErrc::missing_blob:
            return "missing_blob";
        case ProjectErrc::corrupt_blob:
            return "corrupt_blob";
        case ProjectErrc::invalid_data:
            return "invalid_data";
        case ProjectErrc::no_folder:
            return "no_folder";
        case ProjectErrc::unknown_version:
            return "unknown_version";
        case ProjectErrc::already_exists:
            return "already_exists";
        case ProjectErrc::invalid_name:
            return "invalid_name";
        case ProjectErrc::uncommitted_changes:
            return "uncommitted_changes";
        case ProjectErrc::nothing_to_commit:
            return "nothing_to_commit";
        case ProjectErrc::nothing_to_stash:
            return "nothing_to_stash";
        case ProjectErrc::detached:
            return "detached";
        case ProjectErrc::stash_conflict:
            return "stash_conflict";
    }
    return "unknown";
}

ProjectError::ProjectError(ProjectErrc code, const std::string& message, std::string file, int line)
    : std::runtime_error(full_message(message, file, line)),
      code_(code),
      file_(std::move(file)),
      line_(line) {}

}  // namespace fem::storage
