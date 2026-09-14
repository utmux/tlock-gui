#include "TaskQueue.h"

#include "TleRunner.h"
#include "Utils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTimer>
#include <QUuid>

#include <limits>

namespace {
constexpr qint64 safetyMargin = 64LL * 1024LL * 1024LL;

qint64 saturatedAdd(qint64 left, qint64 right)
{
    if (right > 0 && left > std::numeric_limits<qint64>::max() - right)
        return std::numeric_limits<qint64>::max();
    return left + right;
}
}

TaskQueue::TaskQueue(QObject *parent)
    : QObject(parent)
    , m_runner(new TleRunner(this))
{
    connect(m_runner, &TleRunner::outputReceived, this,
            [this](const QString &text, bool standardError) {
        const QString channel = standardError ? QStringLiteral("stderr") : QStringLiteral("stdout");
        const QString trimmed = text.trimmed();
        if (!trimmed.isEmpty())
            emit logMessage(QStringLiteral("[%1]\n%2").arg(channel, trimmed));
    });

    connect(m_runner, &TleRunner::progress, this,
            [this](qint64 bytes, int percent, double speed, qint64 elapsed, qint64 eta) {
        if (!m_running || m_currentIndex < 0 || m_currentIndex >= m_tasks.size())
            return;
        FileTask &task = m_tasks[m_currentIndex];
        task.outputBytes = bytes;
        task.progress = percent;
        emit taskChanged(m_currentIndex, task);
        emit currentProgress(m_currentIndex, bytes, percent, speed, elapsed, eta);
        refreshTotalProgress(bytes);
    });

    connect(m_runner, &TleRunner::finished, this,
            [this](bool processSucceeded, bool cancelled, int exitCode, const QString &diagnostic) {
        if (!m_running || m_currentIndex < 0 || m_currentIndex >= m_tasks.size())
            return;

        FileTask &task = m_tasks[m_currentIndex];
        emit logMessage(QStringLiteral("tle.exe 已退出，退出代码：%1").arg(exitCode));
        if (!diagnostic.isEmpty())
            emit logMessage(QStringLiteral("进程诊断信息：\n%1").arg(diagnostic));

        if (cancelled) {
            cleanTemporaryOutput(task.tempOutputPath);
            setStatus(m_currentIndex, TaskStatus::Cancelled, QStringLiteral("用户取消了当前任务。"));
            emit logMessage(QStringLiteral("已取消：%1").arg(task.inputPath));
        } else if (processSucceeded) {
            QString finalizeError;
            if (finalizeOutput(task, m_currentOverwriteAllowed, &finalizeError)) {
                task.progress = 100;
                task.outputBytes = QFileInfo(task.finalOutputPath).size();
                task.status = TaskStatus::Success;
                task.errorMessage.clear();
                m_successfulBytes = saturatedAdd(m_successfulBytes, task.inputSize);
                emit taskChanged(m_currentIndex, task);
                emit currentProgress(m_currentIndex, task.outputBytes, 100, 0.0,
                                     0, 0);
                emit logMessage(QStringLiteral("成功：%1\n输出：%2")
                                    .arg(task.inputPath, task.finalOutputPath));
            } else {
                cleanTemporaryOutput(task.tempOutputPath);
                setStatus(m_currentIndex, TaskStatus::Failed, finalizeError);
                emit logMessage(QStringLiteral("安全保存失败：%1").arg(finalizeError));
            }
        } else {
            cleanTemporaryOutput(task.tempOutputPath);
            const QString failure = friendlyFailure(diagnostic, exitCode);
            setStatus(m_currentIndex, TaskStatus::Failed, failure);
            emit logMessage(QStringLiteral("失败：%1").arg(failure));
        }

        refreshTotalProgress();
        m_currentOverwriteAllowed = false;
        if (m_stopRequested) {
            finishBatch();
            return;
        }
        QTimer::singleShot(0, this, &TaskQueue::startNext);
    });
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
    return m_currentIndex;
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
    m_currentIndex = -1;
    m_waitingForExistingDecision = false;
    m_stopRequested = false;
    m_overwriteAll = false;
    m_skipAll = false;
    m_currentOverwriteAllowed = false;
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
    refreshTotalProgress();
    QTimer::singleShot(0, this, &TaskQueue::startNext);
}

void TaskQueue::cancelCurrent()
{
    if (!m_running)
        return;
    if (m_waitingForExistingDecision) {
        skipCurrent();
        return;
    }
    if (m_runner->isRunning()) {
        emit logMessage(QStringLiteral("正在取消当前任务；将先请求进程正常终止，2 秒后仍未退出则强制结束。"));
        m_runner->cancel();
    }
}

