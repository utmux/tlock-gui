#include "AppTranslator.h"
#include "FileTask.h"
#include "PowerManager.h"
#include "TaskQueue.h"
#include "Utils.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

namespace {

bool writePatternFile(const QString &path, int repetitions, char value)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray block(64 * 1024, value);
    for (int index = 0; index < repetitions; ++index) {
        if (file.write(block) != block.size())
            return false;
    }
    file.close();
    return file.error() == QFile::NoError;
}

bool verifyPatternFile(const QString &path, int repetitions, char value)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray expected(64 * 1024, value);
    for (int index = 0; index < repetitions; ++index) {
        if (file.read(expected.size()) != expected)
            return false;
    }
    return file.atEnd();
}

int fail(const QString &message)
{
    QTextStream(stderr) << "FAIL: " << message << '\n';
    return 1;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    if (application.arguments().size() < 2)
        return fail(QStringLiteral("FakeTle path was not provided"));
    const QString fakeTlePath = application.arguments().at(1);
    if (!QFileInfo(fakeTlePath).isFile())
        return fail(QStringLiteral("FakeTle does not exist"));

    AppTranslator englishTranslator;
    application.installTranslator(&englishTranslator);
    if (Utils::taskStatusText(TaskStatus::Pending) != QStringLiteral("Pending")
        || QCoreApplication::translate("MainWindow", "并行任务数：") != QStringLiteral("Parallel tasks:")
        || QCoreApplication::translate(
               "TaskQueue",
               "尚未到达解锁轮次。预计可解密时间：%1（%2）；按当前轮次估计还需约 %3。"
               "目标轮次：%4，当前轮次：%5。drand 节点可能有数秒延迟，请到时稍后重试。")
               .startsWith(QStringLiteral("The unlock round has not arrived.")) == false) {
        return fail(QStringLiteral("English translation"));
    }
    application.removeTranslator(&englishTranslator);

    QString powerError;
    if (!PowerManager::setKeepAwake(true, &powerError))
        return fail(QStringLiteral("Windows keep-awake request: %1").arg(powerError));
    if (!PowerManager::setKeepAwake(false, &powerError))
        return fail(QStringLiteral("Windows keep-awake reset: %1").arg(powerError));

    if (Utils::formatBytes(10737418240LL) != QStringLiteral("10.00 GiB"))
        return fail(QStringLiteral("64-bit size formatting"));
    if (!Utils::outputPathFor(QStringLiteral("C:/a/movie.mp4.tle"), TaskMode::Decrypt, true, {})
             .endsWith(QStringLiteral("movie.mp4"))) {
        return fail(QStringLiteral("decrypt output naming"));
    }
    if (!Utils::outputPathFor(QStringLiteral("C:/a/data.bin"), TaskMode::Decrypt, true, {})
             .endsWith(QStringLiteral("data.bin.decrypted"))) {
        return fail(QStringLiteral("fallback decrypt output naming"));
    }
    qint64 expectedRound = 0;
    qint64 currentRound = 0;
    if (!Utils::parseDrandTooEarlyRounds(
            QStringLiteral("too early to decrypt: expected round 32194618 > 32194608 current round"),
            &expectedRound, &currentRound)
        || expectedRound != 32194618 || currentRound != 32194608
        || Utils::parseDrandTooEarlyRounds(QStringLiteral("too early to decrypt"), nullptr, nullptr)) {
        return fail(QStringLiteral("drand too-early round parsing"));
    }
    if (Utils::quicknetRoundTimeUtc(1).toSecsSinceEpoch() != 1692803367LL
        || Utils::quicknetRoundTimeUtc(11).toSecsSinceEpoch() != 1692803397LL
        || Utils::quicknetRoundTimeUtc(0).isValid()
        || Utils::quicknetChainHash()
            != QStringLiteral("52db9ba70e0cc0f6eaf7803dd07447a1f5477735fd3f661792ba94600c84e971")) {
        return fail(QStringLiteral("quicknet round time conversion"));
    }

    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid())
        return fail(QStringLiteral("temporary directory"));
    const QString specialDirectory = QDir(temporaryDirectory.path())
        .filePath(QStringLiteral("输出 测试 (1) & [批量]"));
    if (!QDir().mkpath(specialDirectory))
        return fail(QStringLiteral("special directory creation"));

    const QString firstInput = QDir(specialDirectory).filePath(QStringLiteral("视频 文件 (1) &.mp4"));
    const QString secondInput = QDir(specialDirectory).filePath(QStringLiteral("第二个文件.bin"));
    if (!writePatternFile(firstInput, 24, 'A') || !writePatternFile(secondInput, 12, 'B'))
        return fail(QStringLiteral("input creation"));

    QVector<FileTask> tasks;
    for (const QString &input : {firstInput, secondInput}) {
        FileTask task;
        task.inputPath = input;
        task.inputSize = QFileInfo(input).size();
        task.finalOutputPath = Utils::outputPathFor(input, TaskMode::Encrypt, true, {});
        task.tempOutputPath = task.finalOutputPath + QStringLiteral(".part");
        tasks.append(task);
    }

    if (!writePatternFile(tasks.first().finalOutputPath, 1, 'Z'))
        return fail(QStringLiteral("existing target creation"));

    BatchSettings settings;
    settings.tlePath = fakeTlePath;
    settings.mode = TaskMode::Encrypt;
    settings.unlockTarget = QDateTime::currentDateTime().addSecs(600);
    settings.useDefaultNetwork = true;
    settings.maxParallelTasks = 2;

    TaskQueue queue;
    bool timedOut = false;
    bool batchFinished = false;
    int existingPrompts = 0;
    int peakActiveTasks = 0;
    QEventLoop loop;
    QObject::connect(&queue, &TaskQueue::existingTargetFound, &queue,
                     [&queue, &existingPrompts](int, const QString &) {
        ++existingPrompts;
        queue.resolveExistingTarget(TaskQueue::ExistingTargetDecision::Overwrite);
    });
    QObject::connect(&queue, &TaskQueue::activeCountChanged, &queue,
                     [&peakActiveTasks](int active, int) {
        peakActiveTasks = qMax(peakActiveTasks, active);
    });
    QObject::connect(&queue, &TaskQueue::batchFinished, &loop,
                     [&loop, &batchFinished](bool) {
        batchFinished = true;
        loop.quit();
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&loop, &timedOut]() {
        timedOut = true;
        loop.quit();
    });

    queue.start(tasks, settings);
    timeout.start(15000);
    loop.exec();
    if (timedOut || !batchFinished) {
        queue.forceShutdown();
        return fail(QStringLiteral("queue timeout"));
    }
    if (existingPrompts != 1)
        return fail(QStringLiteral("overwrite prompt count"));
    if (peakActiveTasks != 2)
        return fail(QStringLiteral("parallel scheduler did not respect the configured limit"));
    if (queue.tasks().size() != 2
        || queue.tasks().at(0).status != TaskStatus::Success
        || queue.tasks().at(1).status != TaskStatus::Success) {
        return fail(QStringLiteral("parallel task completion"));
    }
    if (QFileInfo::exists(tasks.at(0).tempOutputPath) || QFileInfo::exists(tasks.at(1).tempOutputPath))
        return fail(QStringLiteral("part cleanup and rename"));
    if (!verifyPatternFile(tasks.at(0).finalOutputPath, 24, 'A')
        || !verifyPatternFile(tasks.at(1).finalOutputPath, 12, 'B')) {
        return fail(QStringLiteral("safe final output contents"));
    }

    FileTask protectedTask;
    protectedTask.inputPath = secondInput;
    protectedTask.inputSize = QFileInfo(secondInput).size();
    protectedTask.finalOutputPath = QDir(specialDirectory).filePath(QStringLiteral("protected-output.tle"));
    protectedTask.tempOutputPath = protectedTask.finalOutputPath + QStringLiteral(".part");
    if (!writePatternFile(protectedTask.tempOutputPath, 1, 'P'))
        return fail(QStringLiteral("pre-existing part creation"));

    TaskQueue protectionQueue;
    QEventLoop protectionLoop;
    QTimer protectionTimeout;
    protectionTimeout.setSingleShot(true);
    QObject::connect(&protectionQueue, &TaskQueue::batchFinished,
                     &protectionLoop, [&protectionLoop](bool) { protectionLoop.quit(); });
    QObject::connect(&protectionTimeout, &QTimer::timeout,
                     &protectionLoop, [&protectionLoop]() { protectionLoop.quit(); });
    protectionQueue.start({protectedTask}, settings);
    protectionTimeout.start(5000);
    protectionLoop.exec();
    if (protectionQueue.isRunning()
        || protectionQueue.tasks().constFirst().status != TaskStatus::Failed
        || !verifyPatternFile(protectedTask.tempOutputPath, 1, 'P')) {
        protectionQueue.forceShutdown();
        return fail(QStringLiteral("pre-existing part protection"));
    }

    QVector<FileTask> expiredTasks;
    for (int index = 0; index < 2; ++index) {
        FileTask task;
        task.inputPath = secondInput;
        task.inputSize = QFileInfo(secondInput).size();
        task.finalOutputPath = QDir(specialDirectory)
            .filePath(QStringLiteral("expired-%1.tle").arg(index));
        task.tempOutputPath = task.finalOutputPath + QStringLiteral(".part");
        expiredTasks.append(task);
    }
    BatchSettings expiredSettings = settings;
    expiredSettings.unlockTarget = QDateTime::currentDateTime().addSecs(-5);
    TaskQueue expiredQueue;
    QEventLoop expiredLoop;
    QObject::connect(&expiredQueue, &TaskQueue::batchFinished,
                     &expiredLoop, [&expiredLoop](bool) { expiredLoop.quit(); });
    expiredQueue.start(expiredTasks, expiredSettings);
    QTimer::singleShot(5000, &expiredLoop, &QEventLoop::quit);
    expiredLoop.exec();
    if (expiredQueue.isRunning()
        || expiredQueue.tasks().at(0).status != TaskStatus::Failed
        || expiredQueue.tasks().at(1).status != TaskStatus::Cancelled) {
        expiredQueue.forceShutdown();
        return fail(QStringLiteral("expired target stops remaining encryption"));
    }

    QVector<FileTask> cancelTasks;
    for (int index = 0; index < 3; ++index) {
        const QString cancelInput = QDir(specialDirectory)
            .filePath(QStringLiteral("cancel-source-%1.bin").arg(index));
        if (!writePatternFile(cancelInput, 128, static_cast<char>('C' + index)))
            return fail(QStringLiteral("cancel input creation"));
        FileTask cancelTask;
        cancelTask.inputPath = cancelInput;
        cancelTask.inputSize = QFileInfo(cancelInput).size();
        cancelTask.finalOutputPath = QDir(specialDirectory)
            .filePath(QStringLiteral("cancel-output-%1.tle").arg(index));
        cancelTask.tempOutputPath = cancelTask.finalOutputPath + QStringLiteral(".part");
        cancelTasks.append(cancelTask);
    }

    TaskQueue cancelQueue;
    QEventLoop cancelLoop;
    bool cancelTimedOut = false;
    QObject::connect(&cancelQueue, &TaskQueue::batchFinished,
                     &cancelLoop, [&cancelLoop](bool) { cancelLoop.quit(); });
    QTimer::singleShot(30, &cancelQueue, &TaskQueue::stopAll);
    QTimer::singleShot(5000, &cancelLoop, [&cancelLoop, &cancelTimedOut]() {
        cancelTimedOut = true;
        cancelLoop.quit();
    });
    BatchSettings cancelSettings = settings;
    cancelSettings.maxParallelTasks = 3;
    cancelQueue.start(cancelTasks, cancelSettings);
    cancelLoop.exec();
    bool cancellationClean = !cancelTimedOut && !cancelQueue.isRunning();
    for (int index = 0; index < cancelTasks.size(); ++index) {
        cancellationClean = cancellationClean
            && cancelQueue.tasks().at(index).status == TaskStatus::Cancelled
            && !QFileInfo::exists(cancelTasks.at(index).tempOutputPath)
            && verifyPatternFile(cancelTasks.at(index).inputPath, 128, static_cast<char>('C' + index));
    }
    if (!cancellationClean) {
        cancelQueue.forceShutdown();
        return fail(QStringLiteral("parallel stop lifecycle, part cleanup, or input preservation"));
    }

    QTextStream(stdout) << "PASS: utils, special paths, parallel queue, .part rename, safe overwrite, "
                           "stale part protection, expired target stop, parallel cancellation cleanup\n";
    return 0;
}
