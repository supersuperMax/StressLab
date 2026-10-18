#include "fem/core/command/problem.h"

namespace fem::command {

std::string_view to_string(Problem::Severity severity) noexcept {
    switch (severity) {
        case Problem::Severity::info:
            return "info";
        case Problem::Severity::warning:
            return "warning";
        case Problem::Severity::error:
            return "error";
    }
    return "warning";
}

std::string problem_text(const Problem& problem, const l10n::Localization& localization) {
    std::string text = localization.manifest_text(problem.owner, problem.message, problem.args);
    if (problem.file.empty()) {
        return text;
    }
    std::string place = problem.file;
    if (problem.line > 0) {
        place += ":" + std::to_string(problem.line);
    }
    return place + ": " + text;
}

}  // namespace fem::command
