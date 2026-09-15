#include "TaskQueue.h"

#include "TleRunner.h"
#include "Utils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <limits>

namespace {
constexpr qint64 safetyMargin = 64LL * 1024LL * 1024LL;

qint64 saturatedAdd(qint64 left, qint64 right)
{
    if (right > 0 && left > std::numeric_limits<qint64>::max() - right)
        return std::numeric_limits<qint64>::max();
    return left + right;
}

bool isTerminal(TaskStatus status)
{
    return status == TaskStatus::Success || status == TaskStatus::Failed
        || status == TaskStatus::Skipped || status == TaskStatus::Cancelled;
}
}

TaskQueue::TaskQueue(QObject *parent)
    : QObject(parent)
{
}

TaskQueue::~TaskQueue()
{
    forceShutdown();
}

bool TaskQueue::isRunning() const
{
    return m_running;
}

int TaskQueue::currentIndex() const
{
    return m_lastStartedIndex;
}

int TaskQueue::activeCount() const
{
    return m_activeTasks.size();
}

QVector<int> TaskQueue::activeTaskIndices() const
{
    QVector<int> indices;
    indices.reserve(m_taskRunners.size());
    for (auto iterator = m_taskRunners.constBegin(); iterator != m_taskRunners.constEnd(); ++iterator)
        indices.append(iterator.key());
    std::sort(indices.begin(), indices.end());
    return indices;
}

const QVector<FileTask> &TaskQueue::tasks() const
{
    return m_tasks;
}

void TaskQueue::start(const QVector<FileTask> &tasks, const BatchSettings &settings)
{
    if (m_running || tasks.isEmpty())
        return;

    m_tasks = tasks;
    m_settings = settings;
    m_settings.maxParallelTasks = qBound(1, m_settings.maxParallelTasks, 32);
    m_activeTasks.clear();
    m_taskRunners.clear();
    m_nextIndex = 0;
    m_lastStartedIndex = -1;
    m_waitingDecisionIndex = -1;
    m_stopRequested = false;
    m_stoppedByUser = false;
    m_abortScheduling = false;
    m_overwriteAll = false;
    m_skipAll = false;
    m_totalBytes = 0;
    m_successfulBytes = 0;
    for (FileTask &task : m_tasks) {
        task.status = TaskStatus::Pending;
        task.progress = 0;
        task.outputBytes = 0;
        task.errorMessage.clear();
        m_totalBytes = saturatedAdd(m_totalBytes, qMax<qint64>(0, task.inputSize));
    }

    m_running = true;
    emit runningChanged(true);
    emit activeCountChanged(0, m_settings.maxParallelTasks);
    refreshTotalProgress();
    QTimer::singleShot(0, this, &TaskQueue::schedule);
}

void TaskQueue::cancelCurrent()
{
    if (!m_running)
        return;
    if (m_taskRunners.contains(m_lastStartedIndex)) {
        cancelTask(m_lastStartedIndex);
        return;
    }
    const QVector<int> active = activeTaskIndices();
    if (!active.isEmpty()) {
        cancelTask(active.constFirst());
        return;
    }
    if (m_waitingDecisionIndex >= 0)
        cancelTask(m_waitingDecisionIndex);
}

void TaskQueue::cancelTask(int index)
{
    if (!m_running || index < 0 || index >= m_tasks.size())
        return;

    if (TleRunner *runner = m_taskRunners.value(index, nullptr)) {
        emit logMessage(tr("正在取消任务 %1；将先请求进程正常终止，2 秒后仍未退出则强制结束。")
                            .arg(index + 1));
        runner->cancel();
        return;
    }

    if (m_tasks[index].status != TaskStatus::Pending)
        return;
    if (m_waitingDecisionIndex == index)
        m_waitingDecisionIndex = -1;
    setStatus(index, TaskStatus::Cancelled, tr("用户取消了该任务。"));
    emit logMessage(tr("已取消：%1").arg(m_tasks[index].inputPath));
    refreshTotalProgress();
    QTimer::singleShot(0, this, &TaskQueue::schedule);
}

