#pragma once

#include <QMetaType>
#include <QString>

enum class TaskMode {
    Encrypt,
    Decrypt
};

enum class TaskStatus {
    Pending,
    Encrypting,
    Decrypting,
    Success,
    Failed,
    Skipped,
    Cancelled
};

struct FileTask {
    QString inputPath;
    QString finalOutputPath;
    QString tempOutputPath;
    QString sha256;
    QString errorMessage;
    qint64 inputSize = 0;
    qint64 outputBytes = 0;
    TaskStatus status = TaskStatus::Pending;
    int progress = 0;
};

Q_DECLARE_METATYPE(FileTask)
Q_DECLARE_METATYPE(TaskMode)
Q_DECLARE_METATYPE(TaskStatus)
