#include "TleRunner.h"

#include <QFileInfo>

TleRunner::TleRunner(QObject *parent)
    : QObject(parent)
{
    m_progressTimer.setInterval(500);
    connect(&m_progressTimer, &QTimer::timeout, this, &TleRunner::updateProgress);
}

TleRunner::~TleRunner()
{
    forceStopAndWait();
}

bool TleRunner::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

void TleRunner::start(const QString &program, const QStringList &arguments,
                      const QString &tempOutputPath, qint64 inputSize)
{
    if (isRunning()) {
        emit finished(false, false, -1, QStringLiteral("内部错误：已有 tle.exe 进程正在运行。"));
        return;
    }

    auto *process = new QProcess(this);
    m_process = process;
    m_tempOutputPath = tempOutputPath;
    m_inputSize = inputSize;
    m_standardOutput.clear();
    m_standardError.clear();
    m_samples.clear();
    m_cancelRequested = false;
    m_resultEmitted = false;
    m_suppressResult = false;

    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->setProgram(program);
    process->setArguments(arguments);

    connect(process, &QProcess::started, this, [this]() {
        m_elapsed.start();
        m_samples.append({0, 0});
        m_progressTimer.start();
        emit started();
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [this]() {
        if (!m_process)
            return;
        const QByteArray data = m_process->readAllStandardOutput();
        m_standardOutput += data;
        emit outputReceived(QString::fromUtf8(data), false);
    });
    connect(process, &QProcess::readyReadStandardError, this, [this]() {
        if (!m_process)
            return;
        const QByteArray data = m_process->readAllStandardError();
        m_standardError += data;
        emit outputReceived(QString::fromUtf8(data), true);
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_resultEmitted || !m_process)
            return;
        const QString message = QStringLiteral("tle.exe 启动失败：%1").arg(m_process->errorString());
        QTimer::singleShot(0, this, [this, message]() { completeStartFailure(message); });
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &TleRunner::complete);

    process->start();
}

void TleRunner::cancel()
{
    if (!isRunning())
        return;
    m_cancelRequested = true;
    const QPointer<QProcess> process = m_process;
    process->terminate();
    QTimer::singleShot(2000, this, [process]() {
        if (process && process->state() != QProcess::NotRunning)
            process->kill();
    });
}

void TleRunner::forceStopAndWait()
{
    m_progressTimer.stop();
    if (!m_process)
        return;

    m_suppressResult = true;
    const QPointer<QProcess> process = m_process;
    if (process->state() != QProcess::NotRunning) {
        process->terminate();
        if (!process->waitForFinished(2000)) {
            process->kill();
            process->waitForFinished(5000);
        }
    }
    if (process)
        process->deleteLater();
    if (m_process == process)
        m_process = nullptr;
}

void TleRunner::updateProgress()
{
    if (!m_elapsed.isValid())
        return;

    const qint64 bytes = qMax<qint64>(0, QFileInfo(m_tempOutputPath).size());
    const qint64 milliseconds = m_elapsed.elapsed();
    m_samples.append({milliseconds, bytes});
    while (m_samples.size() > 2 && milliseconds - m_samples.front().milliseconds > 5000)
        m_samples.removeFirst();

    double speed = -1.0;
    if (m_samples.size() >= 2) {
        const Sample &first = m_samples.front();
        const Sample &last = m_samples.back();
        const qint64 deltaMilliseconds = last.milliseconds - first.milliseconds;
        const qint64 deltaBytes = last.bytes - first.bytes;
        if (deltaMilliseconds > 0 && deltaBytes >= 0)
            speed = (static_cast<double>(deltaBytes) * 1000.0) / static_cast<double>(deltaMilliseconds);
    }

    int percent = 0;
    if (m_inputSize > 0) {
        percent = qBound(0,
            static_cast<int>((static_cast<double>(bytes) / static_cast<double>(m_inputSize)) * 100.0),
            99);
    }

    qint64 eta = -1;
    if (speed > 1.0 && bytes < m_inputSize)
        eta = static_cast<qint64>((static_cast<double>(m_inputSize - bytes)) / speed);

    emit progress(bytes, percent, speed, milliseconds / 1000, eta);
}

void TleRunner::consumeOutput()
{
    if (!m_process)
        return;
    const QByteArray standardOutput = m_process->readAllStandardOutput();
    const QByteArray standardError = m_process->readAllStandardError();
    if (!standardOutput.isEmpty()) {
        m_standardOutput += standardOutput;
        emit outputReceived(QString::fromUtf8(standardOutput), false);
    }
    if (!standardError.isEmpty()) {
        m_standardError += standardError;
        emit outputReceived(QString::fromUtf8(standardError), true);
    }
}

void TleRunner::complete(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (m_resultEmitted)
        return;
    m_resultEmitted = true;
    m_progressTimer.stop();
    consumeOutput();
    updateProgress();

    const bool outputValid = QFileInfo(m_tempOutputPath).isFile()
        && QFileInfo(m_tempOutputPath).size() > 0;
    const bool succeeded = !m_cancelRequested
        && exitStatus == QProcess::NormalExit
        && exitCode == 0
        && outputValid;

    QString diagnostic = QString::fromUtf8(m_standardOutput);
    if (!m_standardError.isEmpty()) {
        if (!diagnostic.isEmpty() && !diagnostic.endsWith(QLatin1Char('\n')))
            diagnostic += QLatin1Char('\n');
        diagnostic += QString::fromUtf8(m_standardError);
    }
    if (exitStatus == QProcess::CrashExit && !m_cancelRequested)
        diagnostic += QStringLiteral("\ntle.exe 异常终止。");
    if (exitStatus == QProcess::NormalExit && exitCode == 0 && !outputValid && !m_cancelRequested)
        diagnostic += QStringLiteral("\ntle.exe 返回成功，但临时输出文件不存在或为空。");

    QPointer<QProcess> completedProcess = m_process;
    m_process = nullptr;
    if (completedProcess)
        completedProcess->deleteLater();

    if (!m_suppressResult)
        emit finished(succeeded, m_cancelRequested, exitCode, diagnostic.trimmed());
}

void TleRunner::completeStartFailure(const QString &message)
{
    if (m_resultEmitted)
        return;
    m_resultEmitted = true;
    m_progressTimer.stop();
    QPointer<QProcess> failedProcess = m_process;
    m_process = nullptr;
    if (failedProcess)
        failedProcess->deleteLater();
    if (!m_suppressResult)
        emit finished(false, m_cancelRequested, -1, message);
}
