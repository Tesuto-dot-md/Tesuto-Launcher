#pragma once
#include <QtCore>
#include "Net.h"

// Учётка для запуска и хранения токенов
struct Account {
    QString playerName;     // ник
    QString uuid;           // uuid (без дефисов)
    QString accessToken;    // MC access token (Bearer для api.minecraftservices.com)
    QString xuid;           // Xbox user id (опционально)
    QString msRefreshToken; // <-- добавлено: refresh-токен Microsoft для последующих обновлений
};

class Auth {
public:
    // Порядок: clientId, tenant
    Auth(Net& net, QString clientId, QString tenant)
        : net_(net), clientId_(std::move(clientId)), tenant_(std::move(tenant)) {}

    // Полный интерактивный вход (Device Code → XBL → XSTS → MC)
    Account loginInteractive();

private:
    // 1) Запрос device_code
    QString msDeviceCode(QString& userCode, QString& verificationUri, int& intervalSec, int& expiresInSec);

    // 2) Обмен device_code на access/refresh
    std::pair<QString, QString> msTokenByDeviceCode(const QString& deviceCode,
                                                    int intervalSec,
                                                    int expiresInSec);

    // 3) XBL user.authenticate
    struct XblAuthOut { QString token; QString uhs; };
    static XblAuthOut xblAuth(Net& net, const QString& msAccessToken);

    // 4) XSTS authorize
    struct XstsOut { QString token; QString uhs; };
    static XstsOut xstsAuthorize(Net& net, const QString& xblToken);

    // 5) MC login_with_xbox → MC access token
    QString mcLoginWithXbox(Net& net, const QString& uhs, const QString& xstsToken);


    // 6) MC profile
    Account mcProfile(const QString& mcAccessToken);

private:
    Net& net_;
    QString clientId_;
    QString tenant_; // "consumers" | "common" | ваш tenant id
};
