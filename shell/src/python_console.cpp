#include "fem/shell/python_console.h"

#include "fem/core/log/log.h"
#include "fem/shell/brand.h"
#include "fem/shell/strings.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QStringListModel>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>
#include <algorithm>
#include <exception>

namespace fem::shell {

namespace {

const QString first_prompt = QStringLiteral(">>>");
const QString next_prompt = QStringLiteral("...");

/// The longest start all lines share.
QString common_prefix(const QStringList& lines) {
    QString prefix = lines.isEmpty() ? QString() : lines.front();
    for (const QString& line : lines) {
        qsizetype length = 0;
        while (length < prefix.size() && length < line.size() && prefix[length] == line[length]) {
            ++length;
        }
        prefix.truncate(length);
    }
    return prefix;
}

}  // namespace

PythonConsolePanel::PythonConsolePanel(Workbench& workbench, QWidget* parent)
    : QWidget(parent), workbench_(workbench) {
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    output_ = new QPlainTextEdit(this);
    output_->setObjectName(QStringLiteral("consoleOutput"));
    output_->setReadOnly(true);
    output_->setFont(fixed);
    output_->setFrameShape(QFrame::NoFrame);
    output_->setUndoRedoEnabled(false);

    prompt_ = new QLabel(first_prompt, this);
    prompt_->setObjectName(QStringLiteral("consolePrompt"));
    prompt_->setFont(fixed);
    input_ = new QLineEdit(this);
    input_->setObjectName(QStringLiteral("consoleInput"));
    input_->setFont(fixed);
    input_->setFrame(false);
    input_->installEventFilter(this);
    connect(input_, &QLineEdit::returnPressed, this, [this] { run(input_->text()); });

    matches_ = new QStringListModel(this);
    completer_ = new QCompleter(matches_, this);
    completer_->setObjectName(QStringLiteral("consoleCompleter"));
    completer_->setWidget(input_);
    completer_->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    completer_->popup()->setFont(fixed);
    connect(completer_, qOverload<const QString&>(&QCompleter::activated), this,
            [this](const QString& line) {
                // What was after the cursor stays after the completed text.
                const QString after = input_->text().mid(input_->cursorPosition());
                input_->setText(line + after);
                input_->setCursorPosition(static_cast<int>(line.size()));
            });

    auto* line = new QHBoxLayout;
    line->setContentsMargins(6, 2, 6, 4);
    line->addWidget(prompt_);
    line->addWidget(input_, 1);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(output_, 1);
    layout->addLayout(line);

    if (workbench_.console == nullptr) {
        append(shell::text(workbench_.localization, "console.unavailable",
                           {{"reason", workbench_.console_unavailable}}),
               true);
        input_->setEnabled(false);
    } else {
        append(workbench_.console->banner(), false, true);
        append(text("console.hint"), false, true);
    }
    retranslate();
    language_ =
        workbench_.localization.on_language_changed([this](const std::string&) { retranslate(); });
}

PythonConsolePanel::~PythonConsolePanel() {
    // The line edit may lose the focus while it is destroyed (Linux) and report to `this`.
    input_->removeEventFilter(this);
    input_->disconnect(this);
    completer_->disconnect(this);
}

void PythonConsolePanel::run(const QString& line) {
    if (workbench_.console == nullptr) {
        return;
    }
    completer_->popup()->hide();
    append(prompt() + QLatin1Char(' ') + line, false, true);
    if (!line.trimmed().isEmpty() && (history_.isEmpty() || history_.back() != line)) {
        history_.push_back(line);
    }
    history_index_ = history_.size();
    draft_.clear();
    input_->clear();
    try {
        const ConsoleEngine::Reply reply = workbench_.console->push(line);
        more_ = reply.more;
        for (const ConsoleOutput& piece : reply.output) {
            append(piece.text, piece.error);
        }
    } catch (const std::exception& error) {
        log::logger("shell")->error("the Python console failed: {}", error.what());
        more_ = false;
        append(to_qstring(error.what()), true);
    }
    prompt_->setText(prompt());
}

void PythonConsolePanel::complete() {
    if (workbench_.console == nullptr) {
        return;
    }
    const QString before = input_->text().left(input_->cursorPosition());
    const QString after = input_->text().mid(input_->cursorPosition());
    QStringList lines;
    try {
        lines = workbench_.console->complete(before);
    } catch (const std::exception& error) {
        log::logger("shell")->warn("completion failed: {}", error.what());
    }
    if (lines.isEmpty()) {
        return;
    }
    const QString start = lines.size() == 1 ? lines.front() : common_prefix(lines);
    if (start.size() > before.size()) {
        input_->setText(start + after);
        input_->setCursorPosition(static_cast<int>(start.size()));
    }
    if (lines.size() > 1) {
        matches_->setStringList(lines);
        completer_->setCompletionPrefix(QString());
        completer_->complete();
    }
}

QString PythonConsolePanel::transcript() const {
    return output_->toPlainText();
}

QString PythonConsolePanel::prompt() const {
    return more_ ? next_prompt : first_prompt;
}

bool PythonConsolePanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == input_ && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        const bool plain = (key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
        if (completer_->popup()->isVisible()) {
            return false;  // The list handles its keys.
        }
        if (plain && key->key() == Qt::Key_Tab) {
            complete();
            return true;
        }
        if (plain && key->key() == Qt::Key_Up) {
            show_history(-1);
            return true;
        }
        if (plain && key->key() == Qt::Key_Down) {
            show_history(1);
            return true;
        }
        if (plain && key->key() == Qt::Key_Escape) {
            reset_input();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PythonConsolePanel::append(const QString& text, bool error, bool echo) {
    QTextCursor cursor(output_->document());
    cursor.movePosition(QTextCursor::End);
    QTextCharFormat format;
    if (error) {
        format.setForeground(brand::coral);
    } else if (echo) {
        format.setForeground(palette().color(QPalette::PlaceholderText));
    }
    QString shown = text;
    if (shown.endsWith(QLatin1Char('\n'))) {
        shown.chop(1);
    }
    if (!output_->document()->isEmpty()) {
        cursor.insertBlock();
    }
    cursor.insertText(shown, format);
    output_->verticalScrollBar()->setValue(output_->verticalScrollBar()->maximum());
}

void PythonConsolePanel::show_history(int step) {
    if (history_.isEmpty()) {
        return;
    }
    if (history_index_ == history_.size()) {
        draft_ = input_->text();
    }
    history_index_ = std::clamp<qsizetype>(history_index_ + step, 0, history_.size());
    input_->setText(history_index_ == history_.size() ? draft_ : history_[history_index_]);
}

void PythonConsolePanel::reset_input() {
    input_->clear();
    if (more_ && workbench_.console != nullptr) {
        workbench_.console->reset();
        more_ = false;
        prompt_->setText(prompt());
    }
}

void PythonConsolePanel::retranslate() {
    input_->setPlaceholderText(text("console.placeholder"));
}

QString PythonConsolePanel::text(std::string_view key) const {
    return shell::text(workbench_.localization, key);
}

}  // namespace fem::shell
