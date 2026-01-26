#include "MainWindow.h"

#include <QScreen>
#include <QTimer>
#include <QElapsedTimer>
#include <QtWidgets>
#include <QtConcurrent>
#include <QDesktopServices>
#include <QScrollBar>
#include <QSettings>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QImage>
#include <QPixmap>
#include <QPainter>
#include <QPainterPath>
#include <QEvent>
#include <QRandomGenerator>
#include <QFontMetrics>
#include <QCryptographicHash>
#include <QUuid>
#include <QInputDialog>
#include <optional>
#include <functional>
#include <QDateTime>
#include <QToolTip>
#include <QStandardPaths>
#include <QApplication>
#include <QPointer>
#include <QUrl>
#include <QUrlQuery>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QMetaObject>
#include <QCloseEvent>
#include <QMenu>
#include <QRegularExpression>

// HTTP / OAuth
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>

#include "../LoaderPatchIO.h"
#include "../Net.h"
#include "../MojangAPI.h"
#include "../Installer.h"
#include "../Launcher.h"
#include "../InstanceStore.h"
#include "../Settings.h"
#include "../ModLoader.h"
#include "../Util.h"

#include "CreateInstanceDialog.h"
#include "SettingsDialog.h"
#include "InstanceEditorDialog.h"
#include "JavaUtil.h"

static QStringList sanitizeJvmArgs(const QStringList& in)
{
    QStringList out;
    QRegularExpression re(R"(^-(?:Xmx|Xms)\b)", QRegularExpression::CaseInsensitiveOption);
    for (const auto& a : in) {
        if (!re.match(a).hasMatch())
            out << a;
    }
    return out;
}

// ====================== Вспомогательные объявления ======================
static QString readGameDir();
static QPixmap makeAvatarPixmap(const QString&, int);
static void appendLog(const QWidget* root, const QString& s);

// RAII-гарда для гарантированного endBusy() из фоновых задач
struct UiBusyGuard {
    MainWindow* w{};
    ~UiBusyGuard() {
        if (!w) return;
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        // Типобезопасный вызов слота в GUI-потоке
        QMetaObject::invokeMethod(w, &MainWindow::endBusy, Qt::QueuedConnection);
#else
        // Для старых Qt: строковый invoke (endBusy объявлен как public slot)
        QMetaObject::invokeMethod(w, "endBusy", Qt::QueuedConnection);
#endif
    }
};

// ====================== Константы MSA ======================
// По умолчанию используем client_id официального Minecraft Launcher (удобно для dev/тестов).
// В проде лучше зарегистрировать свою Azure App и использовать её client_id.
static const QString kMsaClientIdDefault = QStringLiteral("00000000402B5328");
static const QString kMsaScopeDefault    = QStringLiteral("XboxLive.signin offline_access");
static const QString kMsaTenantDefault   = QStringLiteral("consumers");

static QString readMsaClientId() {
    QSettings s;
    return s.value("msa/clientId", kMsaClientIdDefault).toString().trimmed();
}
static QString readMsaScope() {
    QSettings s;
    return s.value("msa/scope", kMsaScopeDefault).toString().trimmed();
}
static QString readMsaTenant() {
    QSettings s;
    return s.value("msa/tenant", kMsaTenantDefault).toString().trimmed();
}

// ====================== Модель аккаунта ======================
struct Account {
    QString id;           // локальный id записи
    QString kind;         // "msa" | "offline"
    QString name;         // видимое имя игрока
    QString uuid;         // UUID без {}
    QString accessToken;  // для msa
    QString refreshToken; // для msa
    qint64  expiresAt = 0;

    QJsonObject toJson() const {
        QJsonObject o;
        o["id"]=id; o["kind"]=kind; o["name"]=name; o["uuid"]=uuid;
        if (!accessToken.isEmpty())  o["accessToken"]=accessToken;
        if (!refreshToken.isEmpty()) o["refreshToken"]=refreshToken;
        if (expiresAt>0)             o["expiresAt"]=double(expiresAt);
        return o;
    }
    static Account fromJson(const QJsonObject& o) {
        Account a;
        a.id = o.value("id").toString();
        a.kind = o.value("kind").toString();
        a.name = o.value("name").toString();
        a.uuid = o.value("uuid").toString();
        a.accessToken  = o.value("accessToken").toString();
        a.refreshToken = o.value("refreshToken").toString();
        a.expiresAt    = (qint64)o.value("expiresAt").toDouble(0);
        if (a.id.isEmpty()) a.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        return a;
    }
};

struct AccountsState {
    QVector<Account> list;
    QString activeId;
};

static QString accountsFilePath() {
    return QDir(readGameDir()).filePath("accounts.json");
}
static AccountsState loadAccountsState() {
    AccountsState s;
    QFile f(accountsFilePath());
    if (f.open(QIODevice::ReadOnly)) {
        const auto doc = QJsonDocument::fromJson(f.readAll());
        if (doc.isObject()) {
            const auto root = doc.object();
            s.activeId = root.value("activeId").toString();
            for (const auto& v : root.value("accounts").toArray())
                if (v.isObject()) s.list.push_back(Account::fromJson(v.toObject()));
        }
    }
    return s;
}
static bool saveAccountsState(const AccountsState& s) {
    QJsonArray arr;
    for (const auto& a : s.list) arr.append(a.toJson());
    QJsonObject root;
    root["activeId"] = s.activeId;
    root["accounts"] = arr;
    QFile f(accountsFilePath());
    QDir().mkpath(QFileInfo(f).dir().absolutePath());
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}
static std::optional<Account> activeAccount() {
    auto s = loadAccountsState();
    for (const auto& a : s.list) if (a.id == s.activeId) return a;
    return std::nullopt;
}