void TaskQueue::stopAll()
{
    if (!m_running)
        return;
    m_stopRequested = true;
    m_stoppedByUser = true;
    m_abortScheduling = true;

    if (m_waitingDecisionIndex >= 0) {
        setStatus(m_waitingDecisionIndex, TaskStatus::Cancelled, tr("用户停止了批处理。"));
        m_waitingDecisionIndex = -1;
    }
    markRemainingCancelled(0);

    const QList<TleRunner *> runners = m_activeTasks.keys();
    if (!runners.isEmpty())
        emit logMessage(tr("正在停止全部 %1 个运行中的任务。").arg(runners.size()));
    for (TleRunner *runner : runners)
        runner->cancel();
    maybeFinish();
}

void TaskQueue::resolveExistingTarget(ExistingTargetDecision decision)
{
    if (!m_running || m_waitingDecisionIndex < 0)
        return;
    const int index = m_waitingDecisionIndex;
    m_waitingDecisionIndex = -1;

    switch (decision) {
    case ExistingTargetDecision::OverwriteAll:
        m_overwriteAll = true;
        startTask(index, true);
        break;
    case ExistingTargetDecision::Overwrite:
        startTask(index, true);
        break;
    case ExistingTargetDecision::SkipAll:
        m_skipAll = true;
        skipTask(index);
        break;
    case ExistingTargetDecision::Skip:
        skipTask(index);
        break;
    case ExistingTargetDecision::CancelAll:
        setStatus(index, TaskStatus::Cancelled, tr("用户取消了批处理。"));
        stopAll();
        return;
    }
    refreshTotalProgress();
    QTimer::singleShot(0, this, &TaskQueue::schedule);
}

void TaskQueue::forceShutdown()
{
    if (!m_running && m_activeTasks.isEmpty())
        return;

    m_stopRequested = true;
    m_stoppedByUser = true;
    m_abortScheduling = true;
    if (m_waitingDecisionIndex >= 0) {
        setStatus(m_waitingDecisionIndex, TaskStatus::Cancelled, tr("程序退出时任务被终止。"));
        m_waitingDecisionIndex = -1;
    }

    const QList<TleRunner *> runners = m_activeTasks.keys();
    for (TleRunner *runner : runners) {
        const ActiveTask active = m_activeTasks.value(runner);
        runner->forceStopAndWait();
        if (active.index >= 0 && active.index < m_tasks.size()) {
            FileTask &task = m_tasks[active.index];
            cleanTemporaryOutput(task.tempOutputPath);
            if (!isTerminal(task.status))
                setStatus(active.index, TaskStatus::Cancelled, tr("程序退出时任务被终止。"));
        }
        runner->deleteLater();
    }
    m_activeTasks.clear();
    m_taskRunners.clear();
    emit activeCountChanged(0, m_settings.maxParallelTasks);
    markRemainingCancelled(0);
    if (m_running)
        finishBatch();
}

void TaskQueue::schedule()
{
    if (!m_running)
        return;
    if (m_stopRequested || m_abortScheduling) {
        maybeFinish();
        return;
    }

    while (m_running && !m_stopRequested && !m_abortScheduling
           && m_waitingDecisionIndex < 0
           && m_activeTasks.size() < m_settings.maxParallelTasks
           && m_nextIndex < m_tasks.size()) {
        const int index = m_nextIndex++;
        if (m_tasks[index].status != TaskStatus::Pending)
            continue;
        if (!prepareTask(index))
            break;
    }
    maybeFinish();
}

