#include <QCoreApplication>
#include <QFile>
#include <QTextStream>
#include <QThread>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments().mid(1);
    QTextStream standardOutput(stdout);
    QTextStream standardError(stderr);

    if (arguments.contains(QStringLiteral("--metadata"))) {
        standardOutput << "fake-tle: test-only\n";
        return 0;
    }

    const int outputOption = arguments.indexOf(QStringLiteral("-o"));
    if (outputOption < 0 || outputOption + 1 >= arguments.size() || arguments.isEmpty()) {
        standardError << "missing output or input\n";
        return 2;
    }

    const QString outputPath = arguments.at(outputOption + 1);
    const QString inputPath = arguments.constLast();
    QFile input(inputPath);
    QFile output(outputPath);
    if (!input.open(QIODevice::ReadOnly)) {
        standardError << "cannot open input\n";
        return 3;
    }
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        standardError << "cannot open output\n";
        return 4;
    }

    constexpr qint64 chunkSize = 64LL * 1024LL;
    QByteArray chunk;
    chunk.resize(static_cast<qsizetype>(chunkSize));
    while (!input.atEnd()) {
        const qint64 count = input.read(chunk.data(), chunk.size());
        if (count < 0 || output.write(chunk.constData(), count) != count) {
            standardError << "copy failed\n";
            return 5;
        }
        output.flush();
        QThread::msleep(2);
    }
    output.close();
    standardOutput << "fake copy complete\n";
    return 0;
}
