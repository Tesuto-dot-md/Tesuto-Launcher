#include "MojangAPI.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <cstdlib>

static QString mavenPathFromName(const QString& gav) {
    // "group:artifact:version" -> group/with/slashes/artifact/version/artifact-version.jar
    const auto parts = gav.split(':');
    if (parts.size() < 3) return {};
    QString group = parts[0];      // <-- копия
    group.replace('.', '/');       // теперь можно заменять
    const QString artifact = parts[1];
    const QString version  = parts[2];
    return QString("%1/%2/%3/%2-%3.jar").arg(group, artifact, version);
}

QList<VersionRef> MojangAPI::getVersionList() {
    QList<QUrl> candidates;
    if (const char* base = std::getenv("TESUTO_META_BASE")) {
        QString s = QString::fromUtf8(base);
        if (s.endsWith('/')) s.chop(1);
        candidates << QUrl(s + "/mc/game/version_manifest_v2.json");
    }
    candidates << QUrl("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json");
    candidates << QUrl("https://bmclapi2.bangbang93.com/mc/game/version_manifest_v2.json");

    Net::HeaderList h = { {"Accept", "application/json"}, {"Accept-Encoding", "identity"} };

    QJsonObject manifest;
    for (const auto& u : candidates) {
        try {
            manifest = net_.getJson(u, 12000, h);
            if (!manifest.isEmpty()) break;
        } catch (const std::exception& e) {
            qWarning() << "[manifest]" << u << "failed:" << e.what();
        }
    }
    if (manifest.isEmpty()) throw std::runtime_error("Cannot fetch version manifest");

    const auto vers = manifest.value("versions").toArray();
    QList<VersionRef> out;
    out.reserve(vers.size());
    for (const auto& vval : vers) {
        const auto o = vval.toObject();
        VersionRef v;
        v.id   = o.value("id").toString();
        v.url  = QUrl(o.value("url").toString());
        v.type = o.value("type").toString();
        if (!v.id.isEmpty() && v.url.isValid())
            out.push_back(v);
    }
    return out;
}

VersionResolved MojangAPI::resolveVersion(const VersionRef& ref) {
    Net::HeaderList h = { {"Accept", "application/json"}, {"Accept-Encoding", "identity"} };
    const auto vjson = net_.getJson(ref.url, 15000, h);

    VersionResolved r;
    r.id  = vjson.value("id").toString();
    r.raw = vjson;

    r.mainClass = vjson.value("mainClass").toString();

    const auto assetsObj = vjson.value("assetIndex").toObject();
    r.assetIndexId  = assetsObj.value("id").toString();
    r.assetIndexUrl = QUrl(assetsObj.value("url").toString());

    const auto downloads = vjson.value("downloads").toObject();
    const auto client    = downloads.value("client").toObject();
    r.clientJarUrl       = QUrl(client.value("url").toString());

    const auto libs = vjson.value("libraries").toArray();
    for (const auto& lval : libs) {
        const auto lo = lval.toObject();

        const auto dl = lo.value("downloads").toObject();
        const auto art = dl.value("artifact").toObject();
        if (!art.isEmpty()) {
            LibEntry e;
            e.url  = QUrl(art.value("url").toString());
            e.sha1 = art.value("sha1").toString();
            e.path = art.value("path").toString();
            if (e.path.isEmpty()) e.path = mavenPathFromName(lo.value("name").toString());
            e.isNative = false;
            r.libraries.push_back(e);
        }

        // classifiers (опционально: natives для Linux)
        const auto classifiers = dl.value("classifiers").toObject();
        if (!classifiers.isEmpty()) {
            const QString arch = QSysInfo::currentCpuArchitecture(); // "x86_64", "arm64", ...
            const QStringList keysTry = {
                "natives-linux-" + arch,
                "natives-linux-x86_64",
                "natives-linux-amd64",
                "natives-linux",
            };
            for (const auto& k : keysTry) {
                const auto cl = classifiers.value(k).toObject();
                if (cl.isEmpty()) continue;
                LibEntry n;
                n.url  = QUrl(cl.value("url").toString());
                n.sha1 = cl.value("sha1").toString();
                n.path = cl.value("path").toString();
                if (n.path.isEmpty()) n.path = mavenPathFromName(lo.value("name").toString());
                n.isNative = true;
                r.libraries.push_back(n);
                break;
            }
        }
    }

    return r;
}

VersionResolved MojangAPI::resolveVersion(const QString& versionId) {
    const auto all = getVersionList();
    for (const auto& v : all) {
        if (v.id == versionId) return resolveVersion(v);
    }
    // fallback BMCL
    const QUrl bmcl(QString("https://bmclapi2.bangbang93.com/version/%1").arg(versionId));
    Net::HeaderList h = { {"Accept", "application/json"}, {"Accept-Encoding", "identity"} };
    const auto vjson = net_.getJson(bmcl, 15000, h);

    VersionResolved r;
    r.id  = vjson.value("id").toString();
    r.raw = vjson;
    r.mainClass = vjson.value("mainClass").toString();

    const auto assetsObj = vjson.value("assetIndex").toObject();
    r.assetIndexId  = assetsObj.value("id").toString();
    r.assetIndexUrl = QUrl(assetsObj.value("url").toString());

    const auto downloads = vjson.value("downloads").toObject();
    const auto client    = downloads.value("client").toObject();
    r.clientJarUrl       = QUrl(client.value("url").toString());

    const auto libs = vjson.value("libraries").toArray();
    for (const auto& lval : libs) {
        const auto lo = lval.toObject();
        const auto dl = lo.value("downloads").toObject();
        const auto art = dl.value("artifact").toObject();
        if (!art.isEmpty()) {
            LibEntry e;
            e.url  = QUrl(art.value("url").toString());
            e.sha1 = art.value("sha1").toString();
            e.path = art.value("path").toString();
            if (e.path.isEmpty()) e.path = mavenPathFromName(lo.value("name").toString());
            e.isNative = false;
            r.libraries.push_back(e);
        }
    }
    return r;
}
