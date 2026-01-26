#include "Auth.h"
#include <QUrlQuery>
#include <QElapsedTimer>
#include <QThread>
#ifdef USE_QKEYCHAIN
#  if __has_include(<QtKeychain/keychain.h>)
#    include <QtKeychain/keychain.h>
#  elif __has_include(<qt6keychain/keychain.h>)
#    include <qt6keychain/keychain.h>
#  elif __has_include(<qt5keychain/keychain.h>)
#    include <qt5keychain/keychain.h>
#  else
#    warning "QtKeychain header not found, disabling keychain integration in this TU"
#    undef USE_QKEYCHAIN
#  endif
#endif
#ifdef USE_QKEYCHAIN
#  include <QEventLoop>
#endif

// -- helpers ---------------------------------------------------------------

static inline void addForm(QUrlQuery& q, const char* k, const QString& v) {
    q.addQueryItem(QString::fromUtf8(k), v);
}

static inline void addHdr(Net::HeaderList& h, const char* k, const char* v) {
    // Явно создаём std::pair, чтобы не было неоднозначности append(...)
    h.append(Net::Header(QByteArray(k), QByteArray(v)));
}

// -- 1) DEVICE CODE --------------------------------------------------------

QString Auth::msDeviceCode(QString& userCode, QString& verificationUri,
                           int& intervalSec, int& expiresInSec)
{
    const QUrl url(QString("https://login.microsoftonline.com/%1/oauth2/v2.0/devicecode")
                   .arg(tenant_));

    QUrlQuery form;
    addForm(form, "client_id",  clientId_);
    // Важно: scope обязателен
    addForm(form, "scope",      "XboxLive.signin offline_access");

    Net::HeaderList h;
    addHdr(h, "Content-Type", "application/x-www-form-urlencoded");

    const QJsonObject json = net_.postForm(url, form, 15000, h);
    if (json.isEmpty())
        throw std::runtime_error("DeviceCode: invalid JSON");

    const QString deviceCode   = json.value("device_code").toString();
    userCode       = json.value("user_code").toString();
    verificationUri= json.value("verification_uri").toString();
    intervalSec    = json.value("interval").toInt(5);
    expiresInSec   = json.value("expires_in").toInt(900);

    if (deviceCode.isEmpty() || userCode.isEmpty() || verificationUri.isEmpty())
        throw std::runtime_error("DeviceCode: missing fields");

    qInfo().noquote()
        << "[MS Login] Go to" << "\"" + verificationUri + "\""
        << "and enter code:" << "\"" + userCode + "\"";

    return deviceCode;
}

// -- 2) TOKEN BY DEVICE CODE ----------------------------------------------

std::pair<QString, QString>
Auth::msTokenByDeviceCode(const QString& deviceCode, int intervalSec, int expiresInSec)
{
    const QUrl turl(QString("https://login.microsoftonline.com/%1/oauth2/v2.0/token")
                    .arg(tenant_));

    Net::HeaderList h;
    addHdr(h, "Content-Type", "application/x-www-form-urlencoded");

    QElapsedTimer timer; timer.start();
    const int pollMs = qMax(1, intervalSec) * 1000;

    while (true) {
        QUrlQuery f;
        addForm(f, "grant_type",   "urn:ietf:params:oauth:grant-type:device_code");
        addForm(f, "client_id",    clientId_);
        addForm(f, "device_code",  deviceCode);

        QJsonObject tok;
        try {
            tok = net_.postForm(turl, f, 20000, h);
        } catch (...) {
            if (timer.elapsed() / 1000 > expiresInSec)
                throw;
            QThread::msleep(pollMs);
            continue;
        }

        if (tok.contains("access_token")) {
            const QString at = tok.value("access_token").toString();
            const QString rt = tok.value("refresh_token").toString();
            return {at, rt};
        }

        const QString err = tok.value("error").toString();
        if (err == "authorization_pending") {
            if (timer.elapsed() / 1000 > expiresInSec)
                throw std::runtime_error("DeviceCode: expired");
            QThread::msleep(pollMs);
            continue;
        } else if (err == "authorization_declined") {
            throw std::runtime_error("DeviceCode: authorization declined");
        } else if (err == "expired_token") {
            throw std::runtime_error("DeviceCode: token expired");
        } else if (err == "bad_verification_code") {
            throw std::runtime_error("DeviceCode: bad verification code");
        } else {
            throw std::runtime_error(("DeviceCode: token error: " + err).toStdString());
        }
    }
}

// -- 3) XBL AUTH -----------------------------------------------------------

Auth::XblAuthOut Auth::xblAuth(Net& net, const QString& msAccessToken)
{
    const QUrl url("https://user.auth.xboxlive.com/user/authenticate");

    QJsonObject body{
        {"Properties", QJsonObject{
            {"AuthMethod", "RPS"},
            {"SiteName",   "user.auth.xboxlive.com"},
            {"RpsTicket",  "d=" + msAccessToken}
        }},
        {"RelyingParty", "http://auth.xboxlive.com"},
        {"TokenType",    "JWT"}
    };

    Net::HeaderList h;
    addHdr(h, "Accept", "application/json");

    const QJsonObject resp = net.postJson(url, body, 20000, h);
    const QString token = resp.value("Token").toString();

    const auto xui = resp.value("DisplayClaims").toObject().value("xui").toArray();
    QString uhs;
    if (!xui.isEmpty())
        uhs = xui[0].toObject().value("uhs").toString();

    if (token.isEmpty() || uhs.isEmpty())
        throw std::runtime_error("XBL authenticate failed");

    return {token, uhs};
}

