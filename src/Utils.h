#pragma once

#include "FileTask.h"

#include <QDateTime>
#include <QString>

namespace Utils {

enum class StorageMediaType {
    Unknown,
    SolidState,
    Rotational
};

QString formatBytes(qint64 bytes, int precision = 2);
QString formatRate(double bytesPerSecond);
QString formatDuration(qint64 seconds);
QString taskStatusText(TaskStatus status);
QString outputPathFor(const QString &inputPath, TaskMode mode,
                      bool sameDirectory, const QString &outputDirectory);
QString normalizedPathKey(const QString &path);
QString timeZoneDescription(const QDateTime &dateTime = QDateTime::currentDateTime());
bool isPathWritableDirectory(const QString &path, QString *errorMessage = nullptr);
bool parseDrandTooEarlyRounds(const QString &diagnostic, qint64 *expectedRound,
                              qint64 *currentRound);
QDateTime quicknetRoundTimeUtc(qint64 round);
QString quicknetChainHash();
StorageMediaType storageMediaTypeForPath(const QString &path, QString *volumeRoot = nullptr);

} // namespace Utils