bool TaskQueue::prepareTask(int index)
{
    FileTask &task = m_tasks[index];
    const QFileInfo inputInfo(task.inputPath);
    if (!inputInfo.exists() || !inputInfo.isFile()) {
        setStatus(index, TaskStatus::Failed,
                  tr("输入文件不存在或不是普通文件：%1").arg(task.inputPath));
        return true;
    }
    if (inputInfo.size() <= 0) {
        setStatus(index, TaskStatus::Failed,
                  tr("输入文件为空，未启动 tle.exe：%1").arg(task.inputPath));
        return true;
    }
    if (inputInfo.size() != task.inputSize) {
        m_totalBytes -= task.inputSize;
        task.inputSize = inputInfo.size();
        m_totalBytes = saturatedAdd(m_totalBytes, task.inputSize);
        emit taskChanged(index, task);
    }

    if (m_settings.mode == TaskMode::Encrypt
        && QDateTime::currentDateTime().secsTo(m_settings.unlockTarget) <= 0) {
        const QString message = tr("目标解锁时间已经到达或过去，无法继续生成时间锁文件。");
        setStatus(index, TaskStatus::Failed, message);
        markRemainingCancelled(m_nextIndex);
        m_abortScheduling = true;
        emit fatalError(message);
        return false;
    }

    const QString outputDirectory = QFileInfo(task.finalOutputPath).absolutePath();
    QString writableError;
    if (!Utils::isPathWritableDirectory(outputDirectory, &writableError)) {
        setStatus(index, TaskStatus::Failed, writableError);
        return true;
    }

    const QStorageInfo storage(outputDirectory);
    if (storage.isValid() && storage.isReady()
        && storage.bytesAvailable() <= saturatedAdd(task.inputSize, safetyMargin)) {
        const QString message = tr("输出磁盘空间不足。至少需要 %1，可用 %2。")
            .arg(Utils::formatBytes(saturatedAdd(task.inputSize, safetyMargin)),
                 Utils::formatBytes(storage.bytesAvailable()));
        setStatus(index, TaskStatus::Failed, message);
        return true;
    }

    if (QFileInfo::exists(task.finalOutputPath)) {
        if (m_skipAll) {
            skipTask(index);
            return true;
        }
        if (m_overwriteAll) {
            startTask(index, true);
            return true;
        }
        m_waitingDecisionIndex = index;
        emit existingTargetFound(index, task.finalOutputPath);
        return false;
    }

    startTask(index, false);
    return true;
}

