#pragma once

#include "fem/core/l10n/localization.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace fem::command {

/// Something a command tells the user about besides its result: a warning of an import ("*CONTACT
/// CONTROLS kept without interpretation"), a fact worth knowing or the reason it fails. The shell
/// lists problems in the "Problems" tab, the headless mode prints them (CommandBus::on_problem).
struct Problem {
    enum class Severity : std::uint8_t { info, warning, error };

    Severity severity = Severity::warning;
    /// Module whose catalogs translate `message`; the bus sets it to the owner of the command.
    std::string owner{};
    /// Text as in a manifest: "%key%" is a string of the owner's catalogs, with `args` put into
    /// its placeholders; other text is shown as it is.
    std::string message{};
    l10n::Args args{};
    /// Where in a file it is, if anywhere: the file as the user knows it (UTF-8) and its 1-based
    /// line, 0 if none.
    std::string file{};
    std::int64_t line = 0;
    /// Id of the command that reported it; the bus sets it.
    std::string command{};
};

/// "info", "warning", "error".
[[nodiscard]] std::string_view to_string(Problem::Severity severity) noexcept;

/// The text of a problem in the current language, with its place: "model.dat:12: *CONTACT CONTROLS
/// kept without interpretation".
[[nodiscard]] std::string problem_text(const Problem& problem,
                                       const l10n::Localization& localization);

}  // namespace fem::command
