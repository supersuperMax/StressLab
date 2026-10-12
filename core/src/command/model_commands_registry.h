#pragma once

namespace fem::command {
class CommandBus;
}

namespace fem::command::detail {

/// Registers the commands of `model_commands.h`; called by every CommandBus.
void register_model_commands(CommandBus& bus);

}  // namespace fem::command::detail
