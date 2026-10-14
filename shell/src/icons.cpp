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
#include <optional>
#include <string>
#include <utility>

namespace fem::shell {

namespace {

/// The share of the size of the default icon left free on each side around the tile.
constexpr qreal default_icon_margin = 0.12;

/// The colours of the status marks: amber for warnings, the coral of the brand for errors.
const QColor warning_color(0xE0, 0x9B, 0x10);

// Shapes of the status marks on a 16x16 grid; the sign is cut out of the shape (even-odd).
constexpr const char* warning_svg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" width="16" height="16">)"
    R"(<path fill-rule="evenodd" d="M8 1.2 15.3 14.6H.7Z M7.1 5.4h1.8v4.9H7.1Z )"
    R"(M7.1 11.4h1.8v1.8H7.1Z"/></svg>)";
constexpr const char* error_svg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" width="16" height="16">)"
    R"(<path fill-rule="evenodd" d="M1 8a7 7 0 1 0 14 0A7 7 0 1 0 1 8Z M4.6 5.9 5.9 4.6 8 6.7 )"
    R"(10.1 4.6 11.4 5.9 9.3 8 11.4 10.1 10.1 11.4 8 9.3 5.9 11.4 4.6 10.1 6.7 8Z"/></svg>)";

/// An SVG drawn as a mask in a colour of the palette of the application, or in a fixed colour.
class TintedSvgEngine final : public QIconEngine {
public:
    explicit TintedSvgEngine(QByteArray svg, std::optional<QColor> color = std::nullopt)
        : svg_(std::move(svg)), color_(color) {}

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

    [[nodiscard]] QIconEngine* clone() const override { return new TintedSvgEngine(svg_, color_); }
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
        // Only the shape stays: every pixel takes the colour, keeping its coverage. A fixed colour
        // gives way to the text of a selection, which may be of the same colour.
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(image.rect(), color_ && mode != QIcon::Selected
                                           ? *color_
                                           : icon_color(QGuiApplication::palette(), mode, state));
        painter.end();
        return image;
    }

    QByteArray svg_;
    std::optional<QColor> color_;
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

QIcon status_icon(form::Severity severity) {
    switch (severity) {
        case form::Severity::warning:
            return QIcon(new TintedSvgEngine(warning_svg, warning_color));
        case form::Severity::error:
            return QIcon(new TintedSvgEngine(error_svg, brand::coral));
        case form::Severity::ok:
            break;
    }
    return {};
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
