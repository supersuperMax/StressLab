#pragma once

#include "fem/shell/commands.h"
#include "fem/shell/workbench.h"

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QString>
#include <functional>
#include <vector>

class QAction;
class QLineEdit;
class QListWidget;
class QListWidgetItem;

namespace fem::shell {

/// The command palette (docs/interface-design.md §7), Ctrl+Shift+P or Cmd+Shift+P: every command
/// of every installed module and the actions of the shell, with the shortcut and the module they
/// come from. Commands that cannot run now are grey, with the reason; a command of a disabled
/// module says so and offers to enable the module (docs/architecture.md §4.6, item 8).
///
/// Typing filters the list: every word must be in the title, the category, the id or the module
/// name, in any case. Enter (or a click) runs the selected command and closes the palette; on a
/// command of a disabled module it enables the module and the palette stays open with the command
/// now available. Escape closes it.
///
/// Object names, for tests: "palette.filter", "palette.list"; each row has the id of its command
/// (or the object name of the action) as data `Qt::UserRole` and, for a disabled module, an
/// "Enable" button "palette.enable".
class CommandPalette : public QDialog {
    Q_OBJECT

public:
    /// What a row of the palette does; for tests.
    enum class RowKind { run, unavailable, enable, cannot_enable };

    /// `shell_actions` are the actions of the shell to list too; they belong to the window.
    CommandPalette(Workbench& workbench, Commands& commands, QList<QAction*> shell_actions,
                   QWidget* parent = nullptr);

    /// Lists the commands again for the current registry and context, keeping the filter.
    void reload();
    /// Shows the palette with an empty filter.
    void open_palette();

    [[nodiscard]] QLineEdit* filter() const noexcept { return filter_; }
    [[nodiscard]] QListWidget* list() const noexcept { return list_; }
    /// The row of a command or action shown now, or nullptr.
    [[nodiscard]] QListWidgetItem* row(const QString& id) const;
    [[nodiscard]] RowKind row_kind(const QListWidgetItem* row) const;
    /// The second line of a row: the module, or why it cannot run.
    [[nodiscard]] QString row_detail(const QListWidgetItem* row) const;
    /// Does what the row offers: runs the command or enables its module.
    void activate(QListWidgetItem* row);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Entry {
        QString id;
        QString title;
        QString detail;
        QString shortcut;
        QString search;  ///< Lowercase text the filter looks in.
        RowKind kind = RowKind::run;
        std::function<void()> action;
    };

    void collect();
    void apply_filter();
    void add_row(const Entry& entry, int index);
    [[nodiscard]] QString text(std::string_view key, const l10n::Args& args = {}) const;
    [[nodiscard]] const Entry* entry_of(const QListWidgetItem* row) const;

    Workbench& workbench_;
    Commands& commands_;
    QList<QPointer<QAction>> shell_actions_;
    std::vector<Entry> entries_;
    QLineEdit* filter_ = nullptr;
    QListWidget* list_ = nullptr;
};

}  // namespace fem::shell
