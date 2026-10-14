// Tests of the shell: one QApplication for all of them, on the platform of Qt without a screen
// unless QT_QPA_PLATFORM names another, so that they run the same on every OS and in CI.

#include <gtest/gtest.h>

#include <QApplication>
#include <QtGlobal>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

int main(int argc, char** argv) {
#if defined(_MSC_VER) && defined(_DEBUG)
    // A failed check of the debug runtime goes to standard error, not to a message box that ctest
    // never shows.
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
