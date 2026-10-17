#include "fem/shell/history_controller.h"

#include "fem/core/command/error.h"
#include "fem/core/command/value.h"
#include "fem/core/log/log.h"
#include "fem/core/product.h"
#include "fem/core/settings/settings.h"
#include "fem/shell/history_text.h"
#include "fem/shell/strings.h"
#include "fem/storage/diff.h"
#include "fem/storage/error.h"
#include "fem/storage/history.h"
#include "fem/storage/session.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <set>
#include <utility>

namespace fem::shell {

namespace {

/// How many changes the automatic message of a version names before "and N more".
constexpr std::size_t automatic_message_changes = 3;
/// The longest automatic message, in characters.
constexpr qsizetype automatic_message_length = 200;

/// The dialog of the shell for changes not in the history: the message of the version to commit
/// them as, and Commit, Put aside, Cancel.
PendingChanges ask_with_dialog(const HistoryController& history, QWidget* parent,
                               QString& message) {
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("pendingChangesDialog"));
    dialog.setWindowTitle(history.text("history.pendingTitle"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* label = new QLabel(history.text("history.pendingText"), &dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto* field = new QLineEdit(message, &dialog);
    field->setObjectName(QStringLiteral("pendingChangesDialog.message"));
    layout->addWidget(field);
    auto* buttons = new QDialogButtonBox(&dialog);
    QPushButton* commit =
        buttons->addButton(history.text("history.pendingCommit"), QDialogButtonBox::AcceptRole);
    QPushButton* stash =
        buttons->addButton(history.text("history.pendingStash"), QDialogButtonBox::ActionRole);
    buttons->addButton(history.text("dialog.cancel"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    PendingChanges choice = PendingChanges::cancel;
    QObject::connect(commit, &QPushButton::clicked, &dialog, [&] {
        choice = PendingChanges::commit;
        dialog.accept();
    });
    QObject::connect(stash, &QPushButton::clicked, &dialog, [&] {
        choice = PendingChanges::stash;
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    commit->setDefault(true);
    (void)dialog.exec();
    message = field->text();
    return choice;
}

/// The versions on the line of first parents from `head`.
std::set<std::string> line_of(const HistoryState& state, const std::string& head) {
    std::set<std::string> line;
    const storage::VersionInfo* at = state.find(head);
    while (at != nullptr && line.insert(at->id).second) {
        at = at->parents.empty() ? nullptr : state.find(at->parents.front());
    }
    return line;
}

}  // namespace

const storage::VersionInfo* HistoryState::find(std::string_view id) const {
    const auto it = std::find_if(versions.begin(), versions.end(),
                                 [id](const storage::VersionInfo& info) { return info.id == id; });
    if (it != versions.end()) {
        return &*it;
    }
    const auto stashed =
        std::find_if(stashes.begin(), stashes.end(),
                     [id](const storage::VersionInfo& info) { return info.id == id; });
    return stashed == stashes.end() ? nullptr : &*stashed;
}

HistoryController::HistoryController(Workbench& workbench, QWidget* window)
    : QObject(window), workbench_(workbench), window_(window) {
    if (workbench_.project != nullptr) {
        history_changed_ = workbench_.project->history().on_changed([this] { refresh(); });
        project_changed_ = workbench_.project->on_changed([this] { refresh(); });
    }
    settings_changed_ =
        workbench_.settings.on_changed([this](std::string_view key) { follow_settings(key); });
    language_ =
        workbench_.localization.on_language_changed([this](const std::string&) { refresh(); });
    refresh();
}

HistoryController::~HistoryController() {
    // The window goes: what it viewed is not read-only for the rest of the application.
    if (state_.viewing()) {
        workbench_.bus.set_read_only(false);
    }
}

void HistoryController::refresh() {
    HistoryState state;
    if (workbench_.project != nullptr && workbench_.project->folder()) {
        state.has_folder = true;
        try {
            const storage::ProjectHistory& history = workbench_.project->history();
            state.branch = history.current_branch();
            state.version = history.current_version();
            state.versions = history.log({}, true);
            state.branches = history.branches();
            state.tags = history.tags();
            state.stashes = history.stashes();
            state.results = workbench_.project->results().list();
        } catch (const std::exception& error) {
            log::logger("shell")->error("the history cannot be read: {}", error.what());
        }
    }
    state_ = std::move(state);
    if (!state_.viewing()) {
        came_from_.reset();
    }
    apply_read_only();
    Q_EMIT changed();
}

void HistoryController::apply_read_only() {
    workbench_.bus.set_read_only(state_.viewing());
}

void HistoryController::follow_settings(std::string_view key) {
    if (key == settings::keys::git_terms) {
        Q_EMIT terms_changed();
        refresh();
    }
}

// --- Words ---------------------------------------------------------------------------------------

QString HistoryController::text(std::string_view key, const l10n::Args& args) const {
    return history_text(workbench_.localization, workbench_.settings, key, args);
}

QString HistoryController::variant_title(std::string_view name) const {
    return shell::variant_title(workbench_.localization, workbench_.settings, name);
}

QString HistoryController::time(std::int64_t seconds) const {
    return version_time(workbench_.localization, seconds);
}

QString HistoryController::current_title() const {
    if (!state_.has_folder) {
        return {};
    }
    if (state_.viewing()) {
        const storage::VersionInfo* info = state_.find(*state_.version);
        return text("history.viewingTitle",
                    {{"time", info != nullptr ? time(info->time).toStdString() : std::string()}});
    }
    return variant_title(state_.branch.value_or(std::string(storage::main_branch)));
}

// --- Changes not in the history ------------------------------------------------------------------

std::vector<std::string> HistoryController::pending_changes() const {
    if (!state_.has_folder || !state_.version) {
        return {};
    }
    try {
        const storage::ModelDiff diff = workbench_.project->history().diff(*state_.version);
        return storage::describe(diff, {.localization = workbench_.localization,
                                        .registry = &workbench_.extensions.registry()});
    } catch (const std::exception& error) {
        log::logger("shell")->error("the changes of the model cannot be listed: {}", error.what());
        return {};
    }
}

QString HistoryController::automatic_message() const {
    if (!state_.version) {
        return text("history.firstVersion");
    }
    const std::vector<std::string> changes = pending_changes();
    if (changes.empty()) {
        return text("history.noChanges");
    }
    std::string joined;
    const std::size_t named = std::min(changes.size(), automatic_message_changes);
    for (std::size_t i = 0; i < named; ++i) {
        joined += (i == 0 ? "" : "; ") + changes[i];
    }
    QString message = to_qstring(joined);
    if (message.size() > automatic_message_length) {
        message = message.left(automatic_message_length - 1) + QStringLiteral("…");
    }
    if (changes.size() > named) {
        message = text("history.moreChanges",
                       {{"changes", message.toStdString()},
                        {"count", static_cast<std::int64_t>(changes.size() - named)}});
    }
    return message;
}

bool HistoryController::has_changes() const {
    if (!state_.has_folder) {
        return false;
    }
    try {
        return workbench_.project->history().has_changes();
    } catch (const std::exception& error) {
        log::logger("shell")->error("the changes of the model cannot be found: {}", error.what());
        return false;
    }
}

// --- Actions -------------------------------------------------------------------------------------

bool HistoryController::commit(const QString& message) {
    if (workbench_.project == nullptr) {
        return false;
    }
    const QString text = message.trimmed().isEmpty() ? automatic_message() : message.trimmed();
    try {
        (void)workbench_.project->history().commit(text.toStdString());
    } catch (const std::exception& error) {
        report("history.commitFailed", error);
        return false;
    }
    Q_EMIT this->message(this->text("history.committed", {{"message", text.toStdString()}}));
    return true;
}

bool HistoryController::settle_changes() {
    if (!has_changes()) {
        return true;
    }
    QString message = automatic_message();
    const PendingChanges choice = workbench_.ask_pending ? workbench_.ask_pending(window_, message)
                                                         : ask_with_dialog(*this, window_, message);
    switch (choice) {
        case PendingChanges::commit:
            return commit(message);
        case PendingChanges::stash:
            try {
                workbench_.project->history().stash(message.trimmed().toStdString());
            } catch (const std::exception& error) {
                report("history.stashFailed", error);
                return false;
            }
            Q_EMIT this->message(text("history.stashed"));
            return true;
        case PendingChanges::cancel:
            break;
    }
    return false;
}

bool HistoryController::open(const std::string& target, std::string_view action_key) {
    if (workbench_.project == nullptr || !state_.has_folder || !settle_changes()) {
        return false;
    }
    try {
        workbench_.project->history().checkout(target);
    } catch (const std::exception& error) {
        report(action_key, error);
        return false;
    }
    return true;
}

bool HistoryController::switch_to(const std::string& variant) {
    if (state_.branch == variant) {
        return true;
    }
    if (!open(variant, "history.switchFailed")) {
        return false;
    }
    Q_EMIT message(text("history.switched", {{"variant", variant_title(variant).toStdString()}}));
    return true;
}

bool HistoryController::view(const std::string& version) {
    const std::optional<std::string> from = state_.viewing() ? came_from_ : state_.branch;
    if (!open(version, "history.viewFailed")) {
        return false;
    }
    // `open` read the state again, which forgot where the version was viewed from.
    came_from_ = from;
    return true;
}

bool HistoryController::return_to_current() {
    if (!state_.viewing()) {
        return true;
    }
    std::string target;
    if (came_from_ && state_.branches.contains(*came_from_)) {
        target = *came_from_;
    } else if (state_.branches.contains(std::string(storage::main_branch))) {
        target = std::string(storage::main_branch);
    } else {
        for (const auto& [name, head] : state_.branches) {
            if (line_of(state_, head).contains(*state_.version)) {
                target = name;
                break;
            }
        }
        if (target.empty() && !state_.branches.empty()) {
            target = state_.branches.begin()->first;
        }
    }
    if (target.empty()) {
        return false;
    }
    return switch_to(target);
}

bool HistoryController::make_variant(const std::string& version, const QString& name) {
    if (workbench_.project == nullptr || !state_.has_folder) {
        return false;
    }
    const std::string at = version.empty() ? state_.version.value_or(std::string()) : version;
    const QString chosen =
        name.isEmpty() ? ask_name("history.newVariantTitle", "history.newVariantLabel", {}) : name;
    if (chosen.trimmed().isEmpty() || at.empty()) {
        return false;
    }
    const std::string variant = chosen.trimmed().toStdString();
    try {
        workbench_.project->history().branch(variant, at);
    } catch (const std::exception& error) {
        report("history.newVariantFailed", error);
        return false;
    }
    return switch_to(variant);
}

bool HistoryController::restore(const std::string& version) {
    if (workbench_.project == nullptr || !state_.has_folder) {
        return false;
    }
    if (state_.viewing() && !return_to_current()) {
        return false;
    }
    try {
        (void)workbench_.project->history().restore_version(version);
    } catch (const std::exception& error) {
        report("history.restoreFailed", error);
        return false;
    }
    const storage::VersionInfo* info = state_.find(version);
    Q_EMIT message(text("history.restored",
                        {{"time", info != nullptr ? time(info->time).toStdString() : version}}));
    return true;
}

bool HistoryController::restore_objects(const std::string& version,
                                        const std::vector<model::ObjectId>& ids) {
    if (workbench_.project == nullptr || ids.empty()) {
        return false;
    }
    command::Value::List objects;
    for (const model::ObjectId id : ids) {
        objects.emplace_back(id);
    }
    try {
        (void)workbench_.bus.execute(storage::ProjectHistory::restore_objects_command,
                                     {{"version", version}, {"objects", std::move(objects)}});
    } catch (const std::exception& error) {
        report("history.restoreFailed", error);
        return false;
    }
    Q_EMIT message(
        text("history.objectsRestored", {{"count", static_cast<std::int64_t>(ids.size())}}));
    return true;
}

bool HistoryController::tag(const std::string& version, const QString& name) {
    if (workbench_.project == nullptr || !state_.has_folder) {
        return false;
    }
    const QString chosen =
        name.isEmpty() ? ask_name("history.newLabelTitle", "history.newLabelLabel", {}) : name;
    if (chosen.trimmed().isEmpty()) {
        return false;
    }
    try {
        workbench_.project->history().tag(chosen.trimmed().toStdString(), version);
    } catch (const std::exception& error) {
        report("history.labelFailed", error);
        return false;
    }
    return true;
}

bool HistoryController::stash_pop(std::size_t index) {
    if (workbench_.project == nullptr) {
        return false;
    }
    try {
        workbench_.project->history().stash_pop(index);
    } catch (const std::exception& error) {
        report("history.stashPopFailed", error);
        return false;
    }
    return true;
}

bool HistoryController::stash_drop(std::size_t index) {
    if (workbench_.project == nullptr) {
        return false;
    }
    try {
        workbench_.project->history().stash_drop(index);
    } catch (const std::exception& error) {
        report("history.stashDropFailed", error);
        return false;
    }
    return true;
}

// --- Results -------------------------------------------------------------------------------------

std::vector<const storage::ResultInfo*> HistoryController::results_of(
    std::string_view version) const {
    std::vector<const storage::ResultInfo*> found;
    for (const storage::ResultInfo& result : state_.results) {
        if (result.version == version) {
            found.push_back(&result);
        }
    }
    return found;
}

bool HistoryController::results_outdated(std::string_view variant) const {
    const auto head = state_.branches.find(std::string(variant));
    if (head == state_.branches.end() || !results_of(head->second).empty()) {
        return false;
    }
    const std::set<std::string> line = line_of(state_, head->second);
    return std::any_of(
        state_.results.begin(), state_.results.end(),
        [&line](const storage::ResultInfo& result) { return line.contains(result.version); });
}

// --- Helpers -------------------------------------------------------------------------------------

QString HistoryController::ask_name(std::string_view title_key, std::string_view label_key,
                                    const QString& text) const {
    const QString title = this->text(title_key);
    const QString label = this->text(label_key);
    if (workbench_.ask_name) {
        return workbench_.ask_name(window_, title, label, text);
    }
    bool accepted = false;
    const QString answer =
        QInputDialog::getText(window_, title, label, QLineEdit::Normal, text, &accepted);
    return accepted ? answer : QString();
}

void HistoryController::report(std::string_view action_key, const std::exception& error) {
    log::logger("shell")->error("{}: {}", action_key, error.what());
    std::string reason;
    if (const auto* project = dynamic_cast<const storage::ProjectError*>(&error)) {
        reason = shell::text(workbench_.localization,
                             "project.error." + std::string(storage::to_string(project->code())),
                             {{"file", project->file()}, {"product", std::string(kProductName)}})
                     .toStdString();
    } else if (const auto* command = dynamic_cast<const command::CommandError*>(&error)) {
        reason =
            shell::text(workbench_.localization, command->code() == command::CommandErrc::read_only
                                                     ? "project.error.readOnly"
                                                     : "project.error.busy")
                .toStdString();
    } else {
        reason = error.what();
    }
    Q_EMIT problem(text(action_key, {{"reason", reason}}));
}

}  // namespace fem::shell
