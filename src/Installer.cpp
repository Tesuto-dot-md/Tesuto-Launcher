#include "Installer.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QProcess>
#include <QtConcurrent>
#include <QThreadPool>
#include <cstdlib>

// -------------------- helpers --------------------

QString Installer::defaultCacheDir()
{
    // 1) env override
    if (const char* env = std::getenv("TESUTO_CACHE_DIR")) {
        const QString s = QString::fromUtf8(env);
        if (!s.isEmpty()) return s;
    }
    // 2) XDG
    QString loc = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (loc.isEmpty()) loc = QDir::homePath() + "/.cache/tesuto-launcher";
    return loc;
}

bool Installer::linkOrCopy(const QString& src, const QString& dst)
{
    // гарантируем директорию
    QDir().mkpath(QFileInfo(dst).dir().absolutePath());
    // если уже есть — удалим
    if (QFileInfo::exists(dst)) QFile::remove(dst);
    // пробуем сделать хардлинк (или симлинк, если платформа так делает)
    if (QFile::link(src, dst)) return true;
    // не вышло — просто копируем
    return QFile::copy(src, dst);
}

// Читает индекс ассетов: инстанс -> кэш -> сеть; при скачивании пишет и в кэш, и в инстанс
QJsonObject Installer::fetchAssetIndexCached(const QUrl& url) const
{
    // Определим id индекс-файла из имени (…/1.21.1.json -> "1.21.1")
    const QString baseName = QFileInfo(url.path()).completeBaseName();

    const QString instIdxPath  = assetsIndexesPath(gameDir_) + "/" + baseName + ".json";
    const QString cacheIdxPath = cacheAssetsIndexes()      + "/" + baseName + ".json";

    // 1) из инстанса
    {
        QFile f(instIdxPath);
        if (f.exists() && f.open(QIODevice::ReadOnly)) {
            const auto doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) return doc.object();
        }
    }
    // 2) из кэша
    {
        QFile f(cacheIdxPath);
        if (f.exists() && f.open(QIODevice::ReadOnly)) {
            const auto doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) {
                // заодно положим в инстанс
                QDir().mkpath(QFileInfo(instIdxPath).path());
                QFile out(instIdxPath);
                if (out.open(QIODevice::WriteOnly))
                    out.write(QJsonDocument(doc.object()).toJson(QJsonDocument::Compact));
                return doc.object();
            }
        }
    }
    // 3) из сети: используем Downloader (точный URL, затем запасные зеркала по id)
    QByteArray body;
    bool ok = false;
    try {
        body = api_.dl().getWithMirrors({ url }, QString());
        ok = true;
    } catch (...) {
        // запасной маршрут: BMCL по имени индекса
        const QUrl bmcl(QString("https://bmclapi2.bangbang93.com/assets/indexes/%1.json").arg(baseName));
        body = api_.dl().getWithMirrors({ bmcl }, QString());
        ok = true;
    }
    if (!ok) throw std::runtime_error("Failed to download asset index");

    const auto doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) throw std::runtime_error("Invalid asset index JSON");

    // сохранить и в кэш, и в инстанс
    QDir().mkpath(QFileInfo(cacheIdxPath).path());
    { QFile f(cacheIdxPath); if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(doc.object()).toJson(QJsonDocument::Compact)); }
    QDir().mkpath(QFileInfo(instIdxPath).path());
    { QFile f(instIdxPath);  if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(doc.object()).toJson(QJsonDocument::Compact)); }

    return doc.object();
}

// -------------------- ctor --------------------

Installer::Installer(MojangAPI& api, QString gameDir, QString cacheDir)
    : api_(api)
    , gameDir_(std::move(gameDir))
    , cacheDir_(cacheDir.isEmpty() ? defaultCacheDir() : std::move(cacheDir))
{
    // гарантируем базовые каталоги
    ensureDir(gameDir_);
    ensureDir(assetsObjectsPath(gameDir_));
    ensureDir(assetsIndexesPath(gameDir_));
    ensureDir(librariesPath(gameDir_));
    ensureDir(versionsPath(gameDir_));

    ensureDir(cacheDir_);
    ensureDir(cacheAssetsObjects());
    ensureDir(cacheAssetsIndexes());
    ensureDir(cacheLibraries());
}

