#pragma once

#include "fem/core/command/command.h"
#include "fem/core/command/command_bus.h"
#include "fem/core/extension/contributions.h"
#include "fem/core/extension/manager.h"
#include "fem/core/extension/registry.h"

#include <filesystem>

namespace fem::extension {

/// The `model-import` file format of an enabled module that reads `file` (by its extension,
/// without ASCII case) and has a command; nullptr if none. With several, the first by module id.
[[nodiscard]] const FileFormatContribution* find_import_format(const ContributionRegistry& registry,
                                                               const std::filesystem::path& file);

/// Reads a model file into the document, as "File → Import" and scripts do: activates the modules
/// waiting for `onOpenFile:` of this file, then runs the command of its import format with
/// `path = file` (that command is what the journal gets). Throws ExtensionError(unsupported_file)
/// if no enabled module reads such files, and whatever the command throws.
command::CommandResult open_model_file(ExtensionManager& manager, command::CommandBus& bus,
                                       const std::filesystem::path& file);

}  // namespace fem::extension
