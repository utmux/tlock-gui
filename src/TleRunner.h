#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QVector>

class TleRunner : public QObject
{
    Q_OBJECT

public:
    explicit TleRunner(QObject *parent = nullptr);
    ~TleRunner() override;

    bool isRunning() const;
    void start(const QString &program, const QStringList &arguments,
               const QString &tempOutputPath, qint64 inputSize);
    void cancel();
    void forceStopAndWait();

signals:
    void started();
    void outputReceived(const QString &text, bool standardError);
    void progress(qint64 outputBytes, int percent, double bytesPerSecond,
                  qint64 elapsedSeconds, qint64 etaSeconds);
    void finished(bool processSucceeded, bool cancelled, int exitCode,
                  const QString &diagnosticOutput);

private slots:
    void updateProgress();

private:
    struct Sample {
        qint64 milliseconds = 0;
        qint64 bytes = 0;
    };

    void consumeOutput();
    void complete(int exitCode, QProcess::ExitStatus exitStatus);
    void completeStartFailure(const QString &message);

    QPointer<QProcess> m_process;
    QTimer m_progressTimer;
    QElapsedTimer m_elapsed;
    QVector<Sample> m_samples;
    QString m_tempOutputPath;
    QByteArray m_standardOutput;
    QByteArray m_standardError;
    qint64 m_inputSize = 0;
    bool m_cancelRequested = false;
    bool m_resultEmitted = false;
    bool m_suppressResult = false;
};
