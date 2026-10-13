#include "fem/shell/brand.h"

#include <QPainter>
#include <QSvgRenderer>
#include <cmath>

namespace fem::shell::brand {

QPixmap render_svg(const QString& file, QSize size, qreal device_pixel_ratio) {
    QSvgRenderer renderer(file);
    if (!renderer.isValid() || size.isEmpty()) {
        return {};
    }
    const QSize device(static_cast<int>(std::lround(size.width() * device_pixel_ratio)),
                       static_cast<int>(std::lround(size.height() * device_pixel_ratio)));
    QPixmap pixmap(device);
    pixmap.fill(Qt::transparent);
    // The drawing keeps its proportions and stands in the middle of the size.
    const QSizeF drawn = QSizeF(renderer.defaultSize()).scaled(QSizeF(device), Qt::KeepAspectRatio);
    const QRectF target(
        QPointF((device.width() - drawn.width()) / 2.0, (device.height() - drawn.height()) / 2.0),
        drawn);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&painter, target);
    painter.end();
    pixmap.setDevicePixelRatio(device_pixel_ratio);
    return pixmap;
}

QIcon application_icon() {
    QIcon icon;
    for (const int size : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256}) {
        const QString& file = size <= small_symbol_max_size ? symbol_small_file : app_icon_file;
        icon.addPixmap(render_svg(file, QSize(size, size)));
    }
    return icon;
}

bool is_dark(const QPalette& palette) {
    return palette.color(QPalette::Window).lightnessF() <
           palette.color(QPalette::WindowText).lightnessF();
}

QString logo_file(const QPalette& palette) {
    return is_dark(palette) ? logo_reversed_file : logo_color_file;
}

}  // namespace fem::shell::brand