// ====================== HTTP JSON ======================
static QJsonObject httpPostJson(const QUrl& url, const QJsonObject& payload,
                                const QList<QPair<QByteArray,QByteArray>>& headers = {}) {
    QNetworkAccessManager nam; QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    for (const auto& h : headers) req.setRawHeader(h.first, h.second);
    QEventLoop loop;
    QNetworkReply* rp = nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(rp, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QJsonObject obj;
    const QByteArray raw = rp->readAll();
    if (rp->error() == QNetworkReply::NoError) {
        const auto doc = QJsonDocument::fromJson(raw);
        if (doc.isObject()) obj = doc.object();
    } else {
        obj.insert("error", QString("network: %1").arg(rp->errorString()));
    }
    // Добавим сырой ответ для диагностики (не сохраняем на диск)
    if (!raw.isEmpty()) obj.insert("_raw", QString::fromUtf8(raw.left(2000)));
    rp->deleteLater();
    return obj;
}
static QJsonObject httpGetJson(const QUrl& url,
                               const QList<QPair<QByteArray,QByteArray>>& headers = {}) {
    QNetworkAccessManager nam; QNetworkRequest req(url);
    for (const auto& h : headers) req.setRawHeader(h.first, h.second);
    QEventLoop loop;
    QNetworkReply* rp = nam.get(req);
    QObject::connect(rp, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QJsonObject obj;
    if (rp->error() == QNetworkReply::NoError) {
        const auto doc = QJsonDocument::fromJson(rp->readAll());
        if (doc.isObject()) obj = doc.object();
    } else {
        obj.insert("error", QString("network: %1").arg(rp->errorString()));
    }
    rp->deleteLater();
    return obj;
}

// ====================== MSA → XBL → XSTS → MC ======================
static std::optional<Account> exchangeMsaToMinecraft(const QString& msaAccess,
                                                     const QString& msaRefresh,
                                                     int /*msaExpiresIn*/) {
    // 1) XBL
    QJsonObject xblReq{
        {"Properties", QJsonObject{
            {"AuthMethod","RPS"},
            {"SiteName","user.auth.xboxlive.com"},
            {"RpsTicket", QString("d=%1").arg(msaAccess)}
        }},
        {"RelyingParty","http://auth.xboxlive.com"},
        {"TokenType","JWT"}
    };
    auto xbl = httpPostJson(QUrl("https://user.auth.xboxlive.com/user/authenticate"), xblReq,
                            {{"Accept","application/json"}});
    const QString xblToken = xbl.value("Token").toString();
    const auto xuiArr = xbl.value("DisplayClaims").toObject().value("xui").toArray();
    const QString uhs = xuiArr.isEmpty() ? QString() : xuiArr.first().toObject().value("uhs").toString();
    if (xblToken.isEmpty() || uhs.isEmpty()) return std::nullopt;

    // 2) XSTS
    QJsonObject xstsReq{
        {"Properties", QJsonObject{
            {"SandboxId","RETAIL"},
            {"UserTokens", QJsonArray{ xblToken }}
        }},
        {"RelyingParty","rp://api.minecraftservices.com/"},
        {"TokenType","JWT"}
    };
    auto xsts = httpPostJson(QUrl("https://xsts.auth.xboxlive.com/xsts/authorize"), xstsReq,
                             {{"Accept","application/json"}});
    const QString xstsToken = xsts.value("Token").toString();
    if (xstsToken.isEmpty()) return std::nullopt;

    // 3) MC login_with_xbox
    QJsonObject mcReq{
        {"identityToken", QString("XBL3.0 x=%1;%2").arg(uhs, xstsToken)},
        {"ensureLegacyEnabled", true}
    };
    auto mcAuth = httpPostJson(QUrl("https://api.minecraftservices.com/authentication/login_with_xbox"), mcReq,
                               {{"Accept","application/json"},
                                {"User-Agent","tesuto-launcher/0.1"}});
    const QString mcAccess = mcAuth.value("access_token").toString();
    const int     mcExpIn  = mcAuth.value("expires_in").toInt(3600);
    if (mcAccess.isEmpty()) return std::nullopt;

    // 4) Профиль
    auto profile = httpGetJson(QUrl("https://api.minecraftservices.com/minecraft/profile"),
                               {{"Authorization", QString("Bearer %1").arg(mcAccess).toUtf8()}});
    const QString mcUuid = profile.value("id").toString();
    const QString mcName = profile.value("name").toString();
    if (mcUuid.isEmpty() || mcName.isEmpty()) return std::nullopt;

    Account a;
    a.id           = QUuid::createUuid().toString(QUuid::WithoutBraces);
    a.kind         = "msa";
    a.name         = mcName;
    a.uuid         = mcUuid;
    a.accessToken  = mcAccess;
    a.refreshToken = msaRefresh;
    a.expiresAt    = QDateTime::currentMSecsSinceEpoch() + (qint64)mcExpIn*1000;
    return a;
}

static std::optional<Account> refreshMsaAccount(const Account& cur) {
    if (cur.kind != "msa" || cur.refreshToken.isEmpty()) return std::nullopt;

    const QString tenant = readMsaTenant();
    QUrl url(QString("https://login.microsoftonline.com/%1/oauth2/v2.0/token").arg(tenant));
    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

    QUrlQuery body;
    body.addQueryItem("grant_type",    "refresh_token");
    body.addQueryItem("client_id",     readMsaClientId());
    body.addQueryItem("refresh_token", cur.refreshToken);
    body.addQueryItem("scope",         readMsaScope());

    QEventLoop loop;
    QNetworkReply* rp = nam.post(req, body.query(QUrl::FullyEncoded).toUtf8());
    QObject::connect(rp, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (rp->error() != QNetworkReply::NoError) { rp->deleteLater(); return std::nullopt; }
    const auto doc = QJsonDocument::fromJson(rp->readAll());
    rp->deleteLater();
    if (!doc.isObject()) return std::nullopt;
    const auto o = doc.object();

    const QString msaAccess  = o.value("access_token").toString();
    const QString msaRefresh = o.value("refresh_token").toString(cur.refreshToken);
    const int     msaExpIn   = o.value("expires_in").toInt(3600);
    if (msaAccess.isEmpty()) return std::nullopt;

    auto acc = exchangeMsaToMinecraft(msaAccess, msaRefresh, msaExpIn);
    if (!acc) return std::nullopt;
    acc->id = cur.id; // сохранить id записи
    return acc;
}

// ====================== Вспомогательные UI функции ======================
static QTextEdit* findLog(const QWidget* root) {
    if (!root) return nullptr;
    return root->findChild<QTextEdit*>("logView");
}
static void appendLog(const QWidget* root, const QString& s) {
    if (!root) { qDebug().noquote() << s; return; }
    if (auto* te = findLog(root)) {
        te->append(s);
        if (auto* sb = te->verticalScrollBar())
            sb->setValue(sb->maximum());
    } else {
        qDebug().noquote() << s;
    }
}

// ====================== Метаданные инстанса и иконки ======================
static QString instanceMetaPath(const QString& instanceDir) {
    return QDir(instanceDir).filePath("instance.json");
}
static QJsonObject loadInstanceMeta(const QString& instanceDir) {
    QFile f(instanceMetaPath(instanceDir));
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return QJsonObject{};
    const auto doc = QJsonDocument::fromJson(f.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

// ====================== Маркер установки инстанса ======================
// Чтобы не прогонять установщик при каждом запуске, пишем небольшой маркер.
static QString installMarkerPath(const QString& instanceDir) {
    return QDir(instanceDir).filePath(".tesuto_installed.json");
}
static QJsonObject loadInstallMarker(const QString& instanceDir) {
    QFile f(installMarkerPath(instanceDir));
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return {};
    const auto doc = QJsonDocument::fromJson(f.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}
static void saveInstallMarker(const QString& instanceDir,
                              const QString& versionId,
                              const QJsonObject& modloader) {
    QJsonObject o;
    o["versionId"] = versionId;
    o["modloader"] = modloader;
    o["installedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QFile f(installMarkerPath(instanceDir));
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
}
static bool markerMatches(const QJsonObject& marker,
                          const QString& versionId,
                          const QJsonObject& modloader) {
    if (marker.value("versionId").toString() != versionId) return false;
    const auto mml = marker.value("modloader").toObject();
    return (mml.value("kind").toString() == modloader.value("kind").toString())
        && (mml.value("version").toString() == modloader.value("version").toString());
}
static void saveInstanceMeta(const QString& instanceDir, const QJsonObject& o) {
    QFile f(instanceMetaPath(instanceDir));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
        f.close();
    }
}
static QString displayKindFromMeta(const QJsonObject& meta) {
    const QString k = meta.value("modloader").toObject().value("kind").toString();
    if (k == "fabric")   return "Fabric";
    if (k == "quilt")    return "Quilt";
    if (k == "forge")    return "Forge";
    if (k == "neoforge") return "NeoForge";
    return "Vanilla";
}
static QString decorateTitleWithModloader(const QString& base, const QJsonObject& meta) {
    const auto ml = meta.value("modloader").toObject();
    const QString k = ml.value("kind").toString();
    if (k.isEmpty() || k == "none") return base;
    const QString ver = ml.value("version").toString();
    return base + QString(" • %1%2").arg(k).arg(ver.isEmpty() ? "" : ("@" + ver));
}

static QPixmap roundedPixmap(const QPixmap& src, int radius) {
    if (src.isNull()) return {};
    QPixmap dst(src.size());
    dst.fill(Qt::transparent);
    QPainter p(&dst);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(dst.rect(), radius, radius);
    p.setClipPath(path);
    p.drawPixmap(0, 0, src);
    return dst;
}
static QColor colorFromString(const QString& s) {
    const uint h = qHash(s);
    return QColor::fromHsv(int(h % 360), 170, 230);
}
static QPixmap makeInstancePlaceholder(const QString& title,
                                       int size,
                                       int radius,
                                       const QString&)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath path;
    path.addRoundedRect(QRect(0,0,size,size), radius, radius);
    p.fillPath(path, colorFromString(title));

    QFont f = p.font();
    f.setBold(true);
    f.setPointSizeF(size * 0.42);
    p.setFont(f);
    p.setPen(Qt::white);
    p.drawText(QRect(0,0,size,size), Qt::AlignCenter, title.left(1).toUpper());

    return pm;
}
static QIcon instanceIconOrPlaceholder(const QString& instDir,
                                       const QString& title,
                                       int size, int radius,
                                       const QString& kindLabel,
                                       const QString& kindRaw)
{
    QPixmap content(size, size);
    content.fill(Qt::transparent);

    QPixmap base;
    const QString iconPath = QDir(instDir).filePath("icon.png");
    if (QFile::exists(iconPath)) {
        base.load(iconPath);
        if (!base.isNull())
            base = base.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }
    if (base.isNull()) {
        base = makeInstancePlaceholder(title, size, radius, instDir);
    }

    {
        QPainter p(&content);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.drawPixmap(0, 0, roundedPixmap(base, radius));

        if (!kindLabel.isEmpty()) {
            const int pad = 6;
            const int h   = 20;
            QFont f = p.font();
            f.setBold(true);
            f.setPointSizeF(9);
            p.setFont(f);

            const QString text = kindLabel;
            const QFontMetrics fm(f);
            const int w = fm.horizontalAdvance(text) + 12;

            QRect r(pad, pad, w, h);

            QColor bg(0,0,0,160);
            if (kindRaw == "fabric")   bg = QColor( 76,175, 80, 200);
            if (kindRaw == "quilt")    bg = QColor(  3,169,244, 200);
            if (kindRaw == "forge")    bg = QColor(255,152,  0, 200);
            if (kindRaw == "neoforge") bg = QColor(156, 39,176, 200);
            if (kindRaw.isEmpty() || kindRaw=="none") bg = QColor(120,120,120, 200);

            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            QPainterPath badge;
            badge.addRoundedRect(r, 6, 6);
            p.drawPath(badge);

            p.setPen(Qt::white);
            p.drawText(r, Qt::AlignCenter, text);
        }
    }

    return QIcon(content);
}

// ====================== Аватар и профиль ======================
static QString currentPlayerName() {
    if (auto a = activeAccount()) return a->name;
    return "Player";
}
static QPixmap makeAvatarPixmap(const QString& playerName, int size) {
    return makeInstancePlaceholder(playerName.isEmpty() ? "Player" : playerName, size, int(size*0.2), QString());
}

// ====================== Auth JSON writer ======================
struct McSession { QString accessToken, uuid, name, xuid; QString userType; };

static void writeAuthJsonForLaunch(const QString& instDir, const McSession& s) {
    QJsonObject auth;
    auth["name"]           = s.name;
    auth["uuid"]           = s.uuid;
    auth["accessToken"]    = s.accessToken;
    auth["userType"]       = s.userType.isEmpty() ? "msa" : s.userType;
    auth["userProperties"] = "{}";
    auth["xuid"]           = s.xuid;

    // плейсхолдеры для аргсабстов
    auth["auth_player_name"]  = s.name;
    auth["auth_uuid"]         = s.uuid;
    auth["auth_access_token"] = s.accessToken;
    auth["user_type"]         = auth["userType"];
    auth["user_properties"]   = "{}";
    auth["auth_xuid"]         = s.xuid;

    QFile f(QDir(instDir).filePath("auth.json"));
    QDir().mkpath(QFileInfo(f).dir().absolutePath());
    if (!f.open(QIODevice::WriteOnly))
        throw std::runtime_error(("Cannot write auth.json: " + f.fileName()).toStdString());
    const QByteArray bytes = QJsonDocument(auth).toJson(QJsonDocument::Compact);
    if (f.write(bytes) != bytes.size())
        throw std::runtime_error(("Short write auth.json: " + f.fileName()).toStdString());
    f.close();

    qInfo().noquote() << "auth.json → name=" << s.name
                      << "uuid=" << s.uuid
                      << "tokenLen=" << s.accessToken.size()
                      << "type=" << s.userType;
}

// ====================== Оффлайн UUID из имени ======================
static QString uuidFromOfflineName(const QString& name) {
    // UUID v3 от "OfflinePlayer:<name>"
    QByteArray base = QByteArray("OfflinePlayer:") + name.toUtf8();
    QByteArray md5  = QCryptographicHash::hash(base, QCryptographicHash::Md5);
    // правим биты под RFC4122 (v3, variant)
    QByteArray h = md5;
    uchar* d = reinterpret_cast<uchar*>(h.data());
    d[6] = (d[6] & 0x0F) | 0x30; // version 3
    d[8] = (d[8] & 0x3F) | 0x80; // variant RFC 4122
    const QString hex = h.toHex();
    return QString("%1-%2-%3-%4-%5")
            .arg(hex.mid(0,8))
            .arg(hex.mid(8,4))
            .arg(hex.mid(12,4))
            .arg(hex.mid(16,4))
            .arg(hex.mid(20,12));
}

// ====================== Настройки путей по умолчанию ======================
static QString defaultGameDir() {
#ifdef Q_OS_LINUX
    QString base = qEnvironmentVariable("XDG_DATA_HOME");
    if (base.isEmpty()) base = QDir::homePath() + "/.local/share";
    return base + "/tesuto-launcher";
#else
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#endif
}
static QString defaultJavaPath() {
#ifdef Q_OS_LINUX
    return "/usr/bin/java";
#else
    return "java";
#endif
}
static QString readGameDir() {
    QSettings s;
    return s.value("paths/gameDir", defaultGameDir()).toString();
}
static QString readJavaPath() {
    QSettings s; // уже после унификации Org/AppName — без ручных строк
    QString p = s.value("paths/javaPath").toString();
#ifdef Q_OS_LINUX
    if (p.isEmpty()) p = "/usr/bin/java";
#endif
    if (!QFileInfo(p).isExecutable()) {
        auto autod = JavaUtil::detectJava(17);
        if (autod.has_value()) {
            p = autod.value();
            s.setValue("java/defaultPath", p);
        }
    }
    return p;
}


// ====================== Диалог Device Code ======================
class DeviceCodeDialog : public QDialog {
    Q_OBJECT
public:
    DeviceCodeDialog(const QJsonObject& deviceObj,
                     const QString& tenant,
                     const QString& clientId,
                     QWidget* parent=nullptr)
        : QDialog(parent),
          tenant_(tenant),
          clientId_(clientId)
    {
        setWindowTitle(tr("Вход через Microsoft"));
        auto* v = new QVBoxLayout(this);

        const QString verification_uri = deviceObj.value("verification_uri").toString("https://microsoft.com/devicelogin");
        const QString user_code        = deviceObj.value("user_code").toString();
        const QString message          = deviceObj.value("message").toString();
        deviceCode_ = deviceObj.value("device_code").toString();
        const int intervalSec          = deviceObj.value("interval").toInt(5);
        const int expiresSec           = deviceObj.value("expires_in").toInt(900);

        pollIntervalMs_ = qMax(2, intervalSec) * 1000;
        expiresAtMs_    = QDateTime::currentMSecsSinceEpoch() + (qint64)expiresSec*1000;

        auto* msgTop = new QLabel(message.isEmpty()
            ? tr("Откройте сайт и введите код:")
            : message, this);
        msgTop->setWordWrap(true);

        auto* url = new QLabel(QString("<a href=\"%1\">%1</a>").arg(verification_uri), this);
        url->setTextFormat(Qt::RichText);
        url->setTextInteractionFlags(Qt::TextBrowserInteraction);
        url->setOpenExternalLinks(true);

        auto* codeLabel = new QLabel(tr("Код подтверждения:"), this);
        codeEdit_ = new QLineEdit(user_code, this);
        codeEdit_->setReadOnly(true);
        codeEdit_->setCursorPosition(0);

        auto* row = new QHBoxLayout;
        auto* btnOpen  = new QPushButton(tr("Открыть сайт"), this);
        auto* btnCopy  = new QPushButton(tr("Скопировать код"), this);
        row->addWidget(btnOpen);
        row->addStretch();
        row->addWidget(btnCopy);

        connect(btnOpen, &QPushButton::clicked, this, [verification_uri]{
            QDesktopServices::openUrl(QUrl(verification_uri));
        });
        connect(btnCopy, &QPushButton::clicked, this, [this]{
            codeEdit_->selectAll();
            codeEdit_->copy();
            QToolTip::showText(QCursor::pos(), tr("Скопировано"));
        });

        status_ = new QLabel(tr("Ожидаем подтверждение входа…"), this);
        status_->setStyleSheet("color: gray");

        auto* btns = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);

        v->addWidget(msgTop);
        v->addWidget(url);
        v->addSpacing(8);
        v->addWidget(codeLabel);
        v->addWidget(codeEdit_);
        v->addLayout(row);
        v->addWidget(status_);
        v->addWidget(btns);

        resize(560, 0);

        // таймер поллинга
        pollTimer_ = new QTimer(this);
        pollTimer_->setInterval(pollIntervalMs_);
        connect(pollTimer_, &QTimer::timeout, this, &DeviceCodeDialog::tryPollToken);
        pollTimer_->start();
    }

signals:
    void loginCompleted(const Account& a);
    void loginFailed(const QString& error);

private slots:
    void tryPollToken() {
        if (QDateTime::currentMSecsSinceEpoch() >= expiresAtMs_) {
            status_->setText(tr("Код истёк. Закройте окно и попробуйте снова."));
            pollTimer_->stop();
            return;
        }

        // POST /token
        QUrl url(QString("https://login.microsoftonline.com/%1/oauth2/v2.0/token").arg(tenant_));
        QNetworkAccessManager nam;
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

        QUrlQuery body;
        body.addQueryItem("grant_type",  "urn:ietf:params:oauth:grant-type:device_code");
        body.addQueryItem("client_id",   clientId_);
        body.addQueryItem("device_code", deviceCode_);

        QEventLoop loop;
        QNetworkReply* rp = nam.post(req, body.query(QUrl::FullyEncoded).toUtf8());
        connect(rp, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        if (rp->error() != QNetworkReply::NoError) {
            status_->setText(tr("Сеть: %1").arg(rp->errorString()));
            rp->deleteLater();
            return;
        }
        const auto doc = QJsonDocument::fromJson(rp->readAll());
        rp->deleteLater();
        if (!doc.isObject()) return;
        const auto o = doc.object();

        if (o.contains("error")) {
            const QString err = o.value("error").toString();
            if (err == "authorization_pending") {
                return;
            } else if (err == "slow_down") {
                pollIntervalMs_ += 2000;
                pollTimer_->setInterval(pollIntervalMs_);
                status_->setText(tr("Слишком частые запросы, замедляемся…"));
                return;
            } else if (err == "expired_token" || err == "authorization_declined") {
                pollTimer_->stop();
                status_->setText(tr("Вход отклонён или код истёк."));
                emit loginFailed(err);
                return;
            } else {
                pollTimer_->stop();
                status_->setText(tr("Ошибка: %1").arg(o.value("error_description").toString()));
                emit loginFailed(o.value("error").toString());
                return;
            }
        }

        // получили MSA access_token/refresh_token
        const QString msaAccess  = o.value("access_token").toString();
        const QString msaRefresh = o.value("refresh_token").toString();
        const int     msaExpIn   = o.value("expires_in").toInt(3600);
        if (msaAccess.isEmpty()) return;

        pollTimer_->stop();
        status_->setText(tr("Вход подтверждён. Получаем доступ к Minecraft…"));

        auto maybeAcc = exchangeMsaToMinecraft(msaAccess, msaRefresh, msaExpIn);
        if (!maybeAcc.has_value()) {
            emit loginFailed(tr("Не удалось получить профиль Minecraft."));
            status_->setText(tr("Не удалось получить профиль Minecraft."));
            return;
        }
        emit loginCompleted(*maybeAcc);
        accept();
    }

private:
    QString tenant_;
    QString clientId_;
    QString deviceCode_;
    qint64  expiresAtMs_{0};
    int     pollIntervalMs_{5000};

    QLineEdit* codeEdit_{};
    QLabel*    status_{};
    QTimer*    pollTimer_{};
};

// ====================== Диалог аккаунтов (MSA + оффлайн) ======================
class AccountsDialog : public QDialog {
    Q_OBJECT
public:
    using MicrosoftLoginFn = std::function<std::optional<Account>()>;

    explicit AccountsDialog(MicrosoftLoginFn msaLogin = nullptr, QWidget* parent = nullptr)
        : QDialog(parent), msaLogin_(std::move(msaLogin))
    {
        setWindowTitle(tr("Профили"));

        list_ = new QListWidget(this);
        list_->setIconSize(QSize(48,48));
        list_->setSelectionMode(QAbstractItemView::SingleSelection);

        auto* btnAdd = new QToolButton(this);
        btnAdd->setText(tr("Добавить"));
        btnAdd->setPopupMode(QToolButton::MenuButtonPopup);
        auto* menu = new QMenu(btnAdd);
        QAction* actAddMsa     = menu->addAction(tr("Войти через Microsoft…"));
        QAction* actAddOffline = menu->addAction(tr("Оффлайн-профиль…"));
        btnAdd->setMenu(menu);

        btnSetActive_ = new QPushButton(tr("Сделать активным"), this);
        btnRemove_    = new QPushButton(tr("Удалить"),          this);

        auto* btnsRow = new QHBoxLayout;
        btnsRow->addWidget(btnAdd);
        btnsRow->addSpacing(16);
        btnsRow->addWidget(btnSetActive_);
        btnsRow->addWidget(btnRemove_);
        btnsRow->addStretch();

        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);

        auto* v = new QVBoxLayout(this);
        v->addWidget(list_, 1);
        v->addLayout(btnsRow);
        v->addWidget(close);

        connect(actAddMsa,     &QAction::triggered, this, &AccountsDialog::addMicrosoft);
        connect(actAddOffline, &QAction::triggered, this, &AccountsDialog::addOffline);
        connect(btnSetActive_, &QPushButton::clicked, this, &AccountsDialog::setActive);
        connect(btnRemove_,    &QPushButton::clicked, this, &AccountsDialog::removeSelected);
        connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*){ setActive(); });

        refresh();
    }

signals:
    void accountsChanged();

private:
    QListWidget* list_{};
    QPushButton* btnSetActive_{};
    QPushButton* btnRemove_{};
    MicrosoftLoginFn msaLogin_;
    AccountsState st_;

    void refresh() {
        st_ = loadAccountsState();
        list_->clear();
        for (const auto& a : st_.list) {
            const bool isActive = (a.id == st_.activeId);
            const QString tag = (a.kind=="msa" ? "Microsoft" : "Offline");
            const QString text = (isActive ? "✓ " : "") + a.name + "  •  " + tag;

            QPixmap pm = makeAvatarPixmap(a.name, 48);
            auto* it = new QListWidgetItem(QIcon(pm), text);
            it->setData(Qt::UserRole, a.id);
            if (isActive) { QFont f = it->font(); f.setBold(true); it->setFont(f); }
            list_->addItem(it);
        }
        const bool hasSel = (list_->currentItem() != nullptr);
        btnSetActive_->setEnabled(hasSel);
        btnRemove_->setEnabled(hasSel);
        connect(list_, &QListWidget::currentItemChanged, this, [=](auto*, auto*){
            const bool hs = (list_->currentItem() != nullptr);
            btnSetActive_->setEnabled(hs);
            btnRemove_->setEnabled(hs);
        });
    }
    std::optional<int> selectedIndex() const {
        auto* it = list_->currentItem();
        if (!it) return std::nullopt;
        const QString id = it->data(Qt::UserRole).toString();
        for (int i=0;i<st_.list.size();++i) if (st_.list[i].id == id) return i;
        return std::nullopt;
    }

    void addMicrosoft() {
        // встроенный device code
        const QString clientId = readMsaClientId();
        if (clientId.isEmpty()) {
            QMessageBox::warning(this, tr("Вход через Microsoft"),
                                 tr("Не задан clientId (msa/clientId) в настройках."));
            return;
        }
        const QString tenant = readMsaTenant();
        const QString scope  = readMsaScope();

        // 1) devicecode
        QUrl url(QString("https://login.microsoftonline.com/%1/oauth2/v2.0/devicecode").arg(tenant));
        QNetworkAccessManager nam;
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

        QUrlQuery body;
        body.addQueryItem("client_id", clientId);
        body.addQueryItem("scope",     scope);

        QEventLoop loop;
        QNetworkReply* rp = nam.post(req, body.query(QUrl::FullyEncoded).toUtf8());
        connect(rp, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        if (rp->error() != QNetworkReply::NoError) {
            QMessageBox::warning(this, tr("Вход через Microsoft"),
                                 tr("Сеть: %1").arg(rp->errorString()));
            rp->deleteLater();
            return;
        }
        const auto doc = QJsonDocument::fromJson(rp->readAll());
        rp->deleteLater();
        if (!doc.isObject()) {
            QMessageBox::warning(this, tr("Вход через Microsoft"), tr("Некорректный ответ устройства."));
            return;
        }
        auto deviceObj = doc.object();
        if (deviceObj.contains("error")) {
            QMessageBox::warning(this, tr("Вход через Microsoft"),
                                 deviceObj.value("error_description").toString());
            return;
        }

        // 2) окно и поллинг
        DeviceCodeDialog dc(deviceObj, tenant, clientId, this);
        connect(&dc, &DeviceCodeDialog::loginCompleted, this, [this](const Account& a){
            bool replaced=false;
            for (auto& x : st_.list) {
                if (!x.uuid.isEmpty() && x.uuid==a.uuid) { x=a; replaced=true; break; }
            }
            if (!replaced) st_.list.push_back(a);
            st_.activeId = a.id;
            saveAccountsState(st_);
            refresh();
            emit accountsChanged();
            QMessageBox::information(this, {}, tr("Аккаунт %1 подключён.").arg(a.name));
        });
        connect(&dc, &DeviceCodeDialog::loginFailed, this, [this](const QString& err){
            Q_UNUSED(err);
        });
        dc.exec();
    }

    void addOffline() {
        bool ok=false;
        const QString name = QInputDialog::getText(this, tr("Оффлайн-профиль"),
                                                   tr("Имя игрока:"), QLineEdit::Normal,
                                                   QString(), &ok).trimmed();
        if (!ok || name.isEmpty()) return;

        Account a;
        a.id   = QUuid::createUuid().toString(QUuid::WithoutBraces);
        a.kind = "offline";
        a.name = name;
        a.uuid = uuidFromOfflineName(name);
        a.accessToken.clear();
        a.refreshToken.clear();
        a.expiresAt = 0;

        st_.list.push_back(a);
        st_.activeId = a.id;
        saveAccountsState(st_);
        refresh();
        emit accountsChanged();
        QMessageBox::information(this, {}, tr("Оффлайн-профиль «%1» добавлен.").arg(name));
    }

    void setActive() {
        if (auto idx = selectedIndex()) {
            st_.activeId = st_.list[*idx].id;
            saveAccountsState(st_);
            refresh();
            emit accountsChanged();
        }
    }
    void removeSelected() {
        auto idx = selectedIndex();
        if (!idx) return;
        const auto a = st_.list[*idx];
        if (QMessageBox::question(this, {}, tr("Удалить профиль '%1'?").arg(a.name)) != QMessageBox::Yes) return;

        st_.list.removeAt(*idx);
        if (st_.activeId == a.id) st_.activeId = st_.list.isEmpty() ? QString() : st_.list.first().id;
        saveAccountsState(st_);
        refresh();
        emit accountsChanged();
    }
};

// ====================== MainWindow реализации begin/end busy ======================
void MainWindow::applyBusyUi() {
    const bool busy = (busyCount_ > 0);
    if (sbProg_) sbProg_->setVisible(busy);
    // Блокируем основные действия, но не весь UI
    if (auto* w = findChild<QPushButton*>("btnInstall")) w->setEnabled(!busy);
    if (auto* w = findChild<QPushButton*>("btnRun"))     w->setEnabled(!busy);
    if (!busy) statusBar()->clearMessage();
}
void MainWindow::beginBusy(const QString& msg) {
    if (sbText_) sbText_->setText(msg.isEmpty() ? tr("Занято…") : msg);
    ++busyCount_;
    applyBusyUi();
}
void MainWindow::endBusy() {
    busyCount_ = std::max(0, busyCount_ - 1);
    if (busyCount_ == 0 && sbText_) sbText_->clear();
    applyBusyUi();
}

// ====================== MainWindow конструктор ======================
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("Tesuto Launcher"));
    resize(1100, 700);

    auto* central = new QWidget(this);
    auto* root    = new QVBoxLayout(central);
    setCentralWidget(central);

    auto* tool = addToolBar(tr("Действия"));
    auto* actSettings        = tool->addAction(tr("Настройки"));
    auto* actRefreshVersions = tool->addAction(tr("Обновить список версий"));
    auto* actToggleLog       = tool->addAction(tr("Показать лог"));


    auto* topRow = new QHBoxLayout;
    auto* searchEdit = new QLineEdit(central);
    searchEdit->setPlaceholderText(tr("Поиск сборок…"));
    auto* btnCreate = new QPushButton(tr("Создать…"), central);
    auto* btnEdit   = new QPushButton(tr("Изменить"), central);
    auto* btnRemove = new QPushButton(tr("Удалить"), central);
    btnCreate->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    btnEdit->setIcon(style()->standardIcon(QStyle::SP_FileDialogContentsView));
    btnRemove->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    topRow->addWidget(searchEdit, 1);
    topRow->addWidget(btnCreate);
    topRow->addWidget(btnEdit);
    topRow->addWidget(btnRemove);
    root->addLayout(topRow);

    auto* list = new QListWidget(central);
    list->setObjectName("instancesList");
    list->setViewMode(QListView::IconMode);
    list->setUniformItemSizes(true);
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setIconSize(QSize(112,112));
    list->setGridSize(QSize(200, 170));
    list->setSpacing(12);
    list->setWordWrap(true);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(list, 1);

    auto* bottomRow = new QHBoxLayout;
    auto* btnInstall = new QPushButton(tr("Установить"), central);
    btnInstall->setObjectName("btnInstall");
    auto* btnRun     = new QPushButton(tr("Запустить"), central);
    btnRun->setObjectName("playButton"); // для on_playButton_clicked()
    btnInstall->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    btnRun->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    auto* spacer     = new QWidget(central); spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* btnProfile = new QToolButton(central);
    btnProfile->setText(tr("Профиль"));
    btnProfile->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    btnProfile->setIconSize(QSize(32,32));
    btnProfile->setAutoRaise(true);

    bottomRow->addWidget(btnInstall);
    bottomRow->addWidget(btnRun);
    bottomRow->addWidget(spacer, 1);
    bottomRow->addWidget(btnProfile);
    root->addLayout(bottomRow);

    // статусбар
    sbText_ = new QLabel(this);
    sbProg_ = new QProgressBar(this);
    sbProg_->setRange(0, 0);
    sbProg_->setVisible(false);
    statusBar()->addWidget(sbText_);
    statusBar()->addPermanentWidget(sbProg_, 1);

    auto logMsg = [this](const QString& s){ appendLog(this, s); };

    auto* logDock = new QDockWidget(tr("Лог"), this);
    logDock->setObjectName("logDock");
    auto* log = new QTextEdit;
    log->setObjectName("logView");
    log->setReadOnly(true);
    logDock->setWidget(log);
    addDockWidget(Qt::BottomDockWidgetArea, logDock);
    logDock->hide();
    connect(actToggleLog, &QAction::triggered, this, [=]{ logDock->setVisible(!logDock->isVisible()); });

    // Иконка профиля
    auto refreshProfileIcon = [btnProfile]{
        auto acc = activeAccount();
        const QString shown = acc ? acc->name : QStringLiteral("Player");
        btnProfile->setIcon(QIcon(roundedPixmap(makeAvatarPixmap(shown, 48), 10)));
    };
    refreshProfileIcon();

    auto makeIcon = [&](const QString& instDir, const QString& title, const QJsonObject& meta) -> QIcon {
        const QString kindRaw   = meta.value("modloader").toObject().value("kind").toString();
        const QString kindLabel = displayKindFromMeta(meta);
        return instanceIconOrPlaceholder(instDir, title, 112, 20, kindLabel, kindRaw);
    };

    // Трей отключён по требованию.

    auto refreshInstances = [this, list, makeIcon](){
        list->setUpdatesEnabled(false);
        list->clear();

        InstanceStore store(readGameDir());
        const auto all = store.list();
        for (const auto& i : all) {
            const QString instDir = store.pathFor(i);
            const auto meta = loadInstanceMeta(instDir);
            const QString label = decorateTitleWithModloader(
                QString("%1\n[%2] — %3").arg(i.name, i.versionId, i.group),
                meta);

            auto* it = new QListWidgetItem(
                makeIcon(instDir, i.name, meta),
                label
            );
            it->setData(Qt::UserRole, i.id);
            it->setSizeHint(QSize(200, 160));
            list->addItem(it);
        }

        for (int r = 0; r < list->count(); ++r) {
            if (!list->item(r)->isHidden()) { list->setCurrentRow(r, QItemSelectionModel::ClearAndSelect); break; }
        }
        list->setUpdatesEnabled(true);
    };

    auto warmVersionsList = [this, logMsg](){
        beginBusy(tr("Загрузка списка версий…"));
        auto fut = QtConcurrent::run([=]{
            UiBusyGuard guard{const_cast<MainWindow*>(this)};
            try {
                Net net; MojangAPI api(net);
                api.getVersionList();
            } catch (const std::exception& e) {
                QMetaObject::invokeMethod(qApp, [=]{ logMsg(QString("ОШИБКА загрузки версий: %1").arg(e.what())); }, Qt::QueuedConnection);
            }
        });
        Q_UNUSED(fut);
    };

    refreshInstances();
    warmVersionsList();

    connect(searchEdit, &QLineEdit::textChanged, this, [this, list](const QString& text){
        const QString needle = text.trimmed();
        for (int i = 0; i < list->count(); ++i) {
            auto* it = list->item(i);
            const bool match = needle.isEmpty() || it->text().contains(needle, Qt::CaseInsensitive);
            it->setHidden(!match);
        }
        for (int r = 0; r < list->count(); ++r) {
            if (!list->item(r)->isHidden()) { list->setCurrentRow(r, QItemSelectionModel::ClearAndSelect); break; }
        }
    });

    connect(actSettings, &QAction::triggered, this, [=]{
        SettingsDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted) {
            refreshInstances();
            refreshProfileIcon();
        }
    });

    connect(actRefreshVersions, &QAction::triggered, this, warmVersionsList);

    connect(btnCreate, &QPushButton::clicked, this, [=]{
        QStringList versions;
        try {
            Net net; MojangAPI api(net);
            for (const auto& v : api.getVersionList()) versions << v.id;
        } catch (const std::exception& e) {
            QMessageBox::warning(this, tr("Ошибка"),
                                 tr("Не удалось получить список версий: %1").arg(e.what()));
            return;
        }
        QSet<QString> groups;
        {
            InstanceStore store(readGameDir());
            for (const auto& i : store.list()) if (!i.group.isEmpty()) groups.insert(i.group);
        }

        CreateInstanceDialog dlg(versions, QStringList(groups.cbegin(), groups.cend()), this);
        if (dlg.exec() != QDialog::Accepted) return;

        Instance inst;
        inst.name      = dlg.name();
        inst.versionId = dlg.versionId();
        inst.group     = dlg.group();

        InstanceStore store(readGameDir());

        QSet<QString> beforeIds;
        for (const auto& x : store.list()) beforeIds.insert(x.id);

        if (!store.add(inst)) {
            QMessageBox::warning(this, tr("Ошибка"),
                                 tr("Не удалось сохранить сборку (возможно, дубликат id)"));
            return;
        }
        refreshInstances();

        std::optional<Instance> created;
        for (const auto& x : store.list()) {
            if (!beforeIds.contains(x.id) && x.name == inst.name && x.versionId == inst.versionId && x.group == inst.group) {
                created = x; break;
            }
        }
        if (!created) {
            for (const auto& x : store.list()) if (x.name == inst.name) { created = x; break; }
        }
        if (!created) return;

        // сохранить выбор модлоадера в meta
        const QString dir = store.pathFor(*created);
        auto meta = loadInstanceMeta(dir);
        QJsonObject ml;
        ml.insert("kind", dlg.loaderKind());
        ml.insert("version", dlg.loaderVersion());
        meta.insert("modloader", ml);
        saveInstanceMeta(dir, meta);
        refreshInstances();
    });

    connect(btnEdit, &QPushButton::clicked, this, [=]{
        auto* it = list->currentItem();
        if (!it) { QMessageBox::information(this, {}, tr("Выберите сборку.")); return; }
        const QString id = it->data(Qt::UserRole).toString();

        InstanceStore store(readGameDir());
        std::optional<Instance> picked;
        for (const auto& i : store.list()) if (i.id == id) { picked = i; break; }
        if (!picked) { QMessageBox::warning(this, tr("Ошибка"), tr("Сборка не найдена")); return; }

        const QString instDir = store.pathFor(*picked);
        InstanceEditorDialog dlg(instDir, picked->versionId, readJavaPath(), this);
        connect(&dlg, &InstanceEditorDialog::instanceChanged, this, [=]{ refreshInstances(); });
        dlg.exec();
        refreshInstances();
    });

    connect(btnRemove, &QPushButton::clicked, this, [=]{
        auto* it = list->currentItem();
        if (!it) return;
        const QString id = it->data(Qt::UserRole).toString();
        if (QMessageBox::question(this, tr("Удалить"),
                                  tr("Удалить сборку '%1'?").arg(it->text().split('\n').first()))
            != QMessageBox::Yes) return;

        InstanceStore store(readGameDir());
        if (!store.remove(id)) {
            QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось удалить запись"));
            return;
        }
        refreshInstances();
    });

    connect(btnInstall, &QPushButton::clicked, this, [=]{
        auto* it = list->currentItem();
        if (!it) { QMessageBox::information(this, {}, tr("Выберите сборку.")); return; }
        const QString id = it->data(Qt::UserRole).toString();

        InstanceStore store(readGameDir());
        std::optional<Instance> picked;
        for (const auto& i : store.list()) if (i.id == id) { picked = i; break; }
        if (!picked) { QMessageBox::warning(this, tr("Ошибка"), tr("Сборка не найдена")); return; }

        beginBusy(tr("Установка сборки…"));
        appendLog(this, tr("Установка сборки “%1” …").arg(picked->name));
        auto fut = QtConcurrent::run([=]{
            UiBusyGuard guard{const_cast<MainWindow*>(this)};
            try {
                Net net; MojangAPI api(net);
                VersionRef ref; {
                    const auto vlist = api.getVersionList();
                    bool ok=false;
                    for (const auto& v : vlist) if (v.id == picked->versionId) { ref=v; ok=true; break; }
                    if (!ok) throw std::runtime_error(("Версия не найдена: " + picked->versionId).toStdString());
                }
                VersionResolved resolved = api.resolveVersion(ref);

                const QString instDir = store.pathFor(*picked);

                {
                    Installer inst(api, instDir);
                    inst.install(resolved);
                }

                try {
                    const QJsonObject meta = loadInstanceMeta(instDir);
                    const QJsonObject ml   = meta.value("modloader").toObject();
                    const QString kindStr  = ml.value("kind").toString();
                    const QString loaderVer= ml.value("version").toString();

                    if (!kindStr.isEmpty() && kindStr != "none") {
                        ModloaderInstaller mli(net, instDir);
                        LoaderPatch patch;

                        if (kindStr == "fabric")        patch = mli.installFabric(picked->versionId, loaderVer);
                        else if (kindStr == "quilt")    patch = mli.installQuilt (picked->versionId, loaderVer);
                        else if (kindStr == "forge")    patch = mli.installForge (picked->versionId, loaderVer);
                        else if (kindStr == "neoforge") patch = mli.installNeoForge(picked->versionId, loaderVer);

                        LoaderPatchIO::save(instDir, patch);
                        QMetaObject::invokeMethod(qApp, [=]{ appendLog(this, tr("Модлоадер установлен: %1").arg(kindStr)); }, Qt::QueuedConnection);
                    } else {
                        QMetaObject::invokeMethod(qApp, [=]{ appendLog(this, tr("Модлоадер не выбран (ваниль).")); }, Qt::QueuedConnection);
                    }
                } catch (const std::exception& e) {
                    QMetaObject::invokeMethod(qApp, [=]{ appendLog(this, tr("Установка модлоадера: %1").arg(QString::fromUtf8(e.what()))); }, Qt::QueuedConnection);
                }

                QMetaObject::invokeMethod(qApp, [=]{ appendLog(this, tr("Установка сборки «%1» завершена.").arg(picked->name)); }, Qt::QueuedConnection);
            } catch (const std::exception& e) {
                const QString msg = QString::fromUtf8(e.what());
                const QString shown = (msg.isEmpty() || msg == QStringLiteral("std::exception"))
                                     ? QStringLiteral("%1 (%2)").arg(QStringLiteral("std::exception"), QString::fromLatin1(typeid(e).name()))
                                     : msg;
                QMetaObject::invokeMethod(qApp, [=]{ appendLog(this, tr("ОШИБКА установки: %1").arg(shown)); }, Qt::QueuedConnection);
            } catch (...) {
                QMetaObject::invokeMethod(qApp, [=]{ appendLog(this, tr("ОШИБКА установки: неизвестное исключение")); }, Qt::QueuedConnection);
            }
            QMetaObject::invokeMethod(qApp, [=]{
                // обновить список и иконки
                auto* w = findChild<QListWidget*>("instancesList");
                if (w) {
                    InstanceStore store2(readGameDir());
                    const auto all2 = store2.list();
                    w->clear();
                    for (const auto& i2 : all2) {
                        const QString instDir2 = store2.pathFor(i2);
                        const auto meta2 = loadInstanceMeta(instDir2);
                        const QString label2 = decorateTitleWithModloader(
                            QString("%1\n[%2] — %3").arg(i2.name, i2.versionId, i2.group),
                            meta2);
                        const QString kindRaw2   = meta2.value("modloader").toObject().value("kind").toString();
                        const QString kindLabel2 = displayKindFromMeta(meta2);
                        auto* it2 = new QListWidgetItem(
                            instanceIconOrPlaceholder(instDir2, i2.name, 112, 20, kindLabel2, kindRaw2),
                            label2
                        );
                        it2->setData(Qt::UserRole, i2.id);
                        it2->setSizeHint(QSize(200, 160));
                        w->addItem(it2);
                    }
                }
            }, Qt::QueuedConnection);
        });
        Q_UNUSED(fut);
    });

    list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list, &QListWidget::customContextMenuRequested, this, [=](const QPoint& pos){
        auto* it = list->itemAt(pos);
        QMenu m(this);
        QAction* actInstall = m.addAction(style()->standardIcon(QStyle::SP_ArrowDown), tr("Установить"));
        QAction* actRun     = m.addAction(style()->standardIcon(QStyle::SP_MediaPlay), tr("Запустить"));
        QAction* actEdit    = m.addAction(style()->standardIcon(QStyle::SP_FileDialogContentsView), tr("Изменить"));
        m.addSeparator();
        QAction* actOpenDir = m.addAction(style()->standardIcon(QStyle::SP_DirOpenIcon), tr("Открыть папку…"));
        QAction* actDelete  = m.addAction(style()->standardIcon(QStyle::SP_TrashIcon), tr("Удалить"));

        if (!it) for (auto* a : {actInstall, actRun, actEdit, actOpenDir, actDelete}) a->setEnabled(false);

        QAction* pickedAct = m.exec(list->viewport()->mapToGlobal(pos));
        if (!pickedAct || !it) return;

        if (pickedAct == actInstall)      btnInstall->click();
        else if (pickedAct == actRun)     btnRun->click();
        else if (pickedAct == actEdit)    btnEdit->click();
        else if (pickedAct == actOpenDir) {
            InstanceStore store(readGameDir());
            const QString id = it->data(Qt::UserRole).toString();
            for (const auto& ins : store.list())
                if (ins.id == id) { QDesktopServices::openUrl(QUrl::fromLocalFile(store.pathFor(ins))); break; }
        } else if (pickedAct == actDelete) btnRemove->click();
    });

    connect(list, &QListWidget::itemDoubleClicked, this, [=](QListWidgetItem*){ btnRun->click(); });

    // Кнопка «Профиль»: менеджер аккаунтов
    connect(btnProfile, &QToolButton::clicked, this, [=]{
        AccountsDialog dlg(/*msaLogin*/ nullptr, this);
        connect(&dlg, &AccountsDialog::accountsChanged, this, refreshProfileIcon);
        dlg.exec();
        refreshProfileIcon();
    });

    // Кнопка «Запустить» → универсальный слот
    connect(btnRun, &QPushButton::clicked, this, &MainWindow::onPlayClicked);

    auto updateEnable = [=]{
        const bool hasSel = (list->currentItem() != nullptr) && !list->currentItem()->isHidden();
        btnInstall->setEnabled(hasSel && busyCount_==0);
        btnRun->setEnabled(hasSel && busyCount_==0);
        btnRemove->setEnabled(hasSel && busyCount_==0);
        btnEdit->setEnabled(hasSel && busyCount_==0);
    };
    connect(list, &QListWidget::currentItemChanged, this, [=](auto*, auto*){ updateEnable(); });
    updateEnable();

    QTimer::singleShot(0, this, [=]{
        if (list->count() <= 0) return;
        if (!list->currentItem()) {
            list->setCurrentRow(0, QItemSelectionModel::ClearAndSelect);
        } else {
            list->setCurrentItem(list->currentItem(), QItemSelectionModel::ClearAndSelect);
        }
        updateEnable();
    });

    {
        QSettings s;
        restoreGeometry(s.value("mw/geom").toByteArray());
        restoreState(s.value("mw/state").toByteArray());
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this]{
            QSettings s;
            s.setValue("mw/geom", saveGeometry());
            s.setValue("mw/state", saveState());
        });
    }
}

