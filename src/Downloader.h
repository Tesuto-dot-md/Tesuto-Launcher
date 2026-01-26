#pragma once
#include <QtCore>
#include "Net.h"

class Downloader {
public:
    explicit Downloader(Net& net);

    // Если rel пустая — base считается полным URL файла.
    QByteArray getWithMirrors(const QList<QUrl>& bases, const QString& rel);

private:
    Net& net_;
};
