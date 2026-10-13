#include "fem/shell/keys.h"

#include <gtest/gtest.h>

namespace {

using fem::shell::key_sequence;
using fem::shell::shortcut_text;

TEST(ShellKeys, ModIsCtrlOrCmd) {
    // Qt::CTRL is Cmd on macOS: one sequence for the three OSes.
    EXPECT_EQ(key_sequence("Mod+Shift+P"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P));
    EXPECT_EQ(key_sequence("mod+q"), QKeySequence(Qt::CTRL | Qt::Key_Q));
    EXPECT_EQ(key_sequence("Mod+,"), QKeySequence(Qt::CTRL | Qt::Key_Comma));
}

TEST(ShellKeys, OtherKeysAreReadAsQtNamesThem) {
    EXPECT_EQ(key_sequence("F5"), QKeySequence(Qt::Key_F5));
    EXPECT_EQ(key_sequence("Alt+F4"), QKeySequence(Qt::ALT | Qt::Key_F4));
    EXPECT_EQ(key_sequence("Shift+Delete"), QKeySequence(Qt::SHIFT | Qt::Key_Delete));
}

TEST(ShellKeys, WhatIsNotOneShortcutIsEmpty) {
    for (const char* text : {"", "Mod+", "Mod+Bogus", "Mod+Q, Mod+W"}) {
        EXPECT_TRUE(key_sequence(text).isEmpty()) << text;
    }
}

TEST(ShellKeys, TheTextIsWrittenAsTheOsWritesIt) {
    const QString text = shortcut_text(key_sequence("Mod+Shift+P"));
#ifdef Q_OS_MACOS
    EXPECT_TRUE(text.contains(QChar(0x2318))) << text.toStdString();  // ⌘
    EXPECT_FALSE(text.contains(QStringLiteral("Ctrl"))) << text.toStdString();
#else
    EXPECT_EQ(text, QStringLiteral("Ctrl+Shift+P"));
#endif
}

}  // namespace
