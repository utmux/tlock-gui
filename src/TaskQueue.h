#pragma once

#include "FileTask.h"

#include <QDateTime>
#include <QObject>
#include <QVector>

class TleRunner;

struct BatchSettings {
    QString tlePath;
    TaskMode mode = TaskMode::Encrypt;
    QDateTime unlockTarget;
    bool useDefaultNetwork = true;
    QString network;
    QString chain;
};

class TaskQueue : public QObject
{
    Q_OBJECT

public:
    enum class ExistingTargetDecision {
        Overwrite,
        Skip,
        OverwriteAll,
        SkipAll,
        CancelAll
    };
    Q_ENUM(ExistingTargetDecision)

    explicit TaskQueue(QObject *parent = nullptr);
    ~TaskQueue() override;

    bool isRunning() const;
    int currentIndex() const;
    const QVector<FileTask> &tasks() const;

    void start(const QVector<FileTask> &tasks, const BatchSettings &settings);
    void cancelCurrent();
    void stopAll();
    void resolveExistingTarget(ExistingTargetDecision decision);
    void forceShutdown();

signals:
    void runningChanged(bool running);
    void taskChanged(int index, const FileTask &task);
    void currentTaskChanged(const QString &inputPath, int oneBasedIndex, int totalCount);
    void currentProgress(int taskIndex, qint64 outputBytes, int percent,
                         double bytesPerSecond, qint64 elapsedSeconds, qint64 etaSeconds);
    void totalProgress(int currentOneBasedIndex, int totalCount,
                       qint64 completedBytes, qint64 totalBytes, int percent);
    void existingTargetFound(int taskIndex, const QString &targetPath);
    void logMessage(const QString &message);
    void fatalError(const QString &message);
    void batchFinished(bool stoppedByUser);

private:
    void startNext();
    void startCurrent();
    void skipCurrent();
    void finishBatch();
    void setStatus(int index, TaskStatus status, const QString &error = {});
    void markRemainingCancelled(int firstIndex);
    void cleanTemporaryOutput(const QString &path);
    bool finalizeOutput(FileTask &task, bool overwriteAllowed, QString *errorMessage);
    void refreshTotalProgress(qint64 currentBytes = 0);
    QString friendlyFailure(const QString &diagnostic, int exitCode) const;

    QVector<FileTask> m_tasks;
    BatchSettings m_settings;
    TleRunner *m_runner = nullptr;
    int m_currentIndex = -1;
    bool m_running = false;
    bool m_waitingForExistingDecision = false;
    bool m_stopRequested = false;
    bool m_overwriteAll = false;
    bool m_skipAll = false;
    bool m_currentOverwriteAllowed = false;
    qint64 m_totalBytes = 0;
    qint64 m_successfulBytes = 0;
};

Q_DECLARE_METATYPE(TaskQueue::ExistingTargetDecision)
