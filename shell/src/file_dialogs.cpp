#include "fem/shell/file_dialogs.h"

#include <QDir>
#include <QFileDialog>

namespace fem::shell {

QString path_text(const std::filesystem::path& path) {
    return QDir::toNativeSeparators(QString::fromStdU16String(path.generic_u16string()));
}

std::filesystem::path to_path(const QString& text) {
    return std::filesystem::path(QDir::fromNativeSeparators(text).toStdU16String());
}

FileDialogs system_file_dialogs() {
    return {
        .open_file =
            [](QWidget* parent, const QString& title, const QString& filter) {
                const QString file = QFileDialog::getOpenFileName(parent, title, {}, filter);
                return file.isEmpty() ? std::filesystem::path() : to_path(file);
            },
        .save_file =
            [](QWidget* parent, const QString& title, const QString& name, const QString& filter) {
                const QString file = QFileDialog::getSaveFileName(parent, title, name, filter);
                return file.isEmpty() ? std::filesystem::path() : to_path(file);
            },
        .open_folder =
            [](QWidget* parent, const QString& title) {
                const QString folder = QFileDialog::getExistingDirectory(parent, title);
                return folder.isEmpty() ? std::filesystem::path() : to_path(folder);
            },
    };
}

}  // namespace fem::shell