void TaskQueue::stopAll()
{
    if (!m_running)
        return;
    m_stopRequested = true;
    markRemainingCancelled(m_currentIndex + 1);

    if (m_waitingForExistingDecision) {
        m_waitingForExistingDecision = false;
        if (m_currentIndex >= 0 && m_currentIndex < m_tasks.size())
            setStatus(m_currentIndex, TaskStatus::Cancelled, QStringLiteral("用户停止了批处理。"));
        finishBatch();
    } else if (m_runner->isRunning()) {
        emit logMessage(QStringLiteral("正在停止全部任务。"));
        m_runner->cancel();
    } else {
        if (m_currentIndex >= 0 && m_currentIndex < m_tasks.size()
            && m_tasks[m_currentIndex].status == TaskStatus::Pending) {
            setStatus(m_currentIndex, TaskStatus::Cancelled, QStringLiteral("用户停止了批处理。"));
        }
        finishBatch();
    }
}

void TaskQueue::resolveExistingTarget(ExistingTargetDecision decision)
{
    if (!m_running || !m_waitingForExistingDecision)
        return;
    m_waitingForExistingDecision = false;

    switch (decision) {
    case ExistingTargetDecision::OverwriteAll:
        m_overwriteAll = true;
        m_currentOverwriteAllowed = true;
        startCurrent();
        break;
    case ExistingTargetDecision::Overwrite:
        m_currentOverwriteAllowed = true;
        startCurrent();
        break;
    case ExistingTargetDecision::SkipAll:
        m_skipAll = true;
        skipCurrent();
        break;
    case ExistingTargetDecision::Skip:
        skipCurrent();
        break;
    case ExistingTargetDecision::CancelAll:
        m_stopRequested = true;
        setStatus(m_currentIndex, TaskStatus::Cancelled, QStringLiteral("用户取消了批处理。"));
        markRemainingCancelled(m_currentIndex + 1);
        finishBatch();
        break;
    }
}

void TaskQueue::forceShutdown()
{
    if (!m_running && !m_runner->isRunning())
        return;

    m_stopRequested = true;
    m_runner->forceStopAndWait();
    if (m_currentIndex >= 0 && m_currentIndex < m_tasks.size()) {
        FileTask &task = m_tasks[m_currentIndex];
        cleanTemporaryOutput(task.tempOutputPath);
        if (task.status == TaskStatus::Encrypting || task.status == TaskStatus::Decrypting
            || task.status == TaskStatus::Pending) {
            task.status = TaskStatus::Cancelled;
            task.errorMessage = QStringLiteral("程序退出时任务被终止。");
            emit taskChanged(m_currentIndex, task);
        }
    }
    markRemainingCancelled(m_currentIndex + 1);
    if (m_running)
        finishBatch();
}

void TaskQueue::startNext()
{
    if (!m_running)
        return;
    if (m_stopRequested) {
        finishBatch();
        return;
    }

    ++m_currentIndex;
    if (m_currentIndex >= m_tasks.size()) {
        finishBatch();
        return;
    }

    FileTask &task = m_tasks[m_currentIndex];
    emit currentTaskChanged(task.inputPath, m_currentIndex + 1, m_tasks.size());
    refreshTotalProgress();

    const QFileInfo inputInfo(task.inputPath);
    if (!inputInfo.exists() || !inputInfo.isFile()) {
        setStatus(m_currentIndex, TaskStatus::Failed,
                  QStringLiteral("输入文件不存在或不是普通文件：%1").arg(task.inputPath));
        QTimer::singleShot(0, this, &TaskQueue::startNext);
        return;
    }
    if (inputInfo.size() <= 0) {
        setStatus(m_currentIndex, TaskStatus::Failed,
                  QStringLiteral("输入文件为空，未启动 tle.exe：%1").arg(task.inputPath));
        QTimer::singleShot(0, this, &TaskQueue::startNext);
        return;
    }
    if (inputInfo.size() != task.inputSize) {
        m_totalBytes -= task.inputSize;
        task.inputSize = inputInfo.size();
        m_totalBytes = saturatedAdd(m_totalBytes, task.inputSize);
        emit taskChanged(m_currentIndex, task);
    }

    if (m_settings.mode == TaskMode::Encrypt
        && QDateTime::currentDateTime().secsTo(m_settings.unlockTarget) <= 0) {
        const QString message = QStringLiteral("目标解锁时间已经到达或过去，无法继续生成时间锁文件。");
        setStatus(m_currentIndex, TaskStatus::Failed, message);
        markRemainingCancelled(m_currentIndex + 1);
        emit fatalError(message);
        finishBatch();
        return;
    }

    const QString outputDirectory = QFileInfo(task.finalOutputPath).absolutePath();
    QString writableError;
    if (!Utils::isPathWritableDirectory(outputDirectory, &writableError)) {
        setStatus(m_currentIndex, TaskStatus::Failed, writableError);
        QTimer::singleShot(0, this, &TaskQueue::startNext);
        return;
    }

    const QStorageInfo storage(outputDirectory);
    if (storage.isValid() && storage.isReady()
        && storage.bytesAvailable() <= saturatedAdd(task.inputSize, safetyMargin)) {
        const QString message = QStringLiteral("输出磁盘空间不足。至少需要 %1，可用 %2。")
            .arg(Utils::formatBytes(saturatedAdd(task.inputSize, safetyMargin)),
                 Utils::formatBytes(storage.bytesAvailable()));
        setStatus(m_currentIndex, TaskStatus::Failed, message);
        QTimer::singleShot(0, this, &TaskQueue::startNext);
        return;
    }

    if (QFileInfo::exists(task.finalOutputPath)) {
        if (m_skipAll) {
            skipCurrent();
            return;
        }
        if (m_overwriteAll) {
            m_currentOverwriteAllowed = true;
            startCurrent();
            return;
        }
        m_waitingForExistingDecision = true;
        emit existingTargetFound(m_currentIndex, task.finalOutputPath);
        return;
    }

    m_currentOverwriteAllowed = false;
    startCurrent();
}

