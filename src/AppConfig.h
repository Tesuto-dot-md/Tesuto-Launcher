#pragma once
#include <QtCore>

struct AppConfig {
    QString gameDir;
    QString javaPath;
    QString version;
    bool    doLogin = false;
    QString msClientId;
    QString msTenant = "consumers";
    QString offlineName;     // offline-name из CLI
    QString accessToken;    // (онлайн) токен MC, если уже сохранён
    QString uuid;           // (онлайн)
    QString xuid;           // (онлайн)
    static AppConfig fromArgs(const QCoreApplication& app);
};
