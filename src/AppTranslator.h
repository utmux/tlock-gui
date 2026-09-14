#pragma once

#include <QTranslator>

class AppTranslator : public QTranslator
{
public:
    explicit AppTranslator(QObject *parent = nullptr);

    bool isEmpty() const override;
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override;
};
