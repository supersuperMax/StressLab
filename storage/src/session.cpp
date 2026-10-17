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
    history_.reset(new ProjectHistory(*this, bus_));
    results_.reset(new ResultCache(*this));
}

ProjectSession::~ProjectSession() = default;

fs::path ProjectSession::with_extension(const fs::path& path) {
    fs::path result = path.has_filename() ? path : path.parent_path();
    if (result.extension() != fs::path(project_extension)) {
        result += fs::path(project_extension);
    }
    return result;
}

fs::path ProjectSession::with_package_extension(const fs::path& path) {
    fs::path result = path.has_filename() ? path : path.parent_path();
    if (result.extension() != fs::path(package_extension)) {
        result += fs::path(package_extension);
    }
    return result;
}

void ProjectSession::open(const fs::path& folder) {
    check_idle("opening a project");
    open_folder(folder, journal_call("open", folder));
}

void ProjectSession::open_folder(const fs::path& folder, std::string_view entry) {
    replace(load_project(folder), entry);
    set_state(absolute_path(folder), false);
    history_->opened(*folder_);
}

void ProjectSession::replace(LoadedProject project, std::string_view entry) {
    try {
        bus_.replace_document(std::move(project.contents), entry);
    } catch (const model::ModelError& error) {
        const auto file = project.files.find(error.object());
        throw ProjectError(ProjectErrc::invalid_model, error.what(),
                           file != project.files.end() ? file->second : std::string());
    }
    opened_versions_ = std::move(project.modules);
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

CleanStats ProjectSession::clean() {
    check_idle("cleaning the project");
    if (!folder_) {
        throw ProjectError(ProjectErrc::no_folder, "the project has no folder to clean");
    }
    const CleanStats stats = clean_project(*folder_, history_->blob_roots());
    bus_.journal_action("stresslab.project.clean()");
    return stats;
}

PackStats ProjectSession::pack(const fs::path& package, bool with_history) {
    check_idle("packing the project");
    if (!folder_) {
        throw ProjectError(ProjectErrc::no_folder,
                           "the project has no folder yet: save it with save_as to pack it");
    }
    const fs::path target = with_package_extension(package);
    write(*folder_);
    set_state(folder_, false);
    const PackStats stats = pack_project(*folder_, target, {.history = with_history});
    bus_.journal_action("stresslab.project.pack(" + command::python_string(io::display(target)) +
                        ", history=" + (with_history ? "True" : "False") + ")");
    return stats;
}

PackStats ProjectSession::unpack(const fs::path& package, const fs::path& folder) {
    check_idle("unpacking a project");
    const fs::path target = with_extension(folder);
    const PackStats stats = unpack_project(package, target);
    open_folder(target, "stresslab.project.unpack(" + command::python_string(io::display(package)) +
                            ", " + command::python_string(io::display(target)) + ")");
    return stats;
}

void ProjectSession::close() {
    check_idle("closing the project");
    bus_.replace_document({}, "stresslab.project.close()");
    opened_versions_.clear();
    set_state(std::nullopt, false);
    history_->closed();
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
    history_->saved(folder);
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
