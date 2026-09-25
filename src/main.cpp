#include "AppTranslator.h"
#include "MainWindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QSettings>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("TLockGUI"));
    QCoreApplication::setApplicationName(QStringLiteral("TLockGUI"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.1.2"));

    AppTranslator englishTranslator;
    QSettings settings;
    if (settings.value(QStringLiteral("ui/language"), QStringLiteral("zh_CN")).toString()
        == QStringLiteral("en")) {
        application.installTranslator(&englishTranslator);
    }

    MainWindow window;
    window.show();
    return application.exec();
}
