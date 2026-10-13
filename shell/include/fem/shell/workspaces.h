#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <optional>

namespace fem::shell {

/// Where the panels and bars of the window are.
struct Layout {
    /// The panels: the state of the Qt Advanced Docking System (`CDockManager::saveState`, XML).
    QByteArray docks;
    /// The bars: `QMainWindow::saveState` (stage bar, activity bar).
    QByteArray bars;

    friend bool operator==(const Layout&, const Layout&) = default;
};

/// Workspaces: named layouts of the window, like the workspaces of Blender
/// (docs/interface-design.md §4), plus the layout and geometry the window had when it was closed.
/// Kept in the file `workspaces.json` in the settings folder of the user, as deterministic JSON
/// (`to_json`: sorted keys, a fixed indent, '\n' line ends), so that saving what was loaded gives
/// the same bytes.
///
///     {"current": "Postprocessing", "format": "stresslab.workspaces", "formatVersion": 1,
///      "session": {"bars": "<base64>", "docks": "<xml>", "geometry": "<base64>"},
///      "workspaces": {"Postprocessing": {"bars": "<base64>", "docks": "<xml>"}}}
class WorkspaceStore {
public:
    /// An empty store without a file.
    WorkspaceStore() = default;
    /// The store of a file; a missing file is an empty store, a broken one too (logged). `save`
    /// writes there.
    explicit WorkspaceStore(std::filesystem::path file);

    /// The JSON of the store.
    [[nodiscard]] QByteArray to_json() const;
    /// A store from its JSON, without a file. Throws std::runtime_error if it is not a store.
    [[nodiscard]] static WorkspaceStore from_json(const QByteArray& json);

    /// Writes the store to its file at once (through a temporary file); true without a file.
    /// False, and logged, if it cannot be written.
    bool save() const;
    [[nodiscard]] const std::filesystem::path& file() const noexcept { return file_; }

    /// Names of the workspaces, sorted.
    [[nodiscard]] QStringList names() const { return workspaces_.keys(); }
    [[nodiscard]] std::optional<Layout> find(const QString& name) const;
    /// Adds a workspace or replaces the one of this name.
    void set(const QString& name, const Layout& layout);
    /// Removes a workspace; the current one becomes the default if it was this one.
    bool remove(const QString& name);

    /// The workspace opened last; empty: the default layout.
    [[nodiscard]] const QString& current() const noexcept { return current_; }
    void set_current(const QString& name) { current_ = name; }

    /// The layout and the geometry of the window when it was closed.
    [[nodiscard]] const std::optional<Layout>& session() const noexcept { return session_; }
    [[nodiscard]] const QByteArray& geometry() const noexcept { return geometry_; }
    void set_session(const Layout& layout, const QByteArray& geometry);

private:
    std::filesystem::path file_;
    QMap<QString, Layout> workspaces_;
    QString current_;
    std::optional<Layout> session_;
    QByteArray geometry_;
};

}  // namespace fem::shell
