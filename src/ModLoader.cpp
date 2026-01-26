#include "ModLoader.h"
#include "Net.h"
#include "Util.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QSslConfiguration>
#include <stdexcept>

// ─────────────────────────────────────────────
// Вспомогательные HTTP (синхронно, чтобы не трогать Net)
// ─────────────────────────────────────────────
static QByteArray httpGet(const QUrl& url, int timeoutMs = 60000)
{
    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "tesuto-launcher/1.0");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    // SSL по умолчанию
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    req.setSslConfiguration(ssl);

    QEventLoop loop;
    QNetworkReply* rep = nam.get(req);
    QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    if (timeoutMs > 0) {
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, [&]{
            if (rep) rep->abort();
            loop.quit();
        });
        timer.start(timeoutMs);
        loop.exec();
    } else {
        loop.exec();
    }

    if (rep->error() != QNetworkReply::NoError)
        throw std::runtime_error(QString("HTTP GET failed: %1 (%2)")
                                 .arg(url.toString(), rep->errorString()).toStdString());

    const QByteArray data = rep->readAll();
    rep->deleteLater();
    return data;
}

static void httpDownloadToFile(const QUrl& url, const QString& filePath, int timeoutMs = 60000)
{
    QByteArray data = httpGet(url, timeoutMs);
    QDir().mkpath(QFileInfo(filePath).path());
    QSaveFile f(filePath);
    if (!f.open(QIODevice::WriteOnly))
        throw std::runtime_error(QString("Cannot open file for write: %1").arg(filePath).toStdString());
    f.write(data);
    if (!f.commit())
        throw std::runtime_error(QString("Cannot commit file: %1").arg(filePath).toStdString());
}



// ─────────────────────────────────────────────
// ModloaderInstaller — общие хелперы
// ─────────────────────────────────────────────

QString ModloaderInstaller::librariesDir() const {
    return joinPath(gameDir_, "libraries");
}

QString ModloaderInstaller::mavenPathFromName(const QString& name) {
    // name = group:artifact:version
    const QStringList parts = name.split(':');
    if (parts.size() < 3) return {};
    QString group = parts[0];
    group.replace('.', '/');
    const QString artifact = parts[1];
    const QString version  = parts[2];
    return QString("%1/%2/%3/%2-%3.jar").arg(group, artifact, version);
}

QString ModloaderInstaller::ensureJar(const QString& baseUrl, const QString& name) {
    const QString rel = mavenPathFromName(name);
    if (rel.isEmpty())
        throw std::runtime_error(QString("Invalid maven name: %1").arg(name).toStdString());

    const QString abs = QDir(librariesDir()).filePath(rel);
    if (QFile::exists(abs))
        return rel;

    const QUrl url(QString("%1/%2").arg(baseUrl, rel));
    httpDownloadToFile(url, abs, 60000);
    return rel;
}

LoaderPatch ModloaderInstaller::installFromProfileJson(const QJsonObject& profile) {
    LoaderPatch patch;

    // mainClass
    patch.mainClass = profile.value("mainClass").toString();

    // libraries
    const auto libs = profile.value("libraries").toArray();
    for (const auto& v : libs) {
        const auto obj = v.toObject();
        const QString name = obj.value("name").toString();
        QString url = obj.value("url").toString();

        // sane defaults по группе
        if (url.isEmpty()) {
            if (name.startsWith("net.fabricmc:"))
                url = "https://maven.fabricmc.net";
            else if (name.startsWith("org.quiltmc:"))
                url = "https://maven.quiltmc.org/repository/release";
            else
                url = "https://libraries.minecraft.net";
        }

        // тянем jar (если нужно) и добавляем в CP (относительный путь от libraries)
        const QString rel = ensureJar(url, name);
        patch.classpath << rel;
    }

    // jvm args (берём только простые строки)
    const auto args = profile.value("arguments").toObject().value("jvm").toArray();
    for (const auto& a : args) {
        if (a.isString())
            patch.jvmArgs << a.toString();
    }

    return patch;
}

