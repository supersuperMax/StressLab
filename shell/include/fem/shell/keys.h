#pragma once

#include <QKeySequence>
#include <QString>
#include <string_view>

namespace fem::shell {

/// A shortcut written as manifests write them (`keybindings`): modifiers and a key joined by
/// '+', e.g. "Mod+Shift+P", "Alt+F4", "F5". `Mod` is Ctrl on Windows and Linux and Cmd on macOS;
/// `Ctrl`, `Shift`, `Alt` and `Meta` are the keys of these names as Qt reads them. An empty
/// sequence if the text is not one shortcut.
[[nodiscard]] QKeySequence key_sequence(std::string_view text);

/// The text of a shortcut for menus and tooltips as this OS writes it: "Ctrl+Shift+P" on Windows
/// and Linux, "⇧⌘P" on macOS.
[[nodiscard]] QString shortcut_text(const QKeySequence& sequence);

}  // namespace fem::shell