// -------------------- install --------------------

void Installer::install(const VersionResolved& v)
{
    ScopeTimer T("install");

    // 1) asset index (кэш-first)
    qInfo() << "assets index";
    const QUrl idxUrl(v.assetIndexUrl);
    const QJsonObject index = fetchAssetIndexCached(idxUrl);

    // 2) assets/objects с проверкой sha1 и кэшированием
    qInfo() << "assets objects";
    const auto objects = index.value("objects").toObject();
    struct AssetTask {
        QString sha;
        QString rel;
        QString instDst;
        QString cacheSrc;
    };
    QVector<AssetTask> tasks;
    tasks.reserve(objects.size());

    for (auto it = objects.begin(); it != objects.end(); ++it) {
        const auto obj      = it.value().toObject();
        const QString sha   = obj.value("hash").toString();
        if (sha.size() != 40) continue;

        const QString rel    = sha.left(2) + "/" + sha;
        const QString instDst  = joinPath(assetsObjectsPath(gameDir_), rel);
        const QString cacheSrc = joinPath(cacheAssetsObjects(),       rel);

        // если в инстансе уже валидно — пропускаем
        if (QFileInfo::exists(instDst) && sha1File(instDst) == sha)
            continue;

        // если в кэше валидно — линкуем/копируем
        if (QFileInfo::exists(cacheSrc) && sha1File(cacheSrc) == sha) {
            if (!linkOrCopy(cacheSrc, instDst))
                throw std::runtime_error(("Cannot place cached asset " + rel).toStdString());
            continue;
        }

        tasks.push_back(AssetTask{sha, rel, instDst, cacheSrc});
    }

    // Параллелим скачивание ассетов (самая долгая часть установки)
    const int threads = qEnvironmentVariableIntValue("TESUTO_DL_THREADS") > 0
                        ? qgetenv("TESUTO_DL_THREADS").toInt()
                        : 8;
    QThreadPool pool;
    pool.setMaxThreadCount(qMax(1, threads));

    const QList<QUrl> bases = {
        QUrl("https://resources.fastmcmirror.org"),
        QUrl("https://resources.download.minecraft.net")
    };

    // ВАЖНО: не бросаем исключения из задач QtConcurrent.
    // На некоторых сборках Qt это приводит к превращению ошибки в голое `std::exception`
    // (и мы теряем текст), а иногда и к аварийному завершению.
    std::atomic_bool anyFail{false};
    QMutex errMx;
    QString firstErr;

    try {
        QtConcurrent::blockingMap(
            &pool,
            tasks,
            [&](AssetTask& t) {
                if (anyFail.load()) return; // быстрый выход, если уже есть ошибка
                try {
                // отдельный Net на поток (QNetworkAccessManager не потокобезопасен)
                Net net;
                MojangAPI api(net);

                QByteArray data = api.dl().getWithMirrors(bases, t.rel);

                // в кэш
                ensureDir(QFileInfo(t.cacheSrc).dir().absolutePath());
                {
                    QFile f(t.cacheSrc);
                    if (!f.open(QIODevice::WriteOnly))
                        throw std::runtime_error(("cache write failed: " + t.cacheSrc).toStdString());
                    if (f.write(data) != data.size())
                        throw std::runtime_error(("cache write short: " + t.cacheSrc).toStdString());
                }

                if (sha1File(t.cacheSrc) != t.sha) {
                    QFile::remove(t.cacheSrc);
                    throw std::runtime_error(("Checksum mismatch for asset " + t.rel).toStdString());
                }

                // в инстанс (линк/копия)
                if (!Installer::linkOrCopy(t.cacheSrc, t.instDst))
                    throw std::runtime_error(("Cannot place asset to instance " + t.rel).toStdString());

                } catch (const std::exception& e) {
                    anyFail.store(true);
                    QMutexLocker lk(&errMx);
                    if (firstErr.isEmpty()) firstErr = QString::fromUtf8(e.what());
                } catch (...) {
                    anyFail.store(true);
                    QMutexLocker lk(&errMx);
                    if (firstErr.isEmpty()) firstErr = QStringLiteral("Unknown non-std exception");
                }
            }
        );
    } catch (const std::exception& e) {
        // Если QtConcurrent всё-таки пробросил исключение наружу — преобразуем в понятный текст
        throw std::runtime_error((QStringLiteral("Assets parallel stage failed: ") + QString::fromUtf8(e.what())).toStdString());
    } catch (...) {
        throw std::runtime_error("Assets parallel stage failed: Unknown exception");
    }

    if (anyFail.load())
        throw std::runtime_error(("Assets install failed: " + firstErr).toStdString());

    // 3) libraries (теперь тоже кэшируем — ускоряет повторные установки)
    qInfo() << "libraries";
    for (const auto& lib : v.libraries) {
        const QString dst = joinPath(librariesPath(gameDir_), lib.path);
        const QString cacheDst = joinPath(cacheLibraries(), lib.path);

        if (QFileInfo::exists(dst) && (lib.sha1.isEmpty() || sha1File(dst) == lib.sha1))
            continue;

        // если в кэше уже есть — разложим
        if (QFileInfo::exists(cacheDst) && (lib.sha1.isEmpty() || sha1File(cacheDst) == lib.sha1)) {
            if (!linkOrCopy(cacheDst, dst))
                throw std::runtime_error(("Cannot place cached lib " + lib.path).toStdString());
        } else {
            ensureDir(QFileInfo(cacheDst).dir().absolutePath());
            QByteArray data;

            try { data = api_.dl().getWithMirrors({ lib.url }, ""); }
            catch (...) {
                const QString rel = lib.path; // стандартный maven layout
                data = api_.dl().getWithMirrors(
                    { QUrl("https://libraries.fastmcmirror.org"),
                      QUrl("https://libraries.minecraft.net") },
                    rel);
            }

            { QFile f(cacheDst); if (!f.open(QIODevice::WriteOnly)) throw std::runtime_error("lib cache write failed"); f.write(data); }

            if (!lib.sha1.isEmpty() && sha1File(cacheDst) != lib.sha1) {
                QFile::remove(cacheDst);
                throw std::runtime_error(("Checksum mismatch for lib " + lib.path).toStdString());
            }

            // в инстанс
            if (!linkOrCopy(cacheDst, dst))
                throw std::runtime_error(("Cannot place lib to instance " + lib.path).toStdString());
        }

        if (lib.isNative) {
            const QString natDir = nativesPath(gameDir_, v.id);
            ensureDir(natDir);
            QProcess p;
            p.setProgram("unzip");
            p.setArguments({ "-o", dst, "-d", natDir });
            p.start();
            if (!p.waitForFinished(-1) || p.exitStatus()!=QProcess::NormalExit || p.exitCode()!=0) {
                const QString err = QString::fromUtf8(p.readAllStandardError());
                throw std::runtime_error(
                    QString("unzip failed for natives: %1 (exit %2) %3")
                        .arg(dst)
                        .arg(p.exitCode())
                        .arg(err)
                        .toStdString());
            }
        }
    }

    // 4) client.jar (как было, без изменений)
    qInfo() << "client.jar";
    const QString verDir   = joinPath(versionsPath(gameDir_), v.id);
    ensureDir(verDir);
    const QString clientJar = joinPath(verDir, v.id + ".jar");

    const auto clientObj = v.raw.value("downloads").toObject().value("client").toObject();
    const QString expectedSha = clientObj.value("sha1").toString();

    auto needDownload = [&](){
        if (!QFileInfo::exists(clientJar)) return true;
        if (expectedSha.isEmpty()) return false;
        return sha1File(clientJar) != expectedSha;
    };

    if (needDownload()) {
        QByteArray data;
        bool ok = false;

        try { data = api_.dl().getWithMirrors({ v.clientJarUrl }, QString()); ok = true; }
        catch (...) {}

        if (!ok && expectedSha.size() == 40) {
            const QString rel = "v1/objects/" + expectedSha + "/client.jar";
            try { data = api_.dl().getWithMirrors({ QUrl("https://piston-data.mojang.com") }, rel); ok = true; }
            catch (...) {}
        }
        if (!ok) {
            const QUrl bmcl(QString("https://bmclapi2.bangbang93.com/version/%1/client").arg(v.id));
            try { data = api_.dl().getWithMirrors({ bmcl }, QString()); ok = true; }
            catch (...) {}
        }
        if (!ok) throw std::runtime_error("Cannot download client.jar from all mirrors");

        { QFile f(clientJar); if (!f.open(QIODevice::WriteOnly)) throw std::runtime_error("write client.jar failed"); f.write(data); }
        if (!expectedSha.isEmpty() && sha1File(clientJar) != expectedSha) {
            QFile::remove(clientJar);
            throw std::runtime_error("Checksum mismatch for client.jar");
        }
    }

    // 5) version.json
    QFile vv(joinPath(verDir, v.id + ".json"));
    if (vv.open(QIODevice::WriteOnly))
        vv.write(QJsonDocument(v.raw).toJson());
}