void TaskQueue::startCurrent()
{
    if (!m_running || m_currentIndex < 0 || m_currentIndex >= m_tasks.size())
        return;

    FileTask &task = m_tasks[m_currentIndex];
    if (QFileInfo::exists(task.tempOutputPath)) {
        setStatus(m_currentIndex, TaskStatus::Failed,
                  QStringLiteral("临时输出路径已存在。为保护现有数据，未覆盖该文件：%1\n"
                                 "请确认后手动移动或删除它，再重试。")
                      .arg(task.tempOutputPath));
        QTimer::singleShot(0, this, &TaskQueue::startNext);
        return;
    }

    QStringList arguments;
    if (m_settings.mode == TaskMode::Encrypt) {
        const qint64 seconds = QDateTime::currentDateTime().secsTo(m_settings.unlockTarget);
        if (seconds <= 0) {
            const QString message = QStringLiteral("目标解锁时间已经到达或过去，无法继续生成时间锁文件。");
            setStatus(m_currentIndex, TaskStatus::Failed, message);
            markRemainingCancelled(m_currentIndex + 1);
            emit fatalError(message);
            finishBatch();
            return;
        }
        arguments << QStringLiteral("-e")
                  << QStringLiteral("-D")
                  << QString::number(seconds) + QStringLiteral("s");
        setStatus(m_currentIndex, TaskStatus::Encrypting);
        emit logMessage(QStringLiteral("开始加密：%1").arg(task.inputPath));
        emit logMessage(QStringLiteral("Unlock target: %1\nRemaining: %2s")
                            .arg(m_settings.unlockTarget.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                            .arg(seconds));
    } else {
        arguments << QStringLiteral("-d");
        setStatus(m_currentIndex, TaskStatus::Decrypting);
        emit logMessage(QStringLiteral("开始解密：%1").arg(task.inputPath));
    }

    if (!m_settings.useDefaultNetwork) {
        arguments << QStringLiteral("-n") << m_settings.network
                  << QStringLiteral("-c") << m_settings.chain;
    }
    arguments << QStringLiteral("-o") << task.tempOutputPath << task.inputPath;

    emit logMessage(QStringLiteral("Program:\n%1\nArguments:\n%2")
                        .arg(m_settings.tlePath, arguments.join(QLatin1Char('\n'))));
    m_runner->start(m_settings.tlePath, arguments, task.tempOutputPath, task.inputSize);
}

void TaskQueue::skipCurrent()
{
    if (m_currentIndex < 0 || m_currentIndex >= m_tasks.size())
        return;
    setStatus(m_currentIndex, TaskStatus::Skipped, QStringLiteral("目标文件已存在，已按用户选择跳过。"));
    emit logMessage(QStringLiteral("已跳过：%1").arg(m_tasks[m_currentIndex].inputPath));
    refreshTotalProgress();
    QTimer::singleShot(0, this, &TaskQueue::startNext);
}

void TaskQueue::finishBatch()
{
    if (!m_running)
        return;
    m_running = false;
    m_waitingForExistingDecision = false;
    refreshTotalProgress();
    emit runningChanged(false);
    emit batchFinished(m_stopRequested);
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
            setStatus(index, TaskStatus::Cancelled, QStringLiteral("批处理已停止。"));
    }
}

