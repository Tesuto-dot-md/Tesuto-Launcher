#pragma once
#include <QtCore>
#include "MojangAPI.h"
#include "Downloader.h"
#include "Util.h"

class Installer {
public:
    // cacheDir можно не указывать — возьмём дефолтный (~/.cache/tesuto-launcher)
    Installer(MojangAPI& api, QString gameDir, QString cacheDir = QString());

    // Ставит всё нужное для версии в gameDir_
    void install(const VersionResolved& v);

    // Класс-путь для запуска (libs + client.jar)
    QStringList classpathJars(const VersionResolved& v) const;



    // Install Temurin JRE to destBase (e.g., <gameDir>/runtime). Returns path to bin/java.
    // Only linux implemented; others return empty string.
    QString installTemurinJre(int major, const QString& destBase, QString* outVersion = nullptr);

private:
    MojangAPI& api_;
    QString gameDir_;
    QString cacheDir_; // общий кэш

    // --- пути внутри инстанса ---
    static QString assetsObjectsPath(const QString& base) { return joinPath(base, "assets/objects"); }
    static QString assetsIndexesPath(const QString& base) { return joinPath(base, "assets/indexes"); }
    static QString librariesPath    (const QString& base) { return joinPath(base, "libraries"); }
    static QString versionsPath     (const QString& base) { return joinPath(base, "versions"); }
    static QString nativesPath      (const QString& base, const QString& id){ return joinPath(base, "versions/"+id+"/natives"); }

    // --- пути внутри кэша ---
    QString cacheAssetsObjects() const { return joinPath(cacheDir_, "assets/objects"); }
    QString cacheAssetsIndexes() const { return joinPath(cacheDir_, "assets/indexes"); }
    QString cacheLibraries     () const { return joinPath(cacheDir_, "libraries"); }

    // Лок. утилиты
    static QString defaultCacheDir();
    static bool linkOrCopy(const QString& src, const QString& dst);

    // Быстрый fetch asset index с локальным кэшем
    QJsonObject fetchAssetIndexCached(const QUrl& url) const;
};
