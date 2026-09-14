#pragma once

#include <QObject>

#include <atomic>

class HashWorker : public QObject
{
    Q_OBJECT

public:
    explicit HashWorker(QObject *parent = nullptr);

public slots:
    void calculate(const QString &path);
    void cancel();

signals:
    void progress(const QString &path, int percent);
    void finished(const QString &path, const QString &hexDigest, const QString &errorMessage);

private:
    std::atomic_bool m_cancelled{false};
};
