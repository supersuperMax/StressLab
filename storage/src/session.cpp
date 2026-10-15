#include "fem/storage/session.h"

#include "fem/core/command/error.h"
#include "fem/core/command/python_literal.h"
#include "fem/core/io/files.h"
#include "fem/core/model/error.h"
#include "fem/core/model/object.h"
#include "fem/core/version.h"
#include "fem/storage/error.h"

#include <system_error>
#include <utility>

namespace fem::storage {

namespace {

namespace fs = std::filesystem;

std::string journal_call(std::string_view function, const fs::path& path) {
    return "stresslab.project." + std::string(function) + "(" +
           command::python_string(io::display(path)) + ")";
}

fs::path absolute_path(const fs::path& path) {
    std::error_code error;
    fs::path result = fs::absolute(path, error);
    return error ? path : result.lexically_normal();
}

}  // namespace

ProjectSession::ProjectSession(command::CommandBus& bus, ModuleVersionLookup versions)
    : bus_(bus), versions_(std::move(versions)) {
    model_changed_ = bus_.on_model_changed([this](const command::ModelChange& change) {
        if (change.cause != command::ModelChange::Cause::load) {
            set_state(folder_, true);
        }
    });
}

fs::path ProjectSession::with_extension(const fs::path& path) {
    fs::path result = path.has_filename() ? path : path.parent_path();
    if (result.extension() != fs::path(project_extension)) {
        result += fs::path(project_extension);
    }
    return result;
}

void ProjectSession::open(const fs::path& folder) {
    check_idle("opening a project");
    LoadedProject project = load_project(folder);
    try {
        bus_.replace_document(std::move(project.contents), journal_call("open", folder));
    } catch (const model::ModelError& error) {
        const auto file = project.files.find(error.object());
        throw ProjectError(ProjectErrc::invalid_model, error.what(),
                           file != project.files.end() ? file->second : std::string());
    }
    opened_versions_ = std::move(project.modules);
    set_state(absolute_path(folder), false);
}

void ProjectSession::save() {
    check_idle("saving the project");
    if (!folder_) {
        throw ProjectError(ProjectErrc::no_folder,
                           "the project has no folder yet: save it with save_as");
    }
    write(*folder_);
    bus_.journal_action("stresslab.project.save()");
    set_state(folder_, false);
}

void ProjectSession::save_as(const fs::path& folder) {
    check_idle("saving the project");
    const fs::path target = with_extension(folder);
    write(target);
    bus_.journal_action(journal_call("save_as", target));
    set_state(absolute_path(target), false);
}

bool ProjectSession::autosave() {
    if (!folder_ || !modified_ || !bus_.is_idle()) {
        return false;
    }
    write(*folder_);
    set_state(folder_, false);
    return true;
}

void ProjectSession::close() {
    check_idle("closing the project");
    bus_.replace_document({}, "stresslab.project.close()");
    opened_versions_.clear();
    set_state(std::nullopt, false);
}

Subscription ProjectSession::on_changed(std::function<void()> listener) {
    return changed_.subscribe(std::move(listener));
}

void ProjectSession::check_idle(std::string_view action) const {
    if (!bus_.is_idle()) {
        throw command::CommandError(
            command::CommandErrc::invalid_state,
            std::string(action) + " is not allowed inside a command or a transaction");
    }
}

ModuleVersions ProjectSession::module_versions() const {
    ModuleVersions result;
    for (const std::string& owner : bus_.document().owners()) {
        std::string version;
        if (owner == model::core_owner) {
            version = std::string(fem::version());
        } else if (versions_) {
            version = versions_(owner);
        }
        if (version.empty()) {
            if (const auto it = opened_versions_.find(owner); it != opened_versions_.end()) {
                version = it->second;
            }
        }
        result.emplace(owner, std::move(version));
    }
    return result;
}

void ProjectSession::write(const fs::path& folder) {
    (void)save_project(folder, bus_.document(), module_versions());
}

void ProjectSession::set_state(std::optional<fs::path> folder, bool modified) {
    if (folder == folder_ && modified == modified_) {
        return;
    }
    folder_ = std::move(folder);
    modified_ = modified;
    changed_.emit();
}

}  // namespace fem::storage
