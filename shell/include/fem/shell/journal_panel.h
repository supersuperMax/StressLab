#pragma once

#include "fem/core/signal.h"
#include "fem/shell/workbench.h"

#include <QString>
#include <QWidget>
#include <filesystem>
#include <functional>

class QAction;
class QPlainTextEdit;
class QToolBar;

namespace fem::shell {

/// The "Journal" tab of the bottom panel (task 1.14, docs/architecture.md §4.6 item 5): the
/// actions of the session as the lines of Python they are journaled as (`stresslab.cmd(...)`,
/// undo, redo, transactions), newest at the bottom, and "Save journal as script…", which writes
/// them as a .slpy script that `stresslab --headless run-script` runs again.
///
/// Object names, for tests: "journalText", the action "journal.saveAsScript".
class JournalPanel : public QWidget {
    Q_OBJECT

public:
    explicit JournalPanel(Workbench& workbench, QWidget* parent = nullptr);
    ~JournalPanel() override;
    JournalPanel(const JournalPanel&) = delete;
    JournalPanel& operator=(const JournalPanel&) = delete;
    JournalPanel(JournalPanel&&) = delete;
    JournalPanel& operator=(JournalPanel&&) = delete;

    /// "Save journal as script…", also in the "File" menu and the command palette.
    [[nodiscard]] QAction* save_action() const noexcept { return save_; }
    /// Asks for a file and saves the journal there; false if cancelled or it cannot be written.
    bool save_as_script();
    /// The journal as it is shown.
    [[nodiscard]] QString text() const;
    /// Where messages for the user go (the prompt line of the window).
    void set_report(std::function<void(const QString&)> report) { report_ = std::move(report); }

private:
    void append(std::string_view entry);
    void retranslate();
    [[nodiscard]] QString tr_text(std::string_view key, const l10n::Args& args = {}) const;

    Workbench& workbench_;
    QToolBar* bar_ = nullptr;
    QAction* save_ = nullptr;
    QPlainTextEdit* text_ = nullptr;
    std::function<void(const QString&)> report_;
    Subscription entries_;
    Subscription language_;
};

}  // namespace fem::shell
