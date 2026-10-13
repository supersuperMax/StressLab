#include "fem/shell/problems.h"

#include <QStringList>

namespace fem::shell {

ProblemsPanel::ProblemsPanel(QWidget* parent) : QListWidget(parent) {
    setObjectName(QStringLiteral("problems.list"));
    setWordWrap(true);
    setTextElideMode(Qt::ElideNone);
    setFrameShape(QFrame::NoFrame);
}

void ProblemsPanel::add(const QString& message) {
    insertItem(0, message);
    Q_EMIT changed();
}

QStringList ProblemsPanel::messages() const {
    QStringList out;
    for (int i = 0; i < count(); ++i) {
        out.push_back(item(i)->text());
    }
    return out;
}

}  // namespace fem::shell
