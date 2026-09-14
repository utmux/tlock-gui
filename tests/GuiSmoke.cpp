#include "MainWindow.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("TLockGUI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("TLockGUI-Smoke"));
    application.setProperty("tlockgui.smokeTest", true);

    MainWindow window;
    window.show();
    application.processEvents();
    window.close();
    application.processEvents();
    return 0;
}
