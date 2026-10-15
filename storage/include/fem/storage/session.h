#pragma once

#include "fem/core/command/command_bus.h"
#include "fem/core/signal.h"
#include "fem/storage/project.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace fem::storage {

/// The project of a document: the folder it is saved to and whether the model changed since it
/// was opened or saved (task 2.1). The window (Ctrl/Cmd+S, autosave), `stresslab.project` in
/// Python and the headless runs use it alike.
///
/// Opening, saving and closing are written to the journal as `stresslab.project.open("...")`,
/// `stresslab.project.save()`, `stresslab.project.save_as("...")` and `stresslab.project.close()`,
/// so a journal replays them; `autosave` is not. Like the bus, it is used from one thread.
class ProjectSession {
public:
    /// The version of an installed module by its id, "" if it is not installed.
    using ModuleVersionLookup = std::function<std::string(std::string_view id)>;

    /// Works with the document of `bus`, which must outlive the session. Without `versions`
    /// every module but the core is saved with the version the opened project had, or "".
    explicit ProjectSession(command::CommandBus& bus, ModuleVersionLookup versions = {});
    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;
    ProjectSession(ProjectSession&&) = delete;
    ProjectSession& operator=(ProjectSession&&) = delete;
    ~ProjectSession() = default;

    /// The project folder; none for a project never saved.
    [[nodiscard]] const std::optional<std::filesystem::path>& folder() const noexcept {
        return folder_;
    }
    /// The model changed since the project was opened, saved or closed.
    [[nodiscard]] bool is_modified() const noexcept { return modified_; }

    /// Opens the project of `folder` in place of the document. Throws ProjectError (the model of
    /// the project is checked too: invalid_model names the file of the object at fault) and
    /// CommandError(invalid_state) inside a command or a transaction; then nothing changes.
    void open(const std::filesystem::path& folder);
    /// Saves to the project folder. Throws ProjectError(no_folder) for a project never saved,
    /// other ProjectErrors from `save_project` and CommandError(invalid_state) inside a command or
    /// a transaction.
    void save();
    /// Saves to `folder`, which becomes the project folder; ".slproj" is added if the name has
    /// another extension. Throws as `save` does.
    void save_as(const std::filesystem::path& folder);
    /// Saves if there is a folder and the model changed, and the bus is idle; true if it saved.
    /// Not written to the journal. Throws as `save` does.
    bool autosave();
    /// A new empty document in place of this one, without a folder. Throws
    /// CommandError(invalid_state) inside a command or a transaction.
    void close();

    /// Called when the folder or `is_modified` changes.
    [[nodiscard]] Subscription on_changed(std::function<void()> listener);

    /// The project folder for `path`: `path` itself if it ends with ".slproj", otherwise `path`
    /// with ".slproj" added.
    [[nodiscard]] static std::filesystem::path with_extension(const std::filesystem::path& path);

private:
    void check_idle(std::string_view action) const;
    [[nodiscard]] ModuleVersions module_versions() const;
    void write(const std::filesystem::path& folder);
    void set_state(std::optional<std::filesystem::path> folder, bool modified);

    command::CommandBus& bus_;
    ModuleVersionLookup versions_;
    std::optional<std::filesystem::path> folder_;
    bool modified_ = false;
    /// Versions the opened project had, for modules that are not installed now.
    ModuleVersions opened_versions_;
    Signal<> changed_;
    Subscription model_changed_;
};

}  // namespace fem::storage
