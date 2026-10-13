#include "fem/shell/strings.h"

#include "fem/core/l10n/catalog.h"
#include "fem/core/l10n/error.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace fem::shell {

void add_shell_catalogs(l10n::Localization& localization) {
    const QDir folder(QStringLiteral(":/stresslab/l10n"));
    for (const QFileInfo& file : folder.entryInfoList({QStringLiteral("*.json")}, QDir::Files)) {
        QFile input(file.filePath());
        if (!input.open(QIODevice::ReadOnly)) {
            throw l10n::L10nError(l10n::L10nErrc::io,
                                  "cannot read " + file.filePath().toStdString());
        }
        const QByteArray json = input.readAll();
        localization.add_catalog(strings_owner, file.completeBaseName().toStdString(),
                                 l10n::parse_catalog(std::string_view(
                                     json.constData(), static_cast<std::size_t>(json.size()))));
    }
}

QString text(const l10n::Localization& localization, std::string_view key, const l10n::Args& args) {
    return to_qstring(localization.text(strings_owner, key, args));
}

QString to_qstring(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

}  // namespace fem::shell
