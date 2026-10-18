#pragma once

// Problems of an import: warnings for the "Problems" tab and the reason a file cannot be read.
// The text is a key of the module's catalogs (l10n/*.json, "problem.*" and "error.*") with
// arguments; the platform translates it (docs/adr/0024-abaqus-inp-import.md).

#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace fem::inp {

struct Problem {
    enum class Severity : std::uint8_t { info, warning, error };

    Severity severity = Severity::warning;
    std::string key{};                               ///< "problem.passthrough", without '%'.
    nlohmann::json args = nlohmann::json::object();  ///< Text or numbers by placeholder.
    std::string file{};     ///< As the user knows it: relative to the main file, '/'.
    std::int64_t line = 0;  ///< 1-based; 0 if the problem is not at a line.
};

[[nodiscard]] const char* to_string(Problem::Severity severity) noexcept;

/// A file the importer cannot read: nothing is imported. `problem()` says why and where.
class InpError : public std::runtime_error {
public:
    explicit InpError(Problem problem);

    [[nodiscard]] const Problem& problem() const noexcept { return problem_; }

private:
    Problem problem_;
};

}  // namespace fem::inp
