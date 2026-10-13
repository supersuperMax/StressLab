#include "fem/shell/keys.h"

#include <QStringList>

namespace fem::shell {

QKeySequence key_sequence(std::string_view text) {
    QStringList parts =
        QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())).split(QLatin1Char('+'));
    // "Ctrl" of the portable text of Qt is Cmd on macOS: just what `Mod` means.
    for (QString& part : parts) {
        if (part.trimmed().compare(QLatin1String("Mod"), Qt::CaseInsensitive) == 0) {
            part = QStringLiteral("Ctrl");
        }
    }
    const QKeySequence sequence =
        QKeySequence::fromString(parts.join(QLatin1Char('+')), QKeySequence::PortableText);
    if (sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown ||
        sequence[0].key() == Qt::Key(0)) {
        return {};
    }
    return sequence;
}

QString shortcut_text(const QKeySequence& sequence) {
    return sequence.toString(QKeySequence::NativeText);
}

}  // namespace fem::shell
