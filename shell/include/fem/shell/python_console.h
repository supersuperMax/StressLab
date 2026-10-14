#pragma once

#include "fem/shell/workbench.h"

#include <QString>
#include <QStringList>
#include <QWidget>
#include <vector>

class QCompleter;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QStringListModel;

namespace fem::shell {

/// A piece of what code run in the console printed.
struct ConsoleOutput {
    bool error = false;  ///< Standard error and tracebacks.
    QString text = {};
};

/// What the console runs lines with: the embedded Python of the application (`app/`), or a fake
/// in tests. The shell does not link Python itself.
class ConsoleEngine {
public:
    struct Reply {
        /// The statement is not complete yet: more lines are needed.
        bool more = false;
        std::vector<ConsoleOutput> output = {};
    };

    ConsoleEngine() = default;
    ConsoleEngine(const ConsoleEngine&) = delete;
    ConsoleEngine& operator=(const ConsoleEngine&) = delete;
    ConsoleEngine(ConsoleEngine&&) = delete;
    ConsoleEngine& operator=(ConsoleEngine&&) = delete;
    virtual ~ConsoleEngine() = default;

    /// The first line of the console: versions of Python and of the API.
    [[nodiscard]] virtual QString banner() const = 0;
    /// Runs a line, or keeps it until the statement it starts is complete.
    virtual Reply push(const QString& line) = 0;
    /// Drops the lines of a statement that is not complete yet.
    virtual void reset() = 0;
    /// The lines Tab can make of `line`.
    [[nodiscard]] virtual QStringList complete(const QString& line) = 0;
};

/// The "Python Console" tab of the bottom panel (task 1.14, docs/architecture.md §4.7): a line to
/// type Python into, with `stresslab` imported, and what it printed above it. Enter runs the line
/// (a statement of several lines runs at the empty line after it, the prompt is "..." meanwhile),
/// Up and Down go through the lines typed before, Tab completes the name or attribute at the end
/// of the line (one match is put in, several are offered in a list), Escape drops an unfinished
/// statement. Commands run from the console go through the Command Bus like any others: they can
/// be undone and are journaled.
///
/// Code runs on the thread of the window, like everything that changes the model: long code
/// blocks the window until it ends, and its output shows when it ends.
///
/// Object names, for tests: "consoleOutput", "consoleInput", "consolePrompt".
class PythonConsolePanel : public QWidget {
    Q_OBJECT

public:
    explicit PythonConsolePanel(Workbench& workbench, QWidget* parent = nullptr);
    ~PythonConsolePanel() override;
    PythonConsolePanel(const PythonConsolePanel&) = delete;
    PythonConsolePanel& operator=(const PythonConsolePanel&) = delete;
    PythonConsolePanel(PythonConsolePanel&&) = delete;
    PythonConsolePanel& operator=(PythonConsolePanel&&) = delete;

    /// Runs a line as if it was typed and Enter pressed.
    void run(const QString& line);
    /// Completes the line being typed, as Tab does.
    void complete();
    /// The text shown above the line, as plain text.
    [[nodiscard]] QString transcript() const;
    /// ">>>" or "...".
    [[nodiscard]] QString prompt() const;
    [[nodiscard]] QLineEdit* input() const noexcept { return input_; }
    /// The list Tab offers when several lines match.
    [[nodiscard]] QCompleter* completer() const noexcept { return completer_; }
    /// The lines run, the oldest first.
    [[nodiscard]] const QStringList& history() const noexcept { return history_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void append(const QString& text, bool error, bool echo = false);
    void show_history(int step);
    void reset_input();
    void retranslate();
    [[nodiscard]] QString text(std::string_view key) const;

    Workbench& workbench_;
    QPlainTextEdit* output_ = nullptr;
    QLabel* prompt_ = nullptr;
    QLineEdit* input_ = nullptr;
    QCompleter* completer_ = nullptr;
    QStringListModel* matches_ = nullptr;
    QStringList history_;
    /// Where Up and Down are in the history: history_.size() is the line being typed.
    qsizetype history_index_ = 0;
    /// The line being typed when Up was pressed, to come back to with Down.
    QString draft_;
    bool more_ = false;
    Subscription language_;
};

}  // namespace fem::shell
