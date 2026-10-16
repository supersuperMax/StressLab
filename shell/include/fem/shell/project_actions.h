#pragma once

#include "fem/core/signal.h"
#include "fem/shell/workbench.h"

#include <QObject>
#include <QString>
#include <exception>
#include <filesystem>
#include <string_view>

class QAction;
class QTimer;
class QWidget;

namespace fem::shell {

/// The project folder in the window (task 2.1, docs/interface-design.md §12): "Open project…"
/// (Ctrl/Cmd+O), "Save" (Ctrl/Cmd+S), "Save as…" (no shortcut: Ctrl/Cmd+Shift+S is kept for
/// "Commit version"), "Clean project" (task 2.2: removes blocks of data the saved project no
/// longer needs), autosave every "project.autosaveInterval" seconds, the title of the window
/// with the name of the project and a mark of unsaved changes, and the question about unsaved
/// changes before the window closes or another project opens.
///
/// Failures are not modal: their message goes to the "Problems" tab and the prompt line
/// (`problem`), the details to the log; what "Clean project" did goes to the prompt line
/// (`message`). Object names, for tests: the actions "project.open", "project.save",
/// "project.saveAs", "project.clean"; the timer "project.autosave".
class ProjectActions : public QObject {
    Q_OBJECT

public:
    /// `window` is the parent of dialogs and gets the title. Without `workbench.project` the
    /// actions are disabled.
    ProjectActions(Workbench& workbench, QWidget* window);
    ~ProjectActions() override;
    ProjectActions(const ProjectActions&) = delete;
    ProjectActions& operator=(const ProjectActions&) = delete;

    [[nodiscard]] QAction* open_action() const noexcept { return open_; }
    [[nodiscard]] QAction* save_action() const noexcept { return save_; }
    [[nodiscard]] QAction* save_as_action() const noexcept { return save_as_; }
    /// Enabled while the project has a folder.
    [[nodiscard]] QAction* clean_action() const noexcept { return clean_; }
    [[nodiscard]] QTimer& autosave_timer() const noexcept { return *autosave_; }

    /// Asks for a folder and opens it; false if cancelled or it failed.
    bool open_dialog();
    /// Opens a project folder in place of the document, after asking about unsaved changes;
    /// false if the user cancelled or it failed.
    bool open(const std::filesystem::path& folder);
    /// Saves to the project folder, or asks for one if there is none; false if cancelled or it
    /// failed.
    bool save();
    /// Asks for a folder and saves there; false if cancelled or it failed.
    bool save_as();
    /// Removes the blocks of data the saved project no longer refers to; false if there is no
    /// folder or it failed.
    bool clean();
    /// Saves if autosave is due (a folder, unsaved changes, nothing running); failures are
    /// reported like the others.
    void autosave();
    /// If the model has unsaved changes, asks whether to save them: true to go on (saved or
    /// discarded), false if the user cancelled or saving failed.
    bool confirm_close();

    /// The title of the window: "<project>[*] — StressLab", with "[*]" for QWidget's mark of
    /// unsaved changes; the product name alone without a project session.
    [[nodiscard]] QString window_title() const;
    /// The name the project is shown with: its folder without ".slproj", or "Untitled".
    [[nodiscard]] QString project_name() const;

    void retranslate();

Q_SIGNALS:
    /// A project was opened: the window checks the modules its model needs.
    void opened();
    /// A message for the user about a failure.
    void problem(const QString& message);
    /// A message for the user about what was done.
    void message(const QString& message);

private:
    void update_state();
    void follow_autosave_interval();
    void report(std::string_view action_key, const std::filesystem::path& folder,
                const std::exception& error);
    [[nodiscard]] QString text(std::string_view key, const l10n::Args& args = {}) const;

    Workbench& workbench_;
    QWidget* window_;
    QAction* open_ = nullptr;
    QAction* save_ = nullptr;
    QAction* save_as_ = nullptr;
    QAction* clean_ = nullptr;
    QTimer* autosave_ = nullptr;
    Subscription project_changed_;
    Subscription settings_changed_;
};

}  // namespace fem::shell
