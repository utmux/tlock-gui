#include "HashWorker.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

HashWorker::HashWorker(QObject *parent)
    : QObject(parent)
{
}

void HashWorker::calculate(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit finished(path, {}, tr("无法读取文件：%1").arg(file.errorString()));
        return;
    }

    const qint64 total = QFileInfo(file).size();
    constexpr qint64 bufferSize = 8LL * 1024LL * 1024LL;
    QByteArray buffer;
    buffer.resize(static_cast<qsizetype>(bufferSize));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 completed = 0;
    int lastPercent = -1;

    while (!file.atEnd()) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            emit finished(path, {}, tr("SHA-256 计算已取消。"));
            return;
        }

        const qint64 bytesRead = file.read(buffer.data(), buffer.size());
        if (bytesRead < 0) {
            emit finished(path, {}, tr("读取文件失败：%1").arg(file.errorString()));
            return;
        }
        if (bytesRead == 0)
            break;

        hash.addData(QByteArrayView(buffer.constData(), static_cast<qsizetype>(bytesRead)));
        completed += bytesRead;
        const int percent = total > 0
            ? qBound(0, static_cast<int>((static_cast<double>(completed) / static_cast<double>(total)) * 100.0), 100)
            : 100;
        if (percent != lastPercent) {
            lastPercent = percent;
            emit progress(path, percent);
        }
    }

    emit finished(path, QString::fromLatin1(hash.result().toHex()), {});
}

void HashWorker::cancel()
{
    m_cancelled.store(true, std::memory_order_relaxed);
}
