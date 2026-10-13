#pragma once

#include "fem/core/context/when.h"
#include "fem/core/extension/contributions.h"
#include "fem/shell/workbench.h"

#include <QString>
#include <functional>
#include <string_view>
#include <utility>

// Commands of modules as the shell shows and runs them (docs/adr/0013-shell-contributions.md):
// their titles, whether they can run now and why not, and running them through the Command Bus.
namespace fem::shell {

/// Whether a command can run now; the reason if not, for the user.
struct Availability {
    bool available = true;
    QString reason;
};

class Commands {
public:
    /// Called with a message for the user when a command fails; may be empty.
    using Report = std::function<void(const QString& message)>;

    explicit Commands(Workbench& workbench) : workbench_(workbench) {}

    void set_report(Report report) { report_ = std::move(report); }

    /// The title of a command in the current language; its id if it is no command of a module.
    [[nodiscard]] QString title(std::string_view id) const;
    [[nodiscard]] QString title(const extension::CommandContribution& command) const;
    /// "Category: Title", or the title if there is no category.
    [[nodiscard]] QString full_title(const extension::CommandContribution& command) const;

    /// Whether a command on the bus can run now: its `enablement` is true and every argument it
    /// needs has a default (the shell has nothing to pass yet; forms come with task 1.13).
    [[nodiscard]] Availability availability(std::string_view id) const;
    /// What a condition asks for, for the user: "a mesh in the model" for `model.hasMesh`, "the
    /// stage “Solve”" for `stage == 'solve'`; other conditions as they are written.
    [[nodiscard]] QString describe(const context::UnmetCondition& condition) const;

    /// Runs a command without arguments if it is available. A failure (an unavailable command, an
    /// error of the command) is logged and reported; returns false then.
    bool run(std::string_view id);

private:
    [[nodiscard]] QString text(std::string_view key, const l10n::Args& args = {}) const;
    void report(const QString& message) const;

    Workbench& workbench_;
    Report report_;
};

}  // namespace fem::shell
