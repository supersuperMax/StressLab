#pragma once

#include "fem/core/l10n/localization.h"

#include <QDialog>
#include <QString>

class QEvent;
class QLabel;

namespace fem::shell {

/// "About StressLab": the horizontal logo of the kit ("color" on a light theme, "reversed" on a
/// dark one, switched when the palette changes), the version, the version of Qt and the license.
class AboutDialog : public QDialog {
    Q_OBJECT

public:
    explicit AboutDialog(const l10n::Localization& localization, QWidget* parent = nullptr);

    /// The logo file shown now.
    [[nodiscard]] const QString& logo_file() const noexcept { return logo_file_; }

    /// Width of the logo in logical pixels; the kit wants at least 96 (guidelines §3).
    static constexpr int logo_width = 240;

protected:
    void changeEvent(QEvent* event) override;

private:
    void update_logo();

    QLabel* logo_ = nullptr;
    QString logo_file_;
};

}  // namespace fem::shell
