#pragma once

#include <QString>
#include <QStringList>

class PowerManager
{
public:
    static bool setKeepAwake(bool enabled, QString *errorMessage = nullptr);
    static bool scheduleShutdown(int delaySeconds, QString *errorMessage = nullptr);
    static bool cancelShutdown(QString *errorMessage = nullptr);

private:
    static QString shutdownProgramPath();
    static bool runShutdown(const QStringList &arguments, QString *errorMessage);
};
