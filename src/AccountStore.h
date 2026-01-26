#pragma once
#include <QtCore>

// Optional: store refresh tokens in the OS keychain.
#if defined(USE_QKEYCHAIN)
#  if __has_include(<QtKeychain/keychain.h>)
#    include <QtKeychain/keychain.h>
#  elif __has_include(<qt6keychain/keychain.h>)
#    include <qt6keychain/keychain.h>
#  elif __has_include(<qt5keychain/keychain.h>)
#    include <qt5keychain/keychain.h>
#  else
#    undef USE_QKEYCHAIN
#  endif
#endif

#ifdef USE_QKEYCHAIN
#  include <QEventLoop>
#endif

#include "Auth.h"

namespace AccountStore {

inline QString fileFor(const QString& gameDir) {
    return QDir(gameDir).filePath("account.json");
}

inline bool save(const QString& gameDir, const Account& a) {
    QJsonObject o;
    o.insert("playerName",     a.playerName);
    o.insert("accessToken",    a.accessToken);
    o.insert("uuid",           a.uuid);
    o.insert("xuid",           a.xuid);

#ifdef USE_QKEYCHAIN
    // Keep refresh token out of plaintext files.
    if (!a.uuid.isEmpty() && !a.msRefreshToken.isEmpty()) {
        QKeychain::WritePasswordJob job("TesutoLauncher");
        job.setAutoDelete(false);
        job.setKey("msRefreshToken:" + a.uuid);
        job.setTextData(a.msRefreshToken);
        QEventLoop loop;
        QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
        job.start();
        loop.exec();
        if (job.error())
            qWarning() << "[Keychain] write failed:" << job.errorString();
    }
#else
    o.insert("msRefreshToken", a.msRefreshToken);
#endif

    QFile f(fileFor(gameDir));
    QDir().mkpath(QFileInfo(f).dir().absolutePath());
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    return true;
}

inline std::optional<Account> load(const QString& gameDir) {
    QFile f(fileFor(gameDir));
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;

    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return std::nullopt;
    const auto o = doc.object();

    Account a;
    a.playerName     = o.value("playerName").toString();
    a.accessToken    = o.value("accessToken").toString();
    a.uuid           = o.value("uuid").toString();
    a.xuid           = o.value("xuid").toString();
    #ifdef USE_QKEYCHAIN
    if (!a.uuid.isEmpty()) {
        QKeychain::ReadPasswordJob job("TesutoLauncher");
        job.setAutoDelete(false);
        job.setKey("msRefreshToken:" + a.uuid);
        QEventLoop loop;
        QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
        job.start();
        loop.exec();
        if (!job.error()) a.msRefreshToken = job.textData();
    }
#else
    a.msRefreshToken = o.value("msRefreshToken").toString();
#endif
    if (a.playerName.isEmpty() || a.accessToken.isEmpty() || a.uuid.isEmpty())
        return std::nullopt;
    return a;
}

} // namespace AccountStore
