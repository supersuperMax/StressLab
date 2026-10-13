#include "fem/shell/theme.h"

#include "fem/shell/brand.h"

#include <QApplication>
#include <QGuiApplication>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace fem::shell {

namespace {

struct Colors {
    QColor window;
    QColor text;
    QColor base;
    QColor alternate_base;
    QColor button;
    QColor line;      ///< Splitters and frames (mid).
    QColor muted;     ///< Placeholders, hints.
    QColor disabled;  ///< Text of disabled items.
};

const Colors light{
    .window = brand::paper,
    .text = brand::graphite,
    .base = QColor(0xFF, 0xFF, 0xFF),
    .alternate_base = QColor(0xEC, 0xEE, 0xF1),
    .button = QColor(0xE9, 0xEB, 0xEE),
    .line = QColor(0xD3, 0xD7, 0xDC),
    .muted = QColor(0x6B, 0x72, 0x7B),
    .disabled = QColor(0xA0, 0xA6, 0xAE),
};

const Colors dark{
    .window = brand::graphite,
    .text = brand::paper,
    .base = QColor(0x14, 0x17, 0x1B),
    .alternate_base = QColor(0x22, 0x27, 0x2D),
    .button = QColor(0x2A, 0x30, 0x37),
    .line = QColor(0x0D, 0x0F, 0x12),
    .muted = QColor(0x98, 0x9F, 0xA8),
    .disabled = QColor(0x5E, 0x65, 0x6E),
};

}  // namespace

QPalette theme_palette(Qt::ColorScheme scheme) {
    const Colors& c = scheme == Qt::ColorScheme::Dark ? dark : light;
    // Light, mid, dark and shadow (bevels of Fusion) are derived from the button and window.
    QPalette palette(c.button, c.window);
    palette.setColor(QPalette::Window, c.window);
    palette.setColor(QPalette::WindowText, c.text);
    palette.setColor(QPalette::Base, c.base);
    palette.setColor(QPalette::AlternateBase, c.alternate_base);
    palette.setColor(QPalette::Text, c.text);
    palette.setColor(QPalette::Button, c.button);
    palette.setColor(QPalette::ButtonText, c.text);
    palette.setColor(QPalette::PlaceholderText, c.muted);
    palette.setColor(QPalette::Mid, c.line);
    palette.setColor(QPalette::BrightText, brand::coral);
    // Tooltips are the other way round: graphite on paper or paper on graphite.
    palette.setColor(QPalette::ToolTipBase, c.text);
    palette.setColor(QPalette::ToolTipText, c.window);
    // Coral is for accents only (guidelines §4): the accent and the selection. Graphite on coral
    // reads better than white (about 5:1 against 3:1).
    palette.setColor(QPalette::Accent, brand::coral);
    palette.setColor(QPalette::Highlight, brand::coral);
    palette.setColor(QPalette::HighlightedText, brand::graphite);
    palette.setColor(QPalette::Link, brand::coral);
    palette.setColor(QPalette::LinkVisited, brand::coral);
    for (const QPalette::ColorRole role :
         {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        palette.setColor(QPalette::Disabled, role, c.disabled);
    }
    palette.setColor(QPalette::Disabled, QPalette::Highlight, c.alternate_base);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, c.disabled);
    return palette;
}

void apply_theme(QApplication& application, Qt::ColorScheme scheme) {
    if (application.style()->name().compare(QStringLiteral("fusion"), Qt::CaseInsensitive) != 0) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }
    QApplication::setPalette(theme_palette(scheme));
}

void follow_system_theme(QApplication& application) {
    QStyleHints* hints = QGuiApplication::styleHints();
    apply_theme(application, hints->colorScheme());
    QObject::connect(hints, &QStyleHints::colorSchemeChanged, &application,
                     [&application](Qt::ColorScheme scheme) { apply_theme(application, scheme); });
}

}  // namespace fem::shell