void TaskQueue::startTask(int index, bool overwriteAllowed)
{
    if (!m_running || m_stopRequested || m_abortScheduling
        || index < 0 || index >= m_tasks.size()
        || m_tasks[index].status != TaskStatus::Pending) {
        return;
    }

    FileTask &task = m_tasks[index];
    if (QFileInfo::exists(task.tempOutputPath)) {
        setStatus(index, TaskStatus::Failed,
                  tr("临时输出路径已存在。为保护现有数据，未覆盖该文件：%1\n"
                     "请确认后手动移动或删除它，再重试。")
                      .arg(task.tempOutputPath));
        return;
    }

    QStringList arguments;
    if (m_settings.mode == TaskMode::Encrypt) {
        const qint64 seconds = QDateTime::currentDateTime().secsTo(m_settings.unlockTarget);
        if (seconds <= 0) {
            const QString message = tr("目标解锁时间已经到达或过去，无法继续生成时间锁文件。");
            setStatus(index, TaskStatus::Failed, message);
            markRemainingCancelled(m_nextIndex);
            m_abortScheduling = true;
            emit fatalError(message);
            return;
        }
        arguments << QStringLiteral("-e")
                  << QStringLiteral("-D")
                  << QString::number(seconds) + QStringLiteral("s");
        setStatus(index, TaskStatus::Encrypting);
        emit logMessage(tr("开始加密 [%1/%2]：%3").arg(index + 1).arg(m_tasks.size()).arg(task.inputPath));
        emit logMessage(tr("解锁目标：%1\n剩余：%2 秒")
                            .arg(m_settings.unlockTarget.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                            .arg(seconds));
    } else {
        arguments << QStringLiteral("-d");
        setStatus(index, TaskStatus::Decrypting);
        emit logMessage(tr("开始解密 [%1/%2]：%3").arg(index + 1).arg(m_tasks.size()).arg(task.inputPath));
    }

    if (!m_settings.useDefaultNetwork) {
        arguments << QStringLiteral("-n") << m_settings.network
                  << QStringLiteral("-c") << m_settings.chain;
    }
    arguments << QStringLiteral("-o") << task.tempOutputPath << task.inputPath;

    auto *runner = new TleRunner(this);
    m_activeTasks.insert(runner, {index, overwriteAllowed});
    m_taskRunners.insert(index, runner);
    m_lastStartedIndex = index;

    connect(runner, &TleRunner::outputReceived, this,
            [this, runner](const QString &text, bool standardError) {
        if (!m_activeTasks.contains(runner))
            return;
        const int taskIndex = m_activeTasks.value(runner).index;
        const QString channel = standardError ? QStringLiteral("stderr") : QStringLiteral("stdout");
        const QString trimmed = text.trimmed();
        if (!trimmed.isEmpty())
            emit logMessage(QStringLiteral("[%1/%2][%3]\n%4")
                                .arg(taskIndex + 1).arg(m_tasks.size()).arg(channel, trimmed));
    });
    connect(runner, &TleRunner::progress, this,
            [this, runner](qint64 bytes, int percent, double speed, qint64 elapsed, qint64 eta) {
        if (!m_running || !m_activeTasks.contains(runner))
            return;
        const int taskIndex = m_activeTasks.value(runner).index;
        FileTask &runningTask = m_tasks[taskIndex];
        runningTask.outputBytes = bytes;
        runningTask.progress = percent;
        emit taskChanged(taskIndex, runningTask);
        emit currentProgress(taskIndex, bytes, percent, speed, elapsed, eta);
        refreshTotalProgress();
    });
    connect(runner, &TleRunner::finished, this,
            [this, runner](bool succeeded, bool cancelled, int exitCode, const QString &diagnostic) {
        handleRunnerFinished(runner, succeeded, cancelled, exitCode, diagnostic);
    });

    emit currentTaskChanged(task.inputPath, index + 1, m_tasks.size());
    emit activeCountChanged(m_activeTasks.size(), m_settings.maxParallelTasks);
    emit logMessage(tr("程序：\n%1\n参数：\n%2")
                        .arg(m_settings.tlePath, arguments.join(QLatin1Char('\n'))));
    runner->start(m_settings.tlePath, arguments, task.tempOutputPath, task.inputSize);
}

void TaskQueue::skipTask(int index)
{
    if (index < 0 || index >= m_tasks.size())
        return;
    setStatus(index, TaskStatus::Skipped, tr("目标文件已存在，已按用户选择跳过。"));
    emit logMessage(tr("已跳过：%1").arg(m_tasks[index].inputPath));
}

void TaskQueue::handleRunnerFinished(TleRunner *runner, bool processSucceeded,
                                     bool cancelled, int exitCode, const QString &diagnostic)
{
    if (!m_activeTasks.contains(runner))
        return;
    const ActiveTask active = m_activeTasks.take(runner);
    m_taskRunners.remove(active.index);
    emit activeCountChanged(m_activeTasks.size(), m_settings.maxParallelTasks);

    FileTask &task = m_tasks[active.index];
    emit logMessage(tr("tle.exe [%1/%2] 已退出，退出代码：%3")
                        .arg(active.index + 1).arg(m_tasks.size()).arg(exitCode));
    if (!diagnostic.isEmpty())
        emit logMessage(tr("进程诊断信息：\n%1").arg(diagnostic));

    if (cancelled) {
        cleanTemporaryOutput(task.tempOutputPath);
        setStatus(active.index, TaskStatus::Cancelled,
                  m_stopRequested ? tr("用户停止了批处理。") : tr("用户取消了该任务。"));
        emit logMessage(tr("已取消：%1").arg(task.inputPath));
    } else if (processSucceeded) {
        QString finalizeError;
        if (finalizeOutput(task, active.overwriteAllowed, &finalizeError)) {
            task.progress = 100;
            task.outputBytes = QFileInfo(task.finalOutputPath).size();
            task.status = TaskStatus::Success;
            task.errorMessage.clear();
            m_successfulBytes = saturatedAdd(m_successfulBytes, task.inputSize);
            emit taskChanged(active.index, task);
            emit currentProgress(active.index, task.outputBytes, 100, 0.0, 0, 0);
            emit logMessage(tr("成功：%1\n输出：%2").arg(task.inputPath, task.finalOutputPath));
        } else {
            cleanTemporaryOutput(task.tempOutputPath);
            setStatus(active.index, TaskStatus::Failed, finalizeError);
            emit logMessage(tr("安全保存失败：%1").arg(finalizeError));
        }
    } else {
        cleanTemporaryOutput(task.tempOutputPath);
        const QString failure = friendlyFailure(diagnostic, exitCode);
        setStatus(active.index, TaskStatus::Failed, failure);
        emit logMessage(tr("失败：%1").arg(failure));
    }

    runner->deleteLater();
    refreshTotalProgress();
    QTimer::singleShot(0, this, &TaskQueue::schedule);
}

void TaskQueue::maybeFinish()
{
    if (!m_running || !m_activeTasks.isEmpty() || m_waitingDecisionIndex >= 0)
        return;
    if (m_stopRequested || m_abortScheduling || m_nextIndex >= m_tasks.size())
        finishBatch();
}

void TaskQueue::finishBatch()
{
    if (!m_running || !m_activeTasks.isEmpty())
        return;
    m_running = false;
    m_waitingDecisionIndex = -1;
    refreshTotalProgress();
    emit activeCountChanged(0, m_settings.maxParallelTasks);
    emit runningChanged(false);
    emit batchFinished(m_stoppedByUser);
}

void TaskQueue::setStatus(int index, TaskStatus status, const QString &error)
{
    if (index < 0 || index >= m_tasks.size())
        return;
    FileTask &task = m_tasks[index];
    task.status = status;
    task.errorMessage = error;
    emit taskChanged(index, task);
}

void TaskQueue::markRemainingCancelled(int firstIndex)
{
    for (int index = qMax(0, firstIndex); index < m_tasks.size(); ++index) {
        if (m_tasks[index].status == TaskStatus::Pending)
            setStatus(index, TaskStatus::Cancelled, tr("批处理已停止。"));
    }
}

void TaskQueue::cleanTemporaryOutput(const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return;
    if (!QFile::remove(path))
        emit logMessage(tr("警告：无法删除临时文件，请手动处理：%1").arg(path));
}

bool TaskQueue::finalizeOutput(FileTask &task, bool overwriteAllowed, QString *errorMessage)
{
    const QFileInfo tempInfo(task.tempOutputPath);
    if (!tempInfo.exists() || !tempInfo.isFile() || tempInfo.size() <= 0) {
        *errorMessage = tr("临时输出不存在或为空：%1").arg(task.tempOutputPath);
        return false;
    }

    const bool targetExists = QFileInfo::exists(task.finalOutputPath);
    if (targetExists && !overwriteAllowed) {
        *errorMessage = tr("处理期间目标文件出现，为保护数据未覆盖：%1").arg(task.finalOutputPath);
        return false;
    }

    if (!targetExists) {
        if (!QFile::rename(task.tempOutputPath, task.finalOutputPath)) {
            *errorMessage = tr("无法将临时文件重命名为最终文件：%1").arg(task.finalOutputPath);
            return false;
        }
        return true;
    }

    const QString backupPath = task.finalOutputPath
        + QStringLiteral(".tlockgui-backup-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QFile::rename(task.finalOutputPath, backupPath)) {
        *errorMessage = tr("无法暂存已有目标文件，原文件保持不变：%1").arg(task.finalOutputPath);
        return false;
    }

    if (!QFile::rename(task.tempOutputPath, task.finalOutputPath)) {
        const bool restored = QFile::rename(backupPath, task.finalOutputPath);
        *errorMessage = restored
            ? tr("新文件重命名失败；已有目标文件已恢复。")
            : tr("新文件重命名失败，并且旧文件恢复失败。旧文件保存在：%1").arg(backupPath);
        return false;
    }

    if (!QFile::remove(backupPath))
        emit logMessage(tr("警告：新文件已保存，但旧文件备份未能删除：%1").arg(backupPath));
    return true;
}

void TaskQueue::refreshTotalProgress()
{
    qint64 estimatedCompleted = m_successfulBytes;
    for (auto iterator = m_activeTasks.constBegin(); iterator != m_activeTasks.constEnd(); ++iterator) {
        const FileTask &task = m_tasks[iterator.value().index];
        estimatedCompleted = saturatedAdd(
            estimatedCompleted, qMin(task.inputSize, qMax<qint64>(0, task.outputBytes)));
    }
    int completedCount = 0;
    for (const FileTask &task : m_tasks)
        completedCount += isTerminal(task.status) ? 1 : 0;
    const int percent = m_totalBytes > 0
        ? qBound(0, static_cast<int>((static_cast<double>(estimatedCompleted)
                                      / static_cast<double>(m_totalBytes)) * 100.0), 100)
        : 0;
    emit totalProgress(completedCount, m_tasks.size(), m_successfulBytes, m_totalBytes, percent);
}

QString TaskQueue::friendlyFailure(const QString &diagnostic, int exitCode) const
{
    const QString lower = diagnostic.toLower();
    if (lower.contains(QStringLiteral("too early"))) {
        qint64 expectedRound = 0;
        qint64 currentRound = 0;
        if (Utils::parseDrandTooEarlyRounds(diagnostic, &expectedRound, &currentRound)) {
            const qint64 remainingRounds = expectedRound - currentRound;
            const bool knownQuicknet = m_settings.useDefaultNetwork
                || m_settings.chain.trimmed().compare(Utils::quicknetChainHash(), Qt::CaseInsensitive) == 0;
            if (knownQuicknet) {
                const QDateTime unlockTime = Utils::quicknetRoundTimeUtc(expectedRound).toLocalTime();
                const qint64 remainingSeconds = remainingRounds > std::numeric_limits<qint64>::max() / 3
                    ? std::numeric_limits<qint64>::max() : remainingRounds * 3;
                if (unlockTime.isValid()) {
                    return tr("尚未到达解锁轮次。预计可解密时间：%1（%2）；按当前轮次估计还需约 %3。"
                              "目标轮次：%4，当前轮次：%5。drand 节点可能有数秒延迟，请到时稍后重试。")
                        .arg(unlockTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                        .arg(Utils::timeZoneDescription(unlockTime))
                        .arg(Utils::formatDuration(remainingSeconds))
                        .arg(expectedRound)
                        .arg(currentRound);
                }
            }
            return tr("尚未到达解锁轮次。目标轮次：%1，当前轮次：%2，还差 %3 轮。"
                      "当前使用的网络周期未知，无法准确换算本地解密时间。")
                .arg(expectedRound).arg(currentRound).arg(remainingRounds);
        }
        return tr("尚未到达解锁时间，tle.exe 未返回可换算的目标轮次。请稍后重试。退出代码：%1")
            .arg(exitCode);
    }
    if (lower.contains(QStringLiteral("no space"))
        || lower.contains(QStringLiteral("disk full"))
        || lower.contains(QStringLiteral("not enough space"))) {
        return tr("输出磁盘可能已满。tle.exe 退出代码：%1").arg(exitCode);
    }
    if (lower.contains(QStringLiteral("network"))
        || lower.contains(QStringLiteral("connect"))
        || lower.contains(QStringLiteral("request"))) {
        return tr("无法访问 drand 网络，请检查网络连接和高级网络设置。tle.exe 退出代码：%1")
            .arg(exitCode);
    }
    if (!diagnostic.trimmed().isEmpty())
        return tr("tle.exe 执行失败（退出代码 %1）：%2").arg(exitCode).arg(diagnostic.trimmed());
    return tr("tle.exe 执行失败，退出代码：%1").arg(exitCode);
}
