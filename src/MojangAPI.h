#pragma once
#include <QtCore>
#include <QtNetwork>
#include "Net.h"
#include "Downloader.h"

struct LibEntry {
    QString  path;     // относительный maven-путь (group/artifact/version/artifact-version.jar)
    QUrl     url;      // прямой URL (если дан)
    QString  sha1;     // sha1 (если дан)
    bool     isNative = false; // true, если это natives-jar (редко нужен для нашего простого запуска)
};

struct VersionRef {
    QString id;
    QUrl    url;
    QString type;
};

struct VersionResolved {
    QString     id;
    QString     mainClass;
    QJsonObject raw;

    QString assetIndexId;
    QUrl    assetIndexUrl;

    QUrl    clientJarUrl;
    QList<LibEntry> libraries;
};

class MojangAPI {
public:
    explicit MojangAPI(Net& net) : net_(net), downloader_(net) {}

    QList<VersionRef>  getVersionList();
    VersionResolved    resolveVersion(const VersionRef& ref);
    // Удобный перегруз — разрешить по строке версии:
    VersionResolved    resolveVersion(const QString& versionId);

    Downloader& dl() { return downloader_; }

private:
    Net&        net_;
    Downloader  downloader_;
};
