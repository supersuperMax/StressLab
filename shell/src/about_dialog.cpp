#include "fem/shell/about_dialog.h"

#include "fem/core/product.h"
#include "fem/core/version.h"
#include "fem/shell/brand.h"
#include "fem/shell/strings.h"

#include <QDialogButtonBox>
#include <QEvent>
#include <QLabel>
#include <QPushButton>
#include <QSvgRenderer>
#include <QVBoxLayout>
#include <QtGlobal>
#include <cmath>
#include <string>

namespace fem::shell {

AboutDialog::AboutDialog(const l10n::Localization& localization, QWidget* parent)
    : QDialog(parent) {
    const std::string product(kProductName);
    setObjectName(QStringLiteral("aboutDialog"));
    setWindowTitle(text(localization, "about.title", {{"product", product}}));

    logo_ = new QLabel(this);
    logo_->setObjectName(QStringLiteral("aboutLogo"));
    logo_->setAlignment(Qt::AlignCenter);
    logo_->setAccessibleName(to_qstring(kProductName));

    auto* description = new QLabel(text(localization, "about.description"), this);
    auto* version_label = new QLabel(
        text(localization, "about.version", {{"version", std::string(fem::version())}}), this);
    version_label->setObjectName(QStringLiteral("aboutVersion"));
    version_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* qt =
        new QLabel(text(localization, "about.qt", {{"version", std::string(qVersion())}}), this);
    auto* license = new QLabel(text(localization, "about.license"), this);
    for (QLabel* label : {description, version_label, qt, license}) {
        label->setAlignment(Qt::AlignCenter);
    }
    qt->setForegroundRole(QPalette::PlaceholderText);
    license->setForegroundRole(QPalette::PlaceholderText);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(text(localization, "about.close"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    // The clear space around the logo (guidelines §2) is well inside these margins.
    layout->setContentsMargins(32, 32, 32, 20);
    layout->setSpacing(6);
    layout->addWidget(logo_);
    layout->addSpacing(18);
    layout->addWidget(description);
    layout->addWidget(version_label);
    layout->addWidget(qt);
    layout->addSpacing(12);
    layout->addWidget(license);
    layout->addSpacing(12);
    layout->addWidget(buttons);
    layout->setSizeConstraint(QLayout::SetFixedSize);

    update_logo();
}

void AboutDialog::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        update_logo();
    }
}

void AboutDialog::update_logo() {
    const QString file = brand::logo_file(palette());
    if (file == logo_file_ && !logo_->pixmap().isNull()) {
        return;
    }
    logo_file_ = file;
    // The logo keeps its proportions (guidelines §7): the height follows the width.
    const QSizeF size = QSvgRenderer(file).defaultSize();
    const int height =
        size.width() > 0 ? static_cast<int>(std::lround(logo_width * size.height() / size.width()))
                         : logo_width / 4;
    logo_->setPixmap(brand::render_svg(file, QSize(logo_width, height), devicePixelRatioF()));
}

}  // namespace fem::shell