// -- 4) XSTS AUTHORIZE -----------------------------------------------------

Auth::XstsOut Auth::xstsAuthorize(Net& net, const QString& xblToken)
{
    const QUrl url("https://xsts.auth.xboxlive.com/xsts/authorize");

    QJsonObject body{
        {"Properties", QJsonObject{
            {"SandboxId",  "RETAIL"},
            {"UserTokens", QJsonArray{ xblToken }}
        }},
        {"RelyingParty", "rp://api.minecraftservices.com/"},
        {"TokenType",    "JWT"}
    };

    Net::HeaderList h;
    addHdr(h, "Accept", "application/json");

    const QJsonObject resp = net.postJson(url, body, 20000, h);
    const QString token = resp.value("Token").toString();

    const auto xui = resp.value("DisplayClaims").toObject().value("xui").toArray();
    QString uhs;
    if (!xui.isEmpty())
        uhs = xui[0].toObject().value("uhs").toString();

    if (token.isEmpty() || uhs.isEmpty())
        throw std::runtime_error("XSTS authorize failed");

    return {token, uhs};
}

// -- 5) MC LOGIN WITH XBOX -------------------------------------------------

QString Auth::mcLoginWithXbox(Net& net, const QString& uhs, const QString& xstsToken)
{
    const QUrl url("https://api.minecraftservices.com/authentication/login_with_xbox");

    // identityToken строго "XBL3.0 x=<uhs>;<xsts>"
    const QString idToken = QString("XBL3.0 x=%1;%2").arg(uhs, xstsToken);
    QJsonObject body{
        {"identityToken", idToken},
        {"ensureLegacyEnabled", true}
    };

    // Обязательные заголовки
    Net::HeaderList h;
    h.push_back({QByteArray("Accept"),       QByteArray("application/json")});
    h.push_back({QByteArray("Content-Type"), QByteArray("application/json")});
    h.push_back({QByteArray("User-Agent"),   QByteArray("tesuto-launcher/0.1")});

    const auto resp = net.postJson(url, body, 20000, h);

    // Ожидаем "access_token"
    const auto token = resp.value("access_token").toString();
    if (token.isEmpty()) {
        // поможем себе диагностикой
        throw std::runtime_error(
            QString("login_with_xbox: no access_token. Body=%1")
            .arg(QString::fromUtf8(QJsonDocument(resp).toJson(QJsonDocument::Compact)))
            .toStdString());
    }
    return token;
}

// -- 6) MC PROFILE ---------------------------------------------------------

Account Auth::mcProfile(const QString& mcAccessToken)
{
    const QUrl url("https://api.minecraftservices.com/minecraft/profile");

    Net::HeaderList h;
    h.append(Net::Header("Authorization", QByteArray("Bearer ").append(mcAccessToken.toUtf8())));

    const QJsonObject json = net_.getJson(url, 20000, h);

    Account a;
    a.playerName  = json.value("name").toString();
    a.uuid        = json.value("id").toString();
    a.accessToken = mcAccessToken;
    return a;
}

// -- PUBLIC: полный интерактивный вход -------------------------------------

Account Auth::loginInteractive()
{
    // 1) Device code
    QString userCode, verifyUri;
    int intervalSec = 5, expiresSec = 900;
    const QString deviceCode = msDeviceCode(userCode, verifyUri, intervalSec, expiresSec);

    // 2) Ждём MS токены
    QString msAccess, msRefresh;
    std::tie(msAccess, msRefresh) = msTokenByDeviceCode(deviceCode, intervalSec, expiresSec);
    qInfo().noquote() << "[MS Login] Got MS access token (len =" << msAccess.size() << ")";

    // 3) XBL
    auto xa = xblAuth(net_, msAccess);
    qInfo().noquote() << "[MS Login] XBL ok, uhs =" << "\"" + xa.uhs + "\"";

    // 4) XSTS
    auto xs = xstsAuthorize(net_, xa.token);
    qInfo().noquote() << "[MS Login] XSTS ok, uhs =" << "\"" + xs.uhs + "\"";

    // 5) MC access token
    const QString mcAccess = mcLoginWithXbox(net_, xs.uhs, xs.token);
    qInfo().noquote() << "[MS Login] Got MC access token (len =" << mcAccess.size() << ")";

    // 6) Профиль + прикладываем refresh-токен MS
    Account acc = mcProfile(mcAccess);
    acc.xuid = QString();              // можно сохранить XUID, если вытягиваете
    #ifdef USE_QKEYCHAIN
    if (!acc.uuid.isEmpty()) {
        QKeychain::WritePasswordJob job("TesutoLauncher");
        job.setAutoDelete(false);
        job.setKey("msRefreshToken:" + acc.uuid);
        job.setTextData(msRefresh);
        QEventLoop loop;
        QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
        job.start();
        loop.exec();
        if (job.error()) {
            qWarning() << "[Keychain] write failed:" << job.errorString();
        }
    }
#else
    acc.msRefreshToken = msRefresh;
#endif    // <-- теперь это поле существует
    return acc;
}
