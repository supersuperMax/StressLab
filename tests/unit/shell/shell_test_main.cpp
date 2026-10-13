// Tests of the shell: one QApplication for all of them, on the platform of Qt without a screen
// unless QT_QPA_PLATFORM names another, so that they run the same on every OS and in CI.

#include <gtest/gtest.h>

#include <QApplication>
#include <QtGlobal>

int main(int argc, char** argv) {
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
