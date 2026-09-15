#include "FileTask.h"
#include "TaskQueue.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QTimer>

namespace {

struct RunResult {
    bool timedOut = false;
    FileTask task;
    QStringList log;
};

RunResult runOne(const QString &tlePath, FileTask task, TaskMode mode,
                 const QDateTime &unlockTarget, int timeoutMilliseconds)
{
    BatchSettings settings;
    settings.tlePath = tlePath;
    settings.mode = mode;
    settings.unlockTarget = unlockTarget;
    settings.useDefaultNetwork = true;

    TaskQueue queue;
    QEventLoop loop;
    RunResult result;
    QObject::connect(&queue, &TaskQueue::existingTargetFound, &queue,
                     [&queue](int, const QString &) {
        queue.resolveExistingTarget(TaskQueue::ExistingTargetDecision::Overwrite);
    });
    QObject::connect(&queue, &TaskQueue::logMessage, &queue,
                     [&result](const QString &message) { result.log.append(message); });
    QObject::connect(&queue, &TaskQueue::batchFinished, &loop,
                     [&loop](bool) { loop.quit(); });

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        result.timedOut = true;
        queue.forceShutdown();
        loop.quit();
    });
    timeout.start(timeoutMilliseconds);
    queue.start({task}, settings);
    loop.exec();
    if (!queue.tasks().isEmpty())
        result.task = queue.tasks().constFirst();
    return result;
}

QByteArray streamedSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return {};
    return hash.result();
}

void printLog(const QString &title, const RunResult &result)
{
    QTextStream stream(stdout);
    stream << "--- " << title << " ---\n";
    for (const QString &line : result.log)
        stream << line << '\n';
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
    if (application.arguments().size() < 3)
        return fail(QStringLiteral("usage: OfficialTleIntegration tle.exe input-file"));

    const QString tlePath = QFileInfo(application.arguments().at(1)).absoluteFilePath();
    const QString originalInput = QFileInfo(application.arguments().at(2)).absoluteFilePath();
    if (!QFileInfo(tlePath).isFile() || !QFileInfo(originalInput).isFile())
        return fail(QStringLiteral("tle.exe or input does not exist"));

    const QString specialDirectory = QDir(QFileInfo(originalInput).absolutePath())
        .filePath(QStringLiteral("integration-output/中文 测试 (1) & [official]"));
    const QString decryptedDirectory = QDir(specialDirectory).filePath(QStringLiteral("decrypted"));
    if (!QDir().mkpath(decryptedDirectory))
        return fail(QStringLiteral("cannot create integration output directories"));

    const QString stagedInput = QDir(specialDirectory).filePath(
        QStringLiteral("测试 文件 (1) &-%1.txt").arg(QDateTime::currentMSecsSinceEpoch()));
    if (!QFile::copy(originalInput, stagedInput))
        return fail(QStringLiteral("cannot stage test input"));

    const QDateTime target = QDateTime::currentDateTime().addSecs(30);
    FileTask encryptTask;
    encryptTask.inputPath = stagedInput;
    encryptTask.inputSize = QFileInfo(stagedInput).size();
    encryptTask.finalOutputPath = stagedInput + QStringLiteral(".tle");
    encryptTask.tempOutputPath = encryptTask.finalOutputPath + QStringLiteral(".part");

    const RunResult encrypted = runOne(tlePath, encryptTask, TaskMode::Encrypt, target, 30000);
    printLog(QStringLiteral("official encryption"), encrypted);
    if (encrypted.timedOut || encrypted.task.status != TaskStatus::Success)
        return fail(QStringLiteral("official encryption failed: %1").arg(encrypted.task.errorMessage));
    if (QFileInfo::exists(encryptTask.tempOutputPath) || !QFileInfo(encryptTask.finalOutputPath).isFile())
        return fail(QStringLiteral("encryption .part was not safely renamed"));

    FileTask decryptTask;
    decryptTask.inputPath = encryptTask.finalOutputPath;
    decryptTask.inputSize = QFileInfo(decryptTask.inputPath).size();
    decryptTask.finalOutputPath = QDir(decryptedDirectory).filePath(QFileInfo(stagedInput).fileName());
    decryptTask.tempOutputPath = decryptTask.finalOutputPath + QStringLiteral(".part");

    const RunResult early = runOne(tlePath, decryptTask, TaskMode::Decrypt, {}, 30000);
    printLog(QStringLiteral("early decryption"), early);
    if (early.timedOut)
        return fail(QStringLiteral("early decryption timed out"));
    const bool earlyRejected = early.task.status == TaskStatus::Failed
        && early.task.errorMessage.contains(QStringLiteral("预计可解密时间："))
        && early.task.errorMessage.contains(QStringLiteral("目标轮次："))
        && early.task.errorMessage.contains(QStringLiteral("当前轮次："));
    if (!earlyRejected && QDateTime::currentDateTime() < target)
        return fail(QStringLiteral("early decryption was not rejected as too early"));
    if (QFileInfo::exists(decryptTask.tempOutputPath))
        return fail(QStringLiteral("failed decryption left a .part file"));

    const qint64 waitMilliseconds = qMax<qint64>(0, QDateTime::currentDateTime().msecsTo(target.addSecs(5)));
    if (waitMilliseconds > 0) {
        QTextStream(stdout) << "Waiting " << waitMilliseconds << " ms for target round...\n";
        QEventLoop waitLoop;
        QTimer::singleShot(waitMilliseconds, &waitLoop, &QEventLoop::quit);
        waitLoop.exec();
    }

    const RunResult decrypted = runOne(tlePath, decryptTask, TaskMode::Decrypt, {}, 30000);
    printLog(QStringLiteral("mature decryption"), decrypted);
    if (decrypted.timedOut || decrypted.task.status != TaskStatus::Success)
        return fail(QStringLiteral("mature decryption failed: %1").arg(decrypted.task.errorMessage));
    if (QFileInfo::exists(decryptTask.tempOutputPath))
        return fail(QStringLiteral("successful decryption left a .part file"));
    if (streamedSha256(stagedInput).isEmpty()
        || streamedSha256(stagedInput) != streamedSha256(decryptTask.finalOutputPath)) {
        return fail(QStringLiteral("decrypted content does not match input"));
    }

    QTextStream(stdout) << "PASS: official tle.exe metadata-compatible arguments, 30s lock, "
                           "detailed early-decrypt estimate, mature decrypt, Unicode/special path, "
                           "safe .part output\n";
    return 0;
}
