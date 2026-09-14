#include "Utils.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QTimeZone>

#include <cmath>

namespace Utils {

QString formatBytes(qint64 bytes, int precision)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    constexpr int unitCount = sizeof(units) / sizeof(units[0]);

    double value = static_cast<double>(qMax<qint64>(0, bytes));
    int unit = 0;
    while (value >= 1024.0 && unit < unitCount - 1) {
        value /= 1024.0;
        ++unit;
    }

    if (unit == 0)
        return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2").arg(value, 0, 'f', precision).arg(QString::fromLatin1(units[unit]));
}

QString formatRate(double bytesPerSecond)
{
    if (!std::isfinite(bytesPerSecond) || bytesPerSecond < 0.0)
        return QStringLiteral("计算中...");
    return QStringLiteral("约 %1/s").arg(formatBytes(static_cast<qint64>(bytesPerSecond), 2));
}

QString formatDuration(qint64 seconds)
{
    if (seconds < 0)
        return QStringLiteral("计算中...");
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 remainingSeconds = seconds % 60;
    if (hours > 99) {
        const qint64 days = hours / 24;
        return QStringLiteral("%1天 %2:%3:%4")
            .arg(days)
            .arg(hours % 24, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(remainingSeconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(remainingSeconds, 2, 10, QLatin1Char('0'));
}

QString taskStatusText(TaskStatus status)
{
    switch (status) {
    case TaskStatus::Pending: return QStringLiteral("等待中");
    case TaskStatus::Encrypting: return QStringLiteral("正在加密");
    case TaskStatus::Decrypting: return QStringLiteral("正在解密");
    case TaskStatus::Success: return QStringLiteral("成功");
    case TaskStatus::Failed: return QStringLiteral("失败");
    case TaskStatus::Skipped: return QStringLiteral("已跳过");
    case TaskStatus::Cancelled: return QStringLiteral("已取消");
    }
    return QStringLiteral("未知");
}

QString outputPathFor(const QString &inputPath, TaskMode mode,
                      bool sameDirectory, const QString &outputDirectory)
{
    const QFileInfo inputInfo(inputPath);
    QString outputName;
    if (mode == TaskMode::Encrypt) {
        outputName = inputInfo.fileName() + QStringLiteral(".tle");
    } else if (inputInfo.fileName().endsWith(QStringLiteral(".tle"), Qt::CaseInsensitive)) {
        outputName = inputInfo.fileName().left(inputInfo.fileName().size() - 4);
        if (outputName.isEmpty())
            outputName = inputInfo.fileName() + QStringLiteral(".decrypted");
    } else {
        outputName = inputInfo.fileName() + QStringLiteral(".decrypted");
    }

    const QString directory = sameDirectory ? inputInfo.absolutePath() : outputDirectory;
    return QDir(directory).absoluteFilePath(outputName);
}

QString normalizedPathKey(const QString &path)
{
    QString key = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}

QString timeZoneDescription(const QDateTime &dateTime)
{
    const int offset = dateTime.offsetFromUtc();
    const QChar sign = offset >= 0 ? QLatin1Char('+') : QLatin1Char('-');
    const int absolute = qAbs(offset);
    const int hours = absolute / 3600;
    const int minutes = (absolute % 3600) / 60;
    const QString zoneName = dateTime.timeZone().displayName(dateTime, QTimeZone::ShortName);
    return QStringLiteral("UTC%1%2:%3 (%4)")
        .arg(sign)
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(zoneName);
}

bool isPathWritableDirectory(const QString &path, QString *errorMessage)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("输出目录不存在：%1").arg(path);
        return false;
    }

    QTemporaryFile probe(QDir(path).filePath(QStringLiteral(".tlockgui-write-test-XXXXXX")));
    probe.setAutoRemove(true);
    if (!probe.open()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("输出目录不可写：%1\n%2").arg(path, probe.errorString());
        return false;
    }
    return true;
}

} // namespace Utils
