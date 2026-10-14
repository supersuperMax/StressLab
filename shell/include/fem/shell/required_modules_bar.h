#pragma once

#include "fem/shell/workbench.h"

#include <QFrame>
#include <QString>
#include <functional>
#include <string>
#include <vector>

class QLabel;
class QPushButton;

namespace fem::shell {

/// The bar over the 3D view that says which modules the model needs when they are not enabled
/// (docs/interface-design.md §6, "Profiles"): "The model needs the modules Dynamics, Thermal
/// analysis. [Enable] [Open without them]". Their objects stay in the model as they are and are
/// hidden until the modules are enabled; modules that are not installed are named apart and cannot
/// be enabled from here.
///
/// The bar looks at the model only when asked (`check`): when a model is opened, not when the
/// user disables a module in the module manager.
///
/// Object names, for tests: "requiredModules", "requiredModules.text", "requiredModules.enable",
/// "requiredModules.dismiss".
class RequiredModulesBar : public QFrame {
    Q_OBJECT

public:
    explicit RequiredModulesBar(Workbench& workbench, QWidget* parent = nullptr);
    ~RequiredModulesBar() override;
    RequiredModulesBar(const RequiredModulesBar&) = delete;
    RequiredModulesBar& operator=(const RequiredModulesBar&) = delete;
    RequiredModulesBar(RequiredModulesBar&&) = delete;
    RequiredModulesBar& operator=(RequiredModulesBar&&) = delete;

    /// Shows the bar if the model needs modules that are not enabled, hides it otherwise.
    void check();
    /// "Enable": enables the modules that can be; the bar stays only for what is still missing.
    void enable_modules();
    /// "Open without them": hides the bar until the next `check`.
    void dismiss();
    /// The ids the bar names now, the installed ones first.
    [[nodiscard]] const std::vector<std::string>& modules() const noexcept { return modules_; }
    [[nodiscard]] QString message() const;
    /// Where messages for the user go (the prompt line of the window).
    void set_report(std::function<void(const QString&)> report) { report_ = std::move(report); }

private:
    void retranslate();

    Workbench& workbench_;
    QLabel* text_ = nullptr;
    QPushButton* enable_ = nullptr;
    QPushButton* dismiss_ = nullptr;
    std::vector<std::string> modules_;
    std::function<void(const QString&)> report_;
    Subscription language_;
};

}  // namespace fem::shell