void TaskQueue::cleanTemporaryOutput(const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return;
    if (!QFile::remove(path))
        emit logMessage(QStringLiteral("警告：无法删除临时文件，请手动处理：%1").arg(path));
}

bool TaskQueue::finalizeOutput(FileTask &task, bool overwriteAllowed, QString *errorMessage)
{
    const QFileInfo tempInfo(task.tempOutputPath);
    if (!tempInfo.exists() || !tempInfo.isFile() || tempInfo.size() <= 0) {
        *errorMessage = QStringLiteral("临时输出不存在或为空：%1").arg(task.tempOutputPath);
        return false;
    }

    const bool targetExists = QFileInfo::exists(task.finalOutputPath);
    if (targetExists && !overwriteAllowed) {
        *errorMessage = QStringLiteral("处理期间目标文件出现，为保护数据未覆盖：%1").arg(task.finalOutputPath);
        return false;
    }

    if (!targetExists) {
        if (!QFile::rename(task.tempOutputPath, task.finalOutputPath)) {
            *errorMessage = QStringLiteral("无法将临时文件重命名为最终文件：%1").arg(task.finalOutputPath);
            return false;
        }
        return true;
    }

    const QString backupPath = task.finalOutputPath
        + QStringLiteral(".tlockgui-backup-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QFile::rename(task.finalOutputPath, backupPath)) {
        *errorMessage = QStringLiteral("无法暂存已有目标文件，原文件保持不变：%1").arg(task.finalOutputPath);
        return false;
    }

    if (!QFile::rename(task.tempOutputPath, task.finalOutputPath)) {
        const bool restored = QFile::rename(backupPath, task.finalOutputPath);
        *errorMessage = restored
            ? QStringLiteral("新文件重命名失败；已有目标文件已恢复。")
            : QStringLiteral("新文件重命名失败，并且旧文件恢复失败。旧文件保存在：%1").arg(backupPath);
        return false;
    }

    if (!QFile::remove(backupPath))
        emit logMessage(QStringLiteral("警告：新文件已保存，但旧文件备份未能删除：%1").arg(backupPath));
    return true;
}

void TaskQueue::refreshTotalProgress(qint64 currentBytes)
{
    qint64 estimatedCompleted = m_successfulBytes;
    if (m_running && m_currentIndex >= 0 && m_currentIndex < m_tasks.size()) {
        const FileTask &task = m_tasks[m_currentIndex];
        if (task.status == TaskStatus::Encrypting || task.status == TaskStatus::Decrypting)
            estimatedCompleted += qMin(task.inputSize, qMax<qint64>(0, currentBytes));
    }
    const int percent = m_totalBytes > 0
        ? qBound(0, static_cast<int>((static_cast<double>(estimatedCompleted)
                                      / static_cast<double>(m_totalBytes)) * 100.0), 100)
        : 0;
    emit totalProgress(m_currentIndex >= 0 ? m_currentIndex + 1 : 0,
                       m_tasks.size(), m_successfulBytes, m_totalBytes, percent);
}

QString TaskQueue::friendlyFailure(const QString &diagnostic, int exitCode) const
{
    const QString lower = diagnostic.toLower();
    if (lower.contains(QStringLiteral("too early")))
        return QStringLiteral("尚未到达解锁时间。tle.exe 退出代码：%1").arg(exitCode);
    if (lower.contains(QStringLiteral("no space"))
        || lower.contains(QStringLiteral("disk full"))
        || lower.contains(QStringLiteral("not enough space"))) {
        return QStringLiteral("输出磁盘可能已满。tle.exe 退出代码：%1").arg(exitCode);
    }
    if (lower.contains(QStringLiteral("network"))
        || lower.contains(QStringLiteral("connect"))
        || lower.contains(QStringLiteral("request"))) {
        return QStringLiteral("无法访问 drand 网络，请检查网络连接和高级网络设置。tle.exe 退出代码：%1")
            .arg(exitCode);
    }
    if (!diagnostic.trimmed().isEmpty())
        return QStringLiteral("tle.exe 执行失败（退出代码 %1）：%2").arg(exitCode).arg(diagnostic.trimmed());
    return QStringLiteral("tle.exe 执行失败，退出代码：%1").arg(exitCode);
}
