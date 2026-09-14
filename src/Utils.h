#pragma once

#include "FileTask.h"

#include <QDateTime>
#include <QString>

namespace Utils {

QString formatBytes(qint64 bytes, int precision = 2);
QString formatRate(double bytesPerSecond);
QString formatDuration(qint64 seconds);
QString taskStatusText(TaskStatus status);
QString outputPathFor(const QString &inputPath, TaskMode mode,
                      bool sameDirectory, const QString &outputDirectory);
QString normalizedPathKey(const QString &path);
QString timeZoneDescription(const QDateTime &dateTime = QDateTime::currentDateTime());
bool isPathWritableDirectory(const QString &path, QString *errorMessage = nullptr);

} // namespace Utils
