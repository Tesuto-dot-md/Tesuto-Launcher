#pragma once
#include <QtCore>


inline QString sha1File(const QString &path) {
QFile f(path);
if (!f.open(QIODevice::ReadOnly)) return {};
QCryptographicHash h(QCryptographicHash::Sha1);
while (!f.atEnd()) h.addData(f.read(1<<20));
return h.result().toHex();
}


inline bool ensureDir(const QString &p) {
QDir d;
return d.mkpath(p);
}


inline QString joinPath(const QString &a, const QString &b) { return QDir(a).filePath(b); }


struct ScopeTimer {
QString what; QElapsedTimer t; ~ScopeTimer(){ qInfo() << what << "took" << t.elapsed() << "ms"; }
ScopeTimer(QString w):what(std::move(w)){ t.start(); }
};

inline QString offlineUuidFor(const QString &name) {
    // Спиготовская формула: UUIDv3 по MD5("OfflinePlayer:" + name)
    QByteArray ns = QByteArray("OfflinePlayer:") + name.toUtf8();
    QByteArray b = QCryptographicHash::hash(ns, QCryptographicHash::Md5);
    // Проставим биты версии/варианта RFC 4122
    b[6] = (b[6] & 0x0F) | 0x30; // version 3
    b[8] = (b[8] & 0x3F) | 0x80; // variant
    QUuid u = QUuid::fromRfc4122(b);
    return u.toString(QUuid::WithoutBraces);
}
