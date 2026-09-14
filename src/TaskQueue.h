#pragma once

#include "FileTask.h"

#include <QDateTime>
#include <QHash>
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
    int maxParallelTasks = 1;
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
    int activeCount() const;
    QVector<int> activeTaskIndices() const;
    const QVector<FileTask> &tasks() const;

    void start(const QVector<FileTask> &tasks, const BatchSettings &settings);
    void cancelCurrent();
    void cancelTask(int index);
    void stopAll();
    void resolveExistingTarget(ExistingTargetDecision decision);
    void forceShutdown();

signals:
    void runningChanged(bool running);
    void taskChanged(int index, const FileTask &task);
    void currentTaskChanged(const QString &inputPath, int oneBasedIndex, int totalCount);
    void activeCountChanged(int activeCount, int parallelLimit);
    void currentProgress(int taskIndex, qint64 outputBytes, int percent,
                         double bytesPerSecond, qint64 elapsedSeconds, qint64 etaSeconds);
    void totalProgress(int currentOneBasedIndex, int totalCount,
                       qint64 completedBytes, qint64 totalBytes, int percent);
    void existingTargetFound(int taskIndex, const QString &targetPath);
    void logMessage(const QString &message);
    void fatalError(const QString &message);
    void batchFinished(bool stoppedByUser);

private:
    struct ActiveTask {
        int index = -1;
        bool overwriteAllowed = false;
    };

    void schedule();
    bool prepareTask(int index);
    void startTask(int index, bool overwriteAllowed);
    void skipTask(int index);
    void handleRunnerFinished(TleRunner *runner, bool processSucceeded, bool cancelled,
                              int exitCode, const QString &diagnostic);
    void maybeFinish();
    void finishBatch();
    void setStatus(int index, TaskStatus status, const QString &error = {});
    void markRemainingCancelled(int firstIndex);
    void cleanTemporaryOutput(const QString &path);
    bool finalizeOutput(FileTask &task, bool overwriteAllowed, QString *errorMessage);
    void refreshTotalProgress();
    QString friendlyFailure(const QString &diagnostic, int exitCode) const;

    QVector<FileTask> m_tasks;
    BatchSettings m_settings;
    QHash<TleRunner *, ActiveTask> m_activeTasks;
    QHash<int, TleRunner *> m_taskRunners;
    int m_nextIndex = 0;
    int m_lastStartedIndex = -1;
    int m_waitingDecisionIndex = -1;
    bool m_running = false;
    bool m_stopRequested = false;
    bool m_stoppedByUser = false;
    bool m_abortScheduling = false;
    bool m_overwriteAll = false;
    bool m_skipAll = false;
    qint64 m_totalBytes = 0;
    qint64 m_successfulBytes = 0;
};

Q_DECLARE_METATYPE(TaskQueue::ExistingTargetDecision)
