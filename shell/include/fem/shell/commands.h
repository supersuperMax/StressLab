#pragma once

#include "fem/core/context/when.h"
#include "fem/core/extension/contributions.h"
#include "fem/core/model/object_id.h"
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
    /// needs has a default, except `object`, which the object selected in the model tree gives
    /// (docs/adr/0014-tree-and-forms.md).
    [[nodiscard]] Availability availability(std::string_view id) const;
    /// What a condition asks for, for the user: "a mesh in the model" for `model.hasMesh`, "the
    /// stage “Solve”" for `stage == 'solve'`; other conditions as they are written.
    [[nodiscard]] QString describe(const context::UnmetCondition& condition) const;

    /// Runs a command if it is available, with the selected object as its argument `object` if it
    /// has one. A failure (an unavailable command, an error of the command) is logged and
    /// reported; returns false then.
    bool run(std::string_view id);

    /// The object selected in the model tree; an invalid id if none.
    void set_selected_object(model::ObjectId id) { selected_ = id; }
    [[nodiscard]] model::ObjectId selected_object() const noexcept { return selected_; }

    /// Reports a message for the user, as a failed command does.
    void report(const QString& message) const;

private:
    [[nodiscard]] QString text(std::string_view key, const l10n::Args& args = {}) const;
    /// The argument `object` a command takes from the selection: required, of type object.
    [[nodiscard]] static bool takes_selection(const command::ArgSpec& arg);

    Workbench& workbench_;
    Report report_;
    model::ObjectId selected_;
};

}  // namespace fem::shell
