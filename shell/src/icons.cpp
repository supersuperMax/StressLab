#include "fem/shell/icons.h"

#include "fem/shell/brand.h"

#include <QFile>
#include <QGuiApplication>
#include <QIconEngine>
#include <QImage>
#include <QPainter>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>

namespace fem::shell {

namespace {

/// The share of the size of the default icon left free on each side around the tile.
constexpr qreal default_icon_margin = 0.12;

/// An SVG drawn as a mask in a colour of the palette of the application.
class TintedSvgEngine final : public QIconEngine {
public:
    explicit TintedSvgEngine(QByteArray svg) : svg_(std::move(svg)) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State state) override {
        const qreal ratio =
            painter->device() != nullptr ? painter->device()->devicePixelRatio() : 1;
        const QSize device(static_cast<int>(std::lround(rect.width() * ratio)),
                           static_cast<int>(std::lround(rect.height() * ratio)));
        painter->drawImage(rect, render(device, mode, state));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return QPixmap::fromImage(render(size, mode, state));
    }

    // `size` is in device pixels (QIcon::pixmap multiplies it by the scale).
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override {
        QPixmap pixmap = QPixmap::fromImage(render(size, mode, state));
        pixmap.setDevicePixelRatio(scale);
        return pixmap;
    }

    [[nodiscard]] QIconEngine* clone() const override { return new TintedSvgEngine(svg_); }
    [[nodiscard]] QString key() const override { return QStringLiteral("StressLabTintedSvg"); }
    bool isNull() override { return false; }

private:
    [[nodiscard]] QImage render(QSize size, QIcon::Mode mode, QIcon::State state) const {
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QSvgRenderer renderer(svg_);
        if (!renderer.isValid() || size.isEmpty()) {
            return image;
        }
        const QSizeF drawn =
            QSizeF(renderer.defaultSize()).scaled(QSizeF(size), Qt::KeepAspectRatio);
        const QRectF target(
            QPointF((size.width() - drawn.width()) / 2.0, (size.height() - drawn.height()) / 2.0),
            drawn);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter, target);
        // Only the shape stays: every pixel takes the colour, keeping its coverage.
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(image.rect(), icon_color(QGuiApplication::palette(), mode, state));
        painter.end();
        return image;
    }

    QByteArray svg_;
};

QString to_qstring(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.u16string());
}

}  // namespace

QColor icon_color(const QPalette& palette, QIcon::Mode mode, QIcon::State state) {
    if (mode == QIcon::Disabled) {
        return palette.color(QPalette::PlaceholderText);
    }
    if (mode == QIcon::Selected) {
        return palette.color(QPalette::HighlightedText);
    }
    if (state == QIcon::On) {
        return palette.color(QPalette::Accent);
    }
    return palette.color(QPalette::WindowText);
}

QIcon tinted_svg_icon(const QByteArray& svg) {
    if (!QSvgRenderer(svg).isValid()) {
        return {};
    }
    return QIcon(new TintedSvgEngine(svg));
}

QIcon tinted_svg_icon_file(const QString& file) {
    QFile input(file);
    if (!input.open(QIODevice::ReadOnly)) {
        return {};
    }
    return tinted_svg_icon(input.readAll());
}

QByteArray default_module_icon_svg() {
    QFile kit(brand::symbol_small_file);
    if (!kit.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QString symbol = QString::fromUtf8(kit.readAll());
    // The coral tile is the "hot module" of the symbol: the one a module plugs in.
    const QRegularExpression hot_tile(
        QStringLiteral(R"re(<path\s+fill="%1"\s+d="([^"]+)")re").arg(brand::coral.name()),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = hot_tile.match(symbol);
    if (!match.hasMatch()) {
        return {};
    }
    const QString path = match.captured(1);
    // The bounds of the tile in the grid of the symbol, to frame the icon to it.
    const QString whole =
        QStringLiteral(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">)"
                       R"(<path id="tile" d="%1"/></svg>)")
            .arg(path);
    const QRectF tile = QSvgRenderer(whole.toUtf8()).boundsOnElement(QStringLiteral("tile"));
    if (tile.isEmpty()) {
        return {};
    }
    const qreal side = std::max(tile.width(), tile.height()) / (1 - 2 * default_icon_margin);
    const QPointF corner = tile.center() - QPointF(side / 2, side / 2);
    return QStringLiteral(R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="%1 %2 %3 %3" )"
                          R"(width="24" height="24"><path d="%4"/></svg>)")
        .arg(corner.x())
        .arg(corner.y())
        .arg(side)
        .arg(path)
        .toUtf8();
}

QIcon default_module_icon() {
    return tinted_svg_icon(default_module_icon_svg());
}

QIcon module_icon(const extension::ExtensionManager& extensions, std::string_view owner,
                  std::string_view icon, bool fallback) {
    const auto fall_back = [fallback] { return fallback ? default_module_icon() : QIcon(); };
    if (icon.empty()) {
        return fall_back();
    }
    // An id of `icons` (of any module: icon ids are unique) or a path in the folder of the owner.
    std::string_view folder_owner = owner;
    std::string_view path = icon;
    for (const extension::IconContribution* named : extensions.registry().icons()) {
        if (named->id == icon) {
            folder_owner = named->owner;
            path = named->path;
            break;
        }
    }
    const extension::ExtensionInfo* info = extensions.find(folder_owner);
    if (info == nullptr) {
        return fall_back();
    }
    const std::u8string relative(path.begin(), path.end());
    QIcon result = tinted_svg_icon_file(to_qstring(info->folder / std::filesystem::path(relative)));
    return result.isNull() ? fall_back() : result;
}

}  // namespace fem::shell
