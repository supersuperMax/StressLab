#pragma once

#include "fem/core/extension/runtime.h"
#include "fem/core/extension/semver.h"

#include <memory>
#include <string>

namespace fem::extension::detail {

/// Runtime of native modules: loads the library named by `entry` and activates it through the
/// C ABI of fem/extension_abi.h. A library is loaded once per process and never unloaded; a module
/// enabled again in the same session is activated again from the library already loaded.
class NativeRuntime final : public ExtensionRuntime {
public:
    explicit NativeRuntime(const Version& engine) : engine_(engine.to_string()) {}

    [[nodiscard]] std::unique_ptr<ExtensionInstance> activate(const ExtensionInfo& extension,
                                                              ExtensionHost& host) override;
    [[nodiscard]] bool keeps_code_loaded() const noexcept override { return true; }

private:
    std::string engine_;
};

}  // namespace fem::extension::detail
