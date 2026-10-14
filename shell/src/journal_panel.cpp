#include "fem/shell/journal_panel.h"

#include "fem/core/log/log.h"
#include "fem/shell/strings.h"

#include <QAction>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <exception>

namespace fem::shell {

JournalPanel::JournalPanel(Workbench& workbench, QWidget* parent)
    : QWidget(parent), workbench_(workbench) {
    bar_ = new QToolBar(this);
    bar_->setObjectName(QStringLiteral("journalBar"));
    bar_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    save_ = bar_->addAction(QString(), this, [this] { (void)save_as_script(); });
    save_->setObjectName(QStringLiteral("journal.saveAsScript"));

    text_ = new QPlainTextEdit(this);
    text_->setObjectName(QStringLiteral("journalText"));
    text_->setReadOnly(true);
    text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text_->setFrameShape(QFrame::NoFrame);
    text_->setLineWrapMode(QPlainTextEdit::NoWrap);
    text_->setUndoRedoEnabled(false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(bar_);
    layout->addWidget(text_, 1);

    if (workbench_.journal != nullptr) {
        for (const std::string& entry : workbench_.journal->entries()) {
            append(entry);
        }
        entries_ = workbench_.journal->on_entry([this](std::string_view entry) { append(entry); });
    } else {
        save_->setEnabled(false);
    }
    retranslate();
    language_ =
        workbench_.localization.on_language_changed([this](const std::string&) { retranslate(); });
}

JournalPanel::~JournalPanel() = default;

bool JournalPanel::save_as_script() {
    if (workbench_.journal == nullptr || !workbench_.dialogs.save_file) {
        return false;
    }
    std::filesystem::path file =
        workbench_.dialogs.save_file(this, tr_text("journal.saveTitle"),
                                     QStringLiteral("session.slpy"), tr_text("journal.filter"));
    if (file.empty()) {
        return false;
    }
    if (!file.has_extension()) {
        file.replace_extension(".slpy");
    }
    QString message;
    bool saved = false;
    try {
        workbench_.journal->save(file);
        message = tr_text("journal.saved", {{"file", path_text(file).toStdString()}});
        saved = true;
    } catch (const std::exception& error) {
        log::logger("shell")->error("the journal cannot be saved: {}", error.what());
        message = tr_text("journal.notSaved", {{"file", path_text(file).toStdString()}});
    }
    if (report_) {
        report_(message);
    }
    return saved;
}

QString JournalPanel::text() const {
    return text_->toPlainText();
}

void JournalPanel::append(std::string_view entry) {
    text_->appendPlainText(to_qstring(entry));
    text_->verticalScrollBar()->setValue(text_->verticalScrollBar()->maximum());
}

void JournalPanel::retranslate() {
    save_->setText(tr_text("journal.saveAsScript"));
    text_->setPlaceholderText(tr_text("journal.empty"));
}

QString JournalPanel::tr_text(std::string_view key, const l10n::Args& args) const {
    return shell::text(workbench_.localization, key, args);
}

}  // namespace fem::shell
