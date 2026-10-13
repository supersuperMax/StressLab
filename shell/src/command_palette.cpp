#include "fem/shell/command_palette.h"

#include "fem/core/extension/error.h"
#include "fem/core/log/log.h"
#include "fem/core/product.h"
#include "fem/shell/keys.h"
#include "fem/shell/strings.h"

#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace fem::shell {

namespace {

/// The widest the palette gets, and its margin to the edges of the window.
constexpr int palette_width = 640;
constexpr int palette_margin = 24;

/// The text of an action without its mnemonic marks: "&File" → "File", "&&" → "&".
QString plain_text(const QString& text) {
    QString out;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] == QLatin1Char('&') && i + 1 < text.size()) {
            ++i;
        }
        out.push_back(text[i]);
    }
    return out;
}

}  // namespace

CommandPalette::CommandPalette(Workbench& workbench, Commands& commands,
                               QList<QAction*> shell_actions, QWidget* parent)
    : QDialog(parent, Qt::Popup), workbench_(workbench), commands_(commands) {
    setObjectName(QStringLiteral("commandPalette"));
    for (QAction* action : shell_actions) {
        shell_actions_.push_back(action);
    }
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("palette.filter"));
    filter_->setClearButtonEnabled(true);
    filter_->installEventFilter(this);
    layout->addWidget(filter_);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("palette.list"));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setUniformItemSizes(false);
    layout->addWidget(list_);
    connect(filter_, &QLineEdit::textChanged, this, [this] { apply_filter(); });
    connect(list_, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* row) { activate(row); });
    resize(palette_width, 420);
}

void CommandPalette::open_palette() {
    filter_->clear();
    reload();
    if (QWidget* window = parentWidget()) {
        const int width = std::min(palette_width, window->width() - 2 * palette_margin);
        resize(std::max(width, 200), height());
        move(window->mapToGlobal(QPoint((window->width() - this->width()) / 2, palette_margin)));
    }
    filter_->setPlaceholderText(text("palette.placeholder"));
    show();
    raise();
    activateWindow();
    filter_->setFocus();
}

void CommandPalette::reload() {
    collect();
    apply_filter();
}

void CommandPalette::collect() {
    entries_.clear();
    extension::ExtensionManager& extensions = workbench_.extensions;
    const l10n::Localization& localization = workbench_.localization;
    // Shortcuts of modules, by command.
    const auto shortcut = [&](const std::string& command) {
        for (const extension::KeybindingContribution* binding :
             extensions.registry().keybindings()) {
            if (binding->command == command) {
                return shortcut_text(key_sequence(binding->key));
            }
        }
        return QString();
    };
    for (const extension::IndexedCommand& indexed : extensions.command_index()) {
        const extension::CommandContribution& command = *indexed.command;
        const extension::ExtensionInfo& module = *indexed.extension;
        const QString module_name =
            to_qstring(localization.manifest_text(module.id(), module.manifest->name));
        Entry entry{.id = to_qstring(command.id),
                    .title = commands_.full_title(command),
                    .shortcut = shortcut(command.id)};
        if (indexed.enabled()) {
            const Availability availability = commands_.availability(command.id);
            entry.kind = availability.available ? RowKind::run : RowKind::unavailable;
            entry.detail = availability.available ? module_name : availability.reason;
            entry.action = [this, id = command.id] { (void)commands_.run(id); };
        } else {
            try {
                (void)extensions.plan_enable(module.id());
                entry.kind = RowKind::enable;
                entry.detail =
                    text("palette.disabledModule", {{"module", module_name.toStdString()}});
                entry.action = [this, id = module.id()] {
                    try {
                        (void)workbench_.extensions.enable(id);
                    } catch (const extension::ExtensionError& error) {
                        log::logger("shell")->error("module '{}' cannot be enabled: {}", id,
                                                    error.what());
                    }
                };
            } catch (const extension::ExtensionError& error) {
                entry.kind = RowKind::cannot_enable;
                entry.detail =
                    text("reason.cannotEnable", {{"module", module_name.toStdString()},
                                                 {"message", std::string(error.what())}});
            }
        }
        entry.search =
            QStringList{entry.title, entry.id, module_name}.join(QLatin1Char(' ')).toCaseFolded();
        entries_.push_back(std::move(entry));
    }
    for (const QPointer<QAction>& action : shell_actions_) {
        if (action.isNull() || action->isSeparator() || action->text().isEmpty()) {
            continue;
        }
        Entry entry{.id = action->objectName(),
                    .title = plain_text(action->text()),
                    .detail = to_qstring(kProductName),
                    .shortcut = shortcut_text(action->shortcut()),
                    .kind = action->isEnabled() ? RowKind::run : RowKind::unavailable,
                    .action = [action] {
                        if (action) {
                            action->trigger();
                        }
                    }};
        entry.search = QStringList{entry.title, entry.id}.join(QLatin1Char(' ')).toCaseFolded();
        entries_.push_back(std::move(entry));
    }
    std::stable_sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        return QString::localeAwareCompare(a.title, b.title) < 0;
    });
}

