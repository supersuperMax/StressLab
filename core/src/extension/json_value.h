#pragma once

// Values of commands and context keys as JSON text: what the C ABI of native modules and the
// protocol of out-of-process modules pass.

#include "fem/core/command/value.h"
#include "fem/core/context/value.h"

#include <nlohmann/json.hpp>

namespace fem::extension::detail {

/// Paths become UTF-8 strings.
[[nodiscard]] nlohmann::json to_json(const command::Value& value);
/// Throws ExtensionError(command_failed) for integers beyond int64 and binary values.
[[nodiscard]] command::Value from_json(const nlohmann::json& json);
/// Throws ExtensionError(invalid_state) for a value a context key cannot hold.
[[nodiscard]] context::ContextValue to_context_value(const nlohmann::json& json);
[[nodiscard]] nlohmann::json to_json(const context::ContextValue& value);

}  // namespace fem::extension::detail