// ─────────────────────────────────────────────
// Fabric
// ─────────────────────────────────────────────

QJsonObject ModloaderInstaller::fetchFabricProfileJson(const QString& mcVersion,
                                                       const QString& loaderVersion)
{
    QString loader = loaderVersion.trimmed();
    if (loader.isEmpty() || loader == "latest") {
        // выберем последнюю стабильную
        const QUrl listUrl(QString("https://meta.fabricmc.net/v2/versions/loader/%1").arg(mcVersion));
        const auto arr = QJsonDocument::fromJson(httpGet(listUrl)).array();
        if (arr.isEmpty())
            throw std::runtime_error("Fabric: no loader versions for this MC");
        // ищем stable, иначе берём первую
        QString picked;
        for (const auto& v : arr) {
            const auto o = v.toObject();
            const auto l = o.value("loader").toObject();
            const bool stable = l.value("stable").toBool();
            const QString ver = l.value("version").toString();
            if (stable) { picked = ver; break; }
            if (picked.isEmpty()) picked = ver;
        }
        loader = picked;
    }

    const QUrl profUrl(QString("https://meta.fabricmc.net/v2/versions/loader/%1/%2/profile/json")
                       .arg(mcVersion, loader));
    const auto doc = QJsonDocument::fromJson(httpGet(profUrl));
    if (!doc.isObject())
        throw std::runtime_error("Fabric: invalid profile json");
    return doc.object();
}

LoaderPatch ModloaderInstaller::installFabric(const QString& mcVersion,
                                              const QString& loaderVersion)
{
    Q_UNUSED(net_);
    const auto profile = fetchFabricProfileJson(mcVersion, loaderVersion);
    return installFromProfileJson(profile);
}

// ─────────────────────────────────────────────
// Quilt
// ─────────────────────────────────────────────

QJsonObject ModloaderInstaller::fetchQuiltProfileJson(const QString& mcVersion,
                                                      const QString& loaderVersion)
{
    QString loader = loaderVersion.trimmed();
    if (loader.isEmpty() || loader == "latest") {
        // meta Quilt: берём первую стабильную
        const QUrl listUrl(QString("https://meta.quiltmc.org/v3/versions/loader/%1").arg(mcVersion));
        const auto arr = QJsonDocument::fromJson(httpGet(listUrl)).array();
        if (arr.isEmpty())
            throw std::runtime_error("Quilt: no loader versions for this MC");
        QString picked;
        for (const auto& v : arr) {
            const auto o = v.toObject();
            const auto l = o.value("loader").toObject();
            const bool stable = l.value("stable").toBool();
            const QString ver = l.value("version").toString();
            if (stable) { picked = ver; break; }
            if (picked.isEmpty()) picked = ver;
        }
        loader = picked;
    }

    const QUrl profUrl(QString("https://meta.quiltmc.org/v3/versions/loader/%1/%2/profile/json")
                       .arg(mcVersion, loader));
    const auto doc = QJsonDocument::fromJson(httpGet(profUrl));
    if (!doc.isObject())
        throw std::runtime_error("Quilt: invalid profile json");
    return doc.object();
}

LoaderPatch ModloaderInstaller::installQuilt(const QString& mcVersion,
                                             const QString& loaderVersion)
{
    Q_UNUSED(net_);
    const auto profile = fetchQuiltProfileJson(mcVersion, loaderVersion);
    return installFromProfileJson(profile);
}

// ─────────────────────────────────────────────
// Forge/NeoForge — пока заглушки
// ─────────────────────────────────────────────

LoaderPatch ModloaderInstaller::installForge(const QString&, const QString&) {
    throw std::runtime_error("Forge: установка пока не реализована");
}

LoaderPatch ModloaderInstaller::installNeoForge(const QString&, const QString&) {
    throw std::runtime_error("NeoForge: установка пока не реализована");
}