void CommandPalette::apply_filter() {
    const QString previous = list_->currentItem() != nullptr
                                 ? list_->currentItem()->data(Qt::UserRole).toString()
                                 : QString();
    list_->clear();
    const QStringList words =
        filter_->text().toCaseFolded().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
        const Entry& entry = entries_[static_cast<std::size_t>(i)];
        if (std::all_of(words.begin(), words.end(),
                        [&entry](const QString& word) { return entry.search.contains(word); })) {
            add_row(entry, i);
        }
    }
    QListWidgetItem* current = previous.isEmpty() ? nullptr : row(previous);
    if (current == nullptr && list_->count() > 0) {
        current = list_->item(0);
    }
    list_->setCurrentItem(current);
}

void CommandPalette::add_row(const Entry& entry, int index) {
    auto* item = new QListWidgetItem(list_);
    item->setData(Qt::UserRole, entry.id);
    item->setData(Qt::UserRole + 1, index);
    item->setToolTip(entry.detail);

    auto* widget = new QWidget;
    auto* row_layout = new QHBoxLayout(widget);
    row_layout->setContentsMargins(6, 3, 6, 3);
    auto* texts = new QVBoxLayout;
    texts->setSpacing(0);
    auto* title = new QLabel(entry.title, widget);
    title->setObjectName(QStringLiteral("palette.title"));
    title->setTextFormat(Qt::PlainText);
    auto* detail = new QLabel(entry.detail, widget);
    detail->setObjectName(QStringLiteral("palette.detail"));
    detail->setTextFormat(Qt::PlainText);
    detail->setForegroundRole(QPalette::PlaceholderText);
    QFont small = detail->font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    detail->setFont(small);
    // What cannot run now is grey.
    if (entry.kind == RowKind::unavailable || entry.kind == RowKind::cannot_enable) {
        title->setForegroundRole(QPalette::PlaceholderText);
    }
    texts->addWidget(title);
    texts->addWidget(detail);
    row_layout->addLayout(texts, 1);
    if (!entry.shortcut.isEmpty()) {
        auto* keys = new QLabel(entry.shortcut, widget);
        keys->setObjectName(QStringLiteral("palette.shortcut"));
        keys->setForegroundRole(QPalette::PlaceholderText);
        row_layout->addWidget(keys);
    }
    if (entry.kind == RowKind::enable) {
        auto* enable = new QPushButton(text("palette.enable"), widget);
        enable->setObjectName(QStringLiteral("palette.enable"));
        enable->setFocusPolicy(Qt::NoFocus);
        // Queued: enabling lists the rows again, deleting this button.
        connect(
            enable, &QPushButton::clicked, this, [this, id = entry.id] { activate(row(id)); },
            Qt::QueuedConnection);
        row_layout->addWidget(enable);
    }
    item->setSizeHint(widget->sizeHint());
    list_->setItemWidget(item, widget);
}

QListWidgetItem* CommandPalette::row(const QString& id) const {
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(Qt::UserRole).toString() == id) {
            return list_->item(i);
        }
    }
    return nullptr;
}

const CommandPalette::Entry* CommandPalette::entry_of(const QListWidgetItem* row) const {
    if (row == nullptr) {
        return nullptr;
    }
    const int index = row->data(Qt::UserRole + 1).toInt();
    if (index < 0 || index >= static_cast<int>(entries_.size())) {
        return nullptr;
    }
    return &entries_[static_cast<std::size_t>(index)];
}

CommandPalette::RowKind CommandPalette::row_kind(const QListWidgetItem* row) const {
    const Entry* entry = entry_of(row);
    return entry != nullptr ? entry->kind : RowKind::unavailable;
}

QString CommandPalette::row_detail(const QListWidgetItem* row) const {
    const Entry* entry = entry_of(row);
    return entry != nullptr ? entry->detail : QString();
}

void CommandPalette::activate(QListWidgetItem* row) {
    const Entry* entry = entry_of(row);
    if (entry == nullptr) {
        return;
    }
    switch (entry->kind) {
        case RowKind::run: {
            // Closed first: the command may open a dialog of its own.
            const std::function<void()> action = entry->action;
            accept();
            action();
            break;
        }
        case RowKind::enable: {
            // Copies: enabling rebuilds the window, which may reload the palette.
            const QString id = entry->id;
            const std::function<void()> action = entry->action;
            action();
            // The command of the module is there now, available or not.
            reload();
            if (QListWidgetItem* again = this->row(id)) {
                list_->setCurrentItem(again);
            }
            break;
        }
        case RowKind::unavailable:
        case RowKind::cannot_enable:
            break;
    }
}

bool CommandPalette::eventFilter(QObject* watched, QEvent* event) {
    if (watched == filter_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
            case Qt::Key_Down:
            case Qt::Key_Up: {
                if (list_->count() == 0) {
                    return true;
                }
                const int step = key->key() == Qt::Key_Down ? 1 : -1;
                const int next = std::clamp(list_->currentRow() + step, 0, list_->count() - 1);
                list_->setCurrentRow(next);
                return true;
            }
            case Qt::Key_Return:
            case Qt::Key_Enter:
                activate(list_->currentItem());
                return true;
            default:
                break;
        }
    }
    return QDialog::eventFilter(watched, event);
}

QString CommandPalette::text(std::string_view key, const l10n::Args& args) const {
    return shell::text(workbench_.localization, key, args);
}

}  // namespace fem::shell