// ====================== Публичные слоты из заголовка ======================
void MainWindow::installVersion() {
    appendLog(this, tr("Действие отключено. Используйте «Сборки → Установить»."));
}
void MainWindow::launchOffline() {
    appendLog(this, tr("Оффлайн-запуск доступен через выбор оффлайн-профиля в «Профиль»."));
}

// ====================== Универсальный запуск ======================
void MainWindow::doPlay() {
    appendLog(this, tr("Нажата кнопка «Запустить»."));

    auto* list = findChild<QListWidget*>("instancesList");
    if (!list) { QMessageBox::warning(this, tr("Ошибка"), tr("Список сборок недоступен")); return; }

    auto* it = list->currentItem();
    if (!it || it->isHidden()) {
        QMessageBox::information(this, {}, tr("Выберите видимую сборку."));
        return;
    }
    const QString id = it->data(Qt::UserRole).toString();
    appendLog(this, tr("Выбран id сборки: %1").arg(id));

    InstanceStore store(readGameDir());
    std::optional<Instance> picked;
    for (const auto& i : store.list()) if (i.id == id) { picked = i; break; }
    if (!picked) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Сборка не найдена"));
        appendLog(this, tr("Сборка с id %1 не найдена.").arg(id));
        return;
    }

    const QString instGameDir = store.pathFor(*picked);
    appendLog(this, tr("Каталог сборки: %1").arg(QDir::toNativeSeparators(instGameDir)));

    const QString java = readJavaPath();

    auto acc = activeAccount();
    if (!acc) {
        QMessageBox::warning(this, tr("Требуется профиль"),
                             tr("Создайте профиль: «Профиль» → Microsoft или Оффлайн."));
        return;
    }

    beginBusy(tr("Запуск клиента…"));

    auto fut = QtConcurrent::run([=]() mutable {
        UiBusyGuard guard{const_cast<MainWindow*>(this)};
        auto uiLog = [this](const QString& txt){
            QMetaObject::invokeMethod(this, [this, txt]{ appendLog(this, txt); }, Qt::QueuedConnection);
        };

        try {
            // Если MSA — возможно, авто-рефреш токена
            if (acc && acc->kind == "msa") {
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (acc->expiresAt <= now + 60*1000) {
                    if (auto upd = refreshMsaAccount(*acc)) {
                        AccountsState st = loadAccountsState();
                        for (auto& x : st.list) if (x.id == acc->id) { x = *upd; break; }
                        st.activeId = upd->id;
                        saveAccountsState(st);
                        acc = activeAccount(); // перечитать обновлённые токены
                        uiLog(tr("Токен обновлён."));
                    } else {
                        throw std::runtime_error("Не удалось обновить токен Microsoft. Повторите вход в «Профиль».");
                    }
                }
            }

            Net net; MojangAPI api(net);

            // найти ссылку версии
            VersionRef ref;
            {
                const auto vlist = api.getVersionList();
                bool ok=false;
                for (const auto& v : vlist) if (v.id == picked->versionId) { ref=v; ok=true; break; }
                if (!ok) {
                    QMetaObject::invokeMethod(this, [this, picked]{
                        appendLog(this, tr("ОШИБКА: версия '%1' не найдена.").arg(picked->versionId));
                        QMessageBox::information(this, tr("Версия не найдена"),
                                                 tr("Версия '%1' не найдена. Установите сборку через «Установить».")
                                                 .arg(picked->versionId));
                    }, Qt::QueuedConnection);
                    return;
                }
            }

            uiLog(tr("Resolve версии %1…").arg(picked->versionId));
            VersionResolved resolved = api.resolveVersion(ref);

            // -------------------- Авто-установка при запуске --------------------
            // Требование: сборка устанавливается при запуске, а не вручную.
            const QJsonObject meta = loadInstanceMeta(instGameDir);
            const QJsonObject ml   = meta.value("modloader").toObject();
            const QJsonObject mark = loadInstallMarker(instGameDir);

            const QString verDir   = QDir(instGameDir).filePath("versions/" + picked->versionId);
            const QString clientJar= QDir(verDir).filePath(picked->versionId + ".jar");
            const bool haveClient  = QFileInfo::exists(clientJar);
            const bool needInstall = (!haveClient) || (!markerMatches(mark, picked->versionId, ml));

            if (needInstall) {
                uiLog(tr("Авто-установка: подготавливаем клиент %1…").arg(picked->versionId));
                {
                    Installer inst(api, instGameDir);
                    inst.install(resolved);
                }

                // Модлоадер (если задан)
                try {
                    const QString kindStr   = ml.value("kind").toString();
                    const QString loaderVer = ml.value("version").toString();
                    if (!kindStr.isEmpty() && kindStr != "none") {
                        uiLog(tr("Авто-установка: модлоадер %1…").arg(kindStr));
                        ModloaderInstaller mli(net, instGameDir);
                        LoaderPatch patch;
                        if (kindStr == "fabric")        patch = mli.installFabric(picked->versionId, loaderVer);
                        else if (kindStr == "quilt")    patch = mli.installQuilt (picked->versionId, loaderVer);
                        else if (kindStr == "forge")    patch = mli.installForge (picked->versionId, loaderVer);
                        else if (kindStr == "neoforge") patch = mli.installNeoForge(picked->versionId, loaderVer);
                        LoaderPatchIO::save(instGameDir, patch);
                        uiLog(tr("Авто-установка: модлоадер установлен."));
                    } else {
                        uiLog(tr("Авто-установка: ваниль (без модлоадера)."));
                    }
                } catch (const std::exception& e) {
                    uiLog(tr("Авто-установка модлоадера: %1").arg(QString::fromUtf8(e.what())));
                }

                saveInstallMarker(instGameDir, picked->versionId, ml);
                uiLog(tr("Авто-установка завершена."));
            }

            // JVM настройки
            AppSettings app = AppSettings::load();
            QStringList jvmExtra = sanitizeJvmArgs(AppSettings::splitJvmArgs(app.jvmArgs));
            const int total = AppSettings::detectTotalRamMiB();
            const int xmx   = app.autoRam ? AppSettings::recommendedMaxRamMiB(total)
                                        : qMax(512, app.maxRamMiB);
            jvmExtra << QString("-Xmx%1m").arg(xmx);
            jvmExtra << QString("-Xms%1m").arg(qMax(512, xmx/4));
            jvmExtra << "-XX:+UseG1GC";

#ifdef Q_OS_LINUX
            jvmExtra << "-Dcom.mojang.text2speech.disable=true";
#endif

            // Патч лоадера (если есть)
            std::unique_ptr<LoaderPatch> maybePatch;
            if (auto disk = LoaderPatchIO::tryLoad(instGameDir); disk.has_value()) {
                maybePatch = std::make_unique<LoaderPatch>(*disk);
                uiLog(tr("Применён модлоадер из loader.patch.json"));
            } else {
                // meta/ml уже прочитаны выше (и могли быть применены при авто-установке)
                const QString kindStr  = ml.value("kind").toString();
                const QString loaderVer= ml.value("version").toString();

                if (!kindStr.isEmpty() && kindStr != "none") {
                    uiLog(tr("Установка модлоадера для запуска: %1…").arg(kindStr));
                    ModloaderInstaller mli(net, instGameDir);
                    LoaderPatch patch;
                    if (kindStr == "fabric")        patch = mli.installFabric(picked->versionId, loaderVer);
                    else if (kindStr == "quilt")    patch = mli.installQuilt (picked->versionId, loaderVer);
                    else if (kindStr == "forge")    patch = mli.installForge (picked->versionId, loaderVer);
                    else if (kindStr == "neoforge") patch = mli.installNeoForge(picked->versionId, loaderVer);

                    LoaderPatchIO::save(instGameDir, patch);
                    maybePatch = std::make_unique<LoaderPatch>(patch);
                    uiLog(tr("Модлоадер установлен и применён: %1").arg(kindStr));
                } else {
                    uiLog(tr("Модлоадер не задан — запуск ваниллы."));
                }
            }

            // Сформировать Minecraft-сессию
            McSession session;
            session.name = acc->name.isEmpty() ? QStringLiteral("Player") : acc->name;
            if (acc->kind == "msa") {
                if (acc->accessToken.isEmpty() || acc->uuid.isEmpty())
                    throw std::runtime_error("Нет действительной сессии Minecraft. Войдите через Microsoft.");
                session.uuid        = acc->uuid;
                session.accessToken = acc->accessToken;
                session.userType    = "msa";
            } else { // offline
                session.uuid        = acc->uuid.isEmpty() ? uuidFromOfflineName(session.name) : acc->uuid;
                session.accessToken = "0";
                session.userType    = "legacy";
            }

            // auth.json
            writeAuthJsonForLaunch(instGameDir, session);

            // запуск клиента
            QMetaObject::invokeMethod(this, [this]{ appendLog(this, tr("Запуск процесса клиента…")); },
                                      Qt::QueuedConnection);

            // Launcher expects (gameDir, javaPath). The previous order was swapped,
            // which made the instance directory be treated as the Java executable and
            // the Java path be treated as the game directory (breaking classpath).
            Launcher launcher(instGameDir, readJavaPath());
            // Launch: online needs a token, offline uses the legacy path.
            if (session.userType == "legacy") {
                launcher.launch(resolved,
                                session.name,
                                jvmExtra,
                                (maybePatch ? maybePatch.get() : nullptr));
            } else {
                launcher.launchOnline(resolved,
                                      session.name,
                                      session.accessToken,
                                      jvmExtra,
                                      (maybePatch ? maybePatch.get() : nullptr));
            }

            QMetaObject::invokeMethod(this, [this]{
                appendLog(this, tr("Клиент завершил работу."));
            }, Qt::QueuedConnection);

        } catch (const std::exception& e) {
            const QString emsg = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(this, [this, emsg]{
                appendLog(this, tr("ОШИБКА запуска: %1").arg(emsg));
                QMessageBox::warning(this, tr("Ошибка запуска"), emsg);
            }, Qt::QueuedConnection);
        }
    });
    Q_UNUSED(fut);

    appendLog(this, tr("Запуск «%1» с профилем %2…")
              .arg(it->text().split('\n').first(),
                   acc->kind=="msa" ? QStringLiteral("Microsoft") : QStringLiteral("Offline")));
}

void MainWindow::changeEvent(QEvent* ev)
{
    // Реакция на смену языка (если вы подключаете переводчики в рантайме)
    if (ev->type() == QEvent::LanguageChange) {
        setWindowTitle(tr("Tesuto Launcher"));
        // Если у вас есть сохранённые QAction/кнопки как поля — обновите их тексты здесь.
        // Пример:
        // if (actionPlay_) actionPlay_->setText(tr("Запустить (активную)"));
        // и т.д.
    }
    QMainWindow::changeEvent(ev);
}


// ====================== Слоты на play ======================
void MainWindow::on_actionPlay_triggered() { doPlay(); }
void MainWindow::on_playButton_clicked()   { doPlay(); }
void MainWindow::onPlayClicked()           { doPlay(); }

#include "MainWindow.moc"
