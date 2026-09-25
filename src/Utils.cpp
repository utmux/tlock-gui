#include "Utils.h"

#include <QDir>
#include <QCoreApplication>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <QTimeZone>

#include <cmath>
#include <limits>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace Utils {

namespace {
constexpr qint64 quicknetGenesisTime = 1692803367LL;
constexpr qint64 quicknetPeriodSeconds = 3LL;

QString text(const char *source)
{
    return QCoreApplication::translate("Utils", source);
}
}

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
        return text("计算中...");
    return text("约 %1/s").arg(formatBytes(static_cast<qint64>(bytesPerSecond), 2));
}

QString formatDuration(qint64 seconds)
{
    if (seconds < 0)
        return text("计算中...");
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 remainingSeconds = seconds % 60;
    if (hours > 99) {
        const qint64 days = hours / 24;
        return text("%1天 %2:%3:%4")
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
    case TaskStatus::Pending: return text("等待中");
    case TaskStatus::Encrypting: return text("正在加密");
    case TaskStatus::Decrypting: return text("正在解密");
    case TaskStatus::Success: return text("成功");
    case TaskStatus::Failed: return text("失败");
    case TaskStatus::Skipped: return text("已跳过");
    case TaskStatus::Cancelled: return text("已取消");
    }
    return text("未知");
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
            *errorMessage = text("输出目录不存在：%1").arg(path);
        return false;
    }

    QTemporaryFile probe(QDir(path).filePath(QStringLiteral(".tlockgui-write-test-XXXXXX")));
    probe.setAutoRemove(true);
    if (!probe.open()) {
        if (errorMessage)
            *errorMessage = text("输出目录不可写：%1\n%2").arg(path, probe.errorString());
        return false;
    }
    return true;
}

bool parseDrandTooEarlyRounds(const QString &diagnostic, qint64 *expectedRound,
                              qint64 *currentRound)
{
    static const QRegularExpression expression(
        QStringLiteral("expected\\s+round\\s+(\\d+)\\s*>\\s*(\\d+)\\s+current\\s+round"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = expression.match(diagnostic);
    if (!match.hasMatch())
        return false;

    bool expectedOk = false;
    bool currentOk = false;
    const qint64 expected = match.captured(1).toLongLong(&expectedOk);
    const qint64 current = match.captured(2).toLongLong(&currentOk);
    if (!expectedOk || !currentOk || expected <= 0 || current <= 0 || expected <= current)
        return false;
    if (expectedRound)
        *expectedRound = expected;
    if (currentRound)
        *currentRound = current;
    return true;
}

QDateTime quicknetRoundTimeUtc(qint64 round)
{
    if (round <= 0
        || round - 1 > (std::numeric_limits<qint64>::max() - quicknetGenesisTime)
            / quicknetPeriodSeconds) {
        return {};
    }
    return QDateTime::fromSecsSinceEpoch(
        quicknetGenesisTime + (round - 1) * quicknetPeriodSeconds, QTimeZone::UTC);
}

QString quicknetChainHash()
{
    return QStringLiteral("52db9ba70e0cc0f6eaf7803dd07447a1f5477735fd3f661792ba94600c84e971");
}

StorageMediaType storageMediaTypeForPath(const QString &path, QString *volumeRoot)
{
    if (volumeRoot)
        volumeRoot->clear();
    if (path.trimmed().isEmpty())
        return StorageMediaType::Unknown;

    const QStorageInfo storage(path);
    const QString root = QDir::fromNativeSeparators(storage.rootPath());
    if (volumeRoot)
        *volumeRoot = QDir::toNativeSeparators(root);

#ifdef Q_OS_WIN
    if (!storage.isValid() || !storage.isReady() || root.size() < 2
        || !root.at(0).isLetter() || root.at(1) != QLatin1Char(':')) {
        return StorageMediaType::Unknown;
    }

    const QString devicePath = QStringLiteral("\\\\.\\") + root.left(2);
    const HANDLE volume = CreateFileW(
        reinterpret_cast<LPCWSTR>(devicePath.utf16()), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE)
        return StorageMediaType::Unknown;

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor{};
    DWORD bytesReturned = 0;
    const BOOL ok = DeviceIoControl(
        volume, IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query), &descriptor, sizeof(descriptor),
        &bytesReturned, nullptr);
    CloseHandle(volume);

    if (!ok || bytesReturned < sizeof(descriptor))
        return StorageMediaType::Unknown;
    return descriptor.IncursSeekPenalty
        ? StorageMediaType::Rotational : StorageMediaType::SolidState;
#else
    Q_UNUSED(storage);
    return StorageMediaType::Unknown;
#endif
}

} // namespace Utils
