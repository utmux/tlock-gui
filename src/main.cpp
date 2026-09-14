#include "MainWindow.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("TLockGUI"));
    QCoreApplication::setApplicationName(QStringLiteral("TLockGUI"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    MainWindow window;
    window.show();
    return application.exec();
}
