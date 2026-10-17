#pragma once

#include <QFrame>
#include <QString>
#include <QToolButton>

class QLabel;
class QEvent;
class QMenu;
class QPushButton;

namespace fem::shell {

class HistoryController;

/// The bar over the 3D view while a version is viewed (task 2.6, docs/interface-design.md §12):
/// "You are looking at the version of 10.10.2026 14:05 «…». It is read-only. [Back to current]
/// [New variant from here] [Restore]". Hidden otherwise.
///
/// Object names, for tests: "versionBar", "versionBar.text", "versionBar.back",
/// "versionBar.newVariant", "versionBar.restore".
class VersionBar : public QFrame {
    Q_OBJECT

public:
    explicit VersionBar(HistoryController& history, QWidget* parent = nullptr);
    ~VersionBar() override;
    VersionBar(const VersionBar&) = delete;
    VersionBar& operator=(const VersionBar&) = delete;
    VersionBar(VersionBar&&) = delete;
    VersionBar& operator=(VersionBar&&) = delete;

    /// The whole text; the bar shows it in one line, shortened to fit.
    [[nodiscard]] QString message() const;

protected:
    /// Shortens the text again when its label is resized.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void update_state();
    void fit_text();

    HistoryController& history_;
    QString message_;
    QLabel* text_ = nullptr;
    QPushButton* back_ = nullptr;
    QPushButton* new_variant_ = nullptr;
    QPushButton* restore_ = nullptr;
};

/// The name of the current variant in its colour at the right end of the stage bar (task 2.6,
/// docs/interface-design.md §12: "the title and the stage bar show the name of the current
/// variant in colour, so as not to mix them up"), with a menu to switch to another variant in one
/// click, make a new one or open the history. The window shows it while the project has a folder.
///
/// Object names, for tests: "variantSwitcher"; its menu items "variantSwitcher.variant" (one per
/// variant, the name in their data), "variantSwitcher.newVariant", "variantSwitcher.history".
class VariantSwitcher : public QToolButton {
    Q_OBJECT

public:
    explicit VariantSwitcher(HistoryController& history, QWidget* parent = nullptr);
    ~VariantSwitcher() override;
    VariantSwitcher(const VariantSwitcher&) = delete;
    VariantSwitcher& operator=(const VariantSwitcher&) = delete;
    VariantSwitcher(VariantSwitcher&&) = delete;
    VariantSwitcher& operator=(VariantSwitcher&&) = delete;

    /// The menu, rebuilt before it shows.
    [[nodiscard]] QMenu& variant_menu() const noexcept { return *menu_; }
    void rebuild_menu();

Q_SIGNALS:
    /// "Open the history".
    void history_requested();

private:
    void update_state();

    HistoryController& history_;
    QMenu* menu_ = nullptr;
};

}  // namespace fem::shell
