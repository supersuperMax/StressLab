#include "fem/shell/project_actions.h"

#include "fem/core/command/error.h"
#include "fem/core/log/log.h"
#include "fem/core/product.h"
#include "fem/core/settings/settings.h"
#include "fem/shell/strings.h"
#include "fem/storage/error.h"
#include "fem/storage/project.h"

#include <QAction>
#include <QKeySequence>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace fem::shell {

namespace {

namespace fs = std::filesystem;

std::string utf8(const fs::path& path) {
    return path_text(path).toStdString();
}

/// The message box of the system: Save, Don't Save, Cancel.
UnsavedChanges ask_with_message_box(const l10n::Localization& localization, QWidget* parent,
                                    const QString& project) {
    QMessageBox box(QMessageBox::Warning, text(localization, "project.unsavedTitle"),
                    text(localization, "project.unsavedText", {{"project", project.toStdString()}}),
                    QMessageBox::NoButton, parent);
    box.setObjectName(QStringLiteral("unsavedChangesDialog"));
    QPushButton* save =
        box.addButton(text(localization, "project.unsavedSave"), QMessageBox::AcceptRole);
    QPushButton* discard =
        box.addButton(text(localization, "project.unsavedDiscard"), QMessageBox::DestructiveRole);
    box.addButton(text(localization, "dialog.cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(save);
    (void)box.exec();
    if (box.clickedButton() == save) {
        return UnsavedChanges::save;
    }
    return box.clickedButton() == discard ? UnsavedChanges::discard : UnsavedChanges::cancel;
}

}  // namespace

ProjectActions::ProjectActions(Workbench& workbench, QWidget* window)
    : QObject(window), workbench_(workbench), window_(window) {
    open_ = new QAction(this);
    open_->setObjectName(QStringLiteral("project.open"));
    open_->setShortcuts(QKeySequence::Open);
    connect(open_, &QAction::triggered, this, [this] { (void)open_dialog(); });
    save_ = new QAction(this);
    save_->setObjectName(QStringLiteral("project.save"));
    save_->setShortcuts(QKeySequence::Save);
    connect(save_, &QAction::triggered, this, [this] { (void)save(); });
    save_as_ = new QAction(this);
    save_as_->setObjectName(QStringLiteral("project.saveAs"));
    connect(save_as_, &QAction::triggered, this, [this] { (void)save_as(); });
    clean_ = new QAction(this);
    clean_->setObjectName(QStringLiteral("project.clean"));
    connect(clean_, &QAction::triggered, this, [this] { (void)clean(); });

    autosave_ = new QTimer(this);
    autosave_->setObjectName(QStringLiteral("project.autosave"));
    connect(autosave_, &QTimer::timeout, this, &ProjectActions::autosave);

    const bool available = workbench_.project != nullptr;
    for (QAction* action : {open_, save_, save_as_}) {
        action->setEnabled(available);
    }
    clean_->setEnabled(false);  // Until there is a folder: `update_state`.
    if (available) {
        project_changed_ = workbench_.project->on_changed([this] { update_state(); });
        settings_changed_ = workbench_.settings.on_changed([this](std::string_view key) {
            if (key == settings::keys::autosave_interval) {
                follow_autosave_interval();
            }
        });
        follow_autosave_interval();
    }
    retranslate();
}

ProjectActions::~ProjectActions() = default;

bool ProjectActions::open_dialog() {
    if (workbench_.project == nullptr || !workbench_.dialogs.open_folder) {
        return false;
    }
    const fs::path folder = workbench_.dialogs.open_folder(window_, text("project.openTitle"));
    return !folder.empty() && open(folder);
}

bool ProjectActions::open(const fs::path& folder) {
    if (workbench_.project == nullptr || !confirm_close()) {
        return false;
    }
    try {
        workbench_.project->open(folder);
    } catch (const std::exception& error) {
        report("project.openFailed", folder, error);
        return false;
    }
    Q_EMIT opened();
    return true;
}

bool ProjectActions::save() {
    if (workbench_.project == nullptr) {
        return false;
    }
    const std::optional<fs::path>& folder = workbench_.project->folder();
    if (!folder) {
        return save_as();
    }
    try {
        workbench_.project->save();
    } catch (const std::exception& error) {
        report("project.saveFailed", *folder, error);
        return false;
    }
    return true;
}

bool ProjectActions::save_as() {
    if (workbench_.project == nullptr || !workbench_.dialogs.save_file) {
        return false;
    }
    const fs::path folder = workbench_.dialogs.save_file(
        window_, text("project.saveAsTitle"), project_name() + QStringLiteral(".slproj"),
        text("project.filter", {{"product", std::string(kProductName)}}));
    if (folder.empty()) {
        return false;
    }
    try {
        workbench_.project->save_as(folder);
    } catch (const std::exception& error) {
        report("project.saveFailed", storage::ProjectSession::with_extension(folder), error);
        return false;
    }
    return true;
}

bool ProjectActions::clean() {
    if (workbench_.project == nullptr || !workbench_.project->folder()) {
        return false;
    }
    const fs::path folder = *workbench_.project->folder();
    storage::CleanStats stats;
    try {
        stats = workbench_.project->clean();
    } catch (const std::exception& error) {
        report("project.cleanFailed", folder, error);
        return false;
    }
    log::logger("shell")->info("project.clean '{}': {} files removed, {} bytes freed, {} kept",
                               utf8(folder), stats.removed, stats.freed_bytes, stats.kept);
    // Megabytes with one decimal: "12.3".
    const double megabytes =
        std::round(static_cast<double>(stats.freed_bytes) / (1024.0 * 1024.0) * 10.0) / 10.0;
    Q_EMIT message(text("project.cleaned", {{"count", static_cast<std::int64_t>(stats.removed)},
                                            {"megabytes", megabytes}}));
    return true;
}

void ProjectActions::autosave() {
    if (workbench_.project == nullptr) {
        return;
    }
    try {
        (void)workbench_.project->autosave();
    } catch (const std::exception& error) {
        report("project.autosaveFailed", workbench_.project->folder().value_or(fs::path()), error);
    }
}

bool ProjectActions::confirm_close() {
    if (workbench_.project == nullptr || !workbench_.project->is_modified()) {
        return true;
    }
    const UnsavedChanges choice =
        workbench_.ask_unsaved
            ? workbench_.ask_unsaved(window_, project_name())
            : ask_with_message_box(workbench_.localization, window_, project_name());
    switch (choice) {
        case UnsavedChanges::save:
            return save();
        case UnsavedChanges::discard:
            return true;
        case UnsavedChanges::cancel:
            break;
    }
    return false;
}

QString ProjectActions::project_name() const {
    if (workbench_.project == nullptr || !workbench_.project->folder()) {
        return text("project.untitled");
    }
    fs::path folder = *workbench_.project->folder();
    if (folder.extension() == fs::path(storage::project_extension)) {
        folder.replace_extension();
    }
    return path_text(folder.filename());
}

QString ProjectActions::window_title() const {
    if (workbench_.project == nullptr) {
        return to_qstring(kProductName);
    }
    if (!variant_.isEmpty() && workbench_.project->folder()) {
        return text("project.windowTitleVariant", {{"project", project_name().toStdString()},
                                                   {"variant", variant_.toStdString()},
                                                   {"product", std::string(kProductName)}});
    }
    return text("project.windowTitle", {{"project", project_name().toStdString()},
                                        {"product", std::string(kProductName)}});
}

void ProjectActions::set_variant(const QString& variant) {
    if (variant != variant_) {
        variant_ = variant;
        update_state();
    }
}

void ProjectActions::retranslate() {
    open_->setText(text("action.openProject"));
    save_->setText(text("action.save"));
    save_as_->setText(text("action.saveAs"));
    clean_->setText(text("action.cleanProject"));
    update_state();
}

void ProjectActions::update_state() {
    if (window_ == nullptr) {
        return;
    }
    clean_->setEnabled(workbench_.project != nullptr && workbench_.project->folder());
    window_->setWindowTitle(window_title());
    window_->setWindowModified(workbench_.project != nullptr && workbench_.project->is_modified());
}

void ProjectActions::follow_autosave_interval() {
    const auto seconds = workbench_.settings.get<std::int64_t>(settings::keys::autosave_interval);
    if (seconds <= 0) {
        autosave_->stop();
        return;
    }
    autosave_->start(static_cast<int>(std::min<std::int64_t>(seconds, 86400) * 1000));
}

void ProjectActions::report(std::string_view action_key, const fs::path& folder,
                            const std::exception& error) {
    log::logger("shell")->error("{} '{}': {}", action_key, utf8(folder), error.what());
    std::string reason;
    if (const auto* project = dynamic_cast<const storage::ProjectError*>(&error)) {
        reason = text("project.error." + std::string(storage::to_string(project->code())),
                      {{"file", project->file()}, {"product", std::string(kProductName)}})
                     .toStdString();
    } else if (dynamic_cast<const command::CommandError*>(&error) != nullptr) {
        reason = text("project.error.busy").toStdString();
    } else {
        reason = error.what();
    }
    Q_EMIT problem(text(action_key, {{"folder", utf8(folder)}, {"reason", reason}}));
}

QString ProjectActions::text(std::string_view key, const l10n::Args& args) const {
    return shell::text(workbench_.localization, key, args);
}

}  // namespace fem::shell