// -------------------- classpath --------------------

QStringList Installer::classpathJars(const VersionResolved& v) const
{
    QStringList cp;
    for (const auto& lib : v.libraries)
        if (!lib.isNative)
            cp << joinPath(librariesPath(gameDir_), lib.path);
    cp << joinPath(versionsPath(gameDir_), v.id + "/" + v.id + ".jar");
    return cp;
}


QString Installer::installTemurinJre(int major, const QString& destBase, QString* outVersion)
{
#ifdef Q_OS_LINUX
    // Resolve arch/os
#if defined(Q_PROCESSOR_X86_64)
    const QString arch = "x64";
#elif defined(Q_PROCESSOR_ARM_64)
    const QString arch = "aarch64";
#else
    const QString arch = "x64";
#endif
    const QString os = "linux";

    // Build URL to Adoptium API (JRE, hotspot)
    const QString api = QString("https://api.adoptium.net/v3/binary/latest/%1/ga/%2/%3/jre/hotspot/normal/eclipse")
                        .arg(QString::number(major), os, arch);

    Net net;
    Downloader dl(net);
    QByteArray data = dl.getWithMirrors({QUrl(api)}, QString());

    // Save to cache file
    const QString cache = QDir(defaultCacheDir()).filePath(QString("temurin-jre-%1-%2-%3.tar.gz").arg(major).arg(os).arg(arch));
    {
        QFile f(cache);
        if (!f.open(QIODevice::WriteOnly))
            throw std::runtime_error(("Cannot write cache file: " + cache).toStdString());
        if (f.write(data) != data.size())
            throw std::runtime_error(("Short write cache file: " + cache).toStdString());
    }

    // Extract
    const QString runtimeBase = QDir(destBase).filePath(QString("java-%1").arg(major));
    QDir().mkpath(runtimeBase);
    QProcess tar;
    tar.start("tar", {"-xzf", cache, "-C", runtimeBase, "--strip-components=1"});
    if (!tar.waitForFinished(-1) || tar.exitStatus()!=QProcess::NormalExit || tar.exitCode()!=0) {
        const QString err = QString::fromUtf8(tar.readAllStandardError());
        throw std::runtime_error(
            QString("tar extract failed (exit %1): %2")
                .arg(tar.exitCode())
                .arg(err)
                .toStdString());
    }

    const QString javaPath = QDir(runtimeBase).filePath("bin/java");
    QFile fi(javaPath);
    fi.setPermissions(fi.permissions() | QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther);

    if (outVersion) *outVersion = QString::number(major);
    return javaPath;
#else
    Q_UNUSED(major); Q_UNUSED(destBase); Q_UNUSED(outVersion);
    return QString();
#endif
}
