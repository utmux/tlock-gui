#include "PowerManager.h"

#include <QDir>
#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

namespace {
QString powerText(const char *source)
{
    return QCoreApplication::translate("PowerManager", source);
}
}
#include <windows.h>
#endif

bool PowerManager::setKeepAwake(bool enabled, QString *errorMessage)
{
#ifdef Q_OS_WIN
    const EXECUTION_STATE flags = enabled
        ? static_cast<EXECUTION_STATE>(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)
        : ES_CONTINUOUS;
    if (SetThreadExecutionState(flags) != 0)
        return true;

    if (errorMessage) {
        *errorMessage = powerText("Windows 拒绝更新电源请求，错误代码：%1")
            .arg(static_cast<qulonglong>(GetLastError()));
    }
    return false;
#else
    Q_UNUSED(enabled);
    if (errorMessage)
        *errorMessage = powerText("阻止自动睡眠仅支持 Windows。");
    return false;
#endif
}
bool PowerManager::scheduleShutdown(int delaySeconds, QString *errorMessage)
{
    const int safeDelay = qBound(0, delaySeconds, 315360000);
    return runShutdown({
        QStringLiteral("/s"),
        QStringLiteral("/t"),
        QString::number(safeDelay),
        QStringLiteral("/c"),
        powerText("TLockGUI 批处理已成功完成。若需取消，请在 TLockGUI 中点击“取消计划关机”。")
    }, errorMessage);
}

bool PowerManager::cancelShutdown(QString *errorMessage)
{
    return runShutdown({QStringLiteral("/a")}, errorMessage);
}

QString PowerManager::shutdownProgramPath()
{
    const QString systemRoot = qEnvironmentVariable("SystemRoot");
    if (!systemRoot.isEmpty()) {
        const QString candidate = QDir(systemRoot).filePath(QStringLiteral("System32/shutdown.exe"));
        if (QFileInfo(candidate).isFile())
            return candidate;
    }
    return QStandardPaths::findExecutable(QStringLiteral("shutdown.exe"));
}

bool PowerManager::runShutdown(const QStringList &arguments, QString *errorMessage)
{
#ifdef Q_OS_WIN
    const QString program = shutdownProgramPath();
    if (program.isEmpty()) {
        if (errorMessage)
            *errorMessage = powerText("找不到 Windows shutdown.exe。");
        return false;
    }

    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(3000)) {
        if (errorMessage)
            *errorMessage = powerText("无法启动 shutdown.exe：%1").arg(process.errorString());
        return false;
    }
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(2000);
        if (errorMessage)
            *errorMessage = powerText("shutdown.exe 未在预期时间内返回。");
        return false;
    }
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
        return true;

    QString diagnostic = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    if (diagnostic.isEmpty())
        diagnostic = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
    if (errorMessage) {
        *errorMessage = diagnostic.isEmpty()
            ? powerText("shutdown.exe 执行失败，退出代码：%1").arg(process.exitCode())
            : powerText("shutdown.exe 执行失败：%1").arg(diagnostic);
    }
    return false;
#else
    Q_UNUSED(arguments);
    if (errorMessage)
        *errorMessage = powerText("自动关机仅支持 Windows。");
    return false;
#endif
}
