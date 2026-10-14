#pragma once

#include <QString>
#include <filesystem>
#include <functional>

class QWidget;

namespace fem::shell {

/// How the shell asks the user for files and folders (task 1.14). The window uses the dialogs of
/// the system (docs/interface-design.md §10); tests put functions of their own here. Each returns
/// the chosen path, or an empty path if the user cancelled.
struct FileDialogs {
    /// A file to open: (parent, title, filter like "Profiles (*.json)").
    std::function<std::filesystem::path(QWidget*, const QString&, const QString&)> open_file;
    /// A file to save: (parent, title, suggested file name, filter).
    std::function<std::filesystem::path(QWidget*, const QString&, const QString&, const QString&)>
        save_file;
    /// A folder: (parent, title).
    std::function<std::filesystem::path(QWidget*, const QString&)> open_folder;
};

/// The dialogs of the system.
[[nodiscard]] FileDialogs system_file_dialogs();

/// A path as a QString, with the separators of the OS.
[[nodiscard]] QString path_text(const std::filesystem::path& path);
/// A QString as a path.
[[nodiscard]] std::filesystem::path to_path(const QString& text);

}  // namespace fem::shell
