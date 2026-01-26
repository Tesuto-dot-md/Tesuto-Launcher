#include "CreateInstanceDialog.h"
#include <QtWidgets>
#include <QStandardPaths>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QXmlStreamReader>
#include <QSet>
#include <tuple>

// ---- helpers: simple sync GET ----
static QByteArray httpGet(const QUrl& url, int timeoutMs = 15000, QString* errOut = nullptr) {
    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "TesutoLauncher/1.0 (+create-dialog)");
    QEventLoop loop;
    QNetworkReply* rp = nam.get(req);
    QObject::connect(rp, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, [&](){
        if (rp->isRunning()) rp->abort();
    });
    timer.start(timeoutMs);
    loop.exec();
    QByteArray data;
    if (rp->error() == QNetworkReply::NoError) {
        data = rp->readAll();
    } else if (errOut) {
        *errOut = rp->errorString();
    }
    rp->deleteLater();
    return data;
}

// ====================== UI =========================
CreateInstanceDialog::CreateInstanceDialog(const QStringList& mcVersions,
                                           const QStringList& knownGroups,
                                           QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Создать сборку"));
    auto* root = new QVBoxLayout(this);

    // Верх: имя и группа
    auto* topForm = new QFormLayout;
    nameEdit_ = new QLineEdit(this);
    nameEdit_->setPlaceholderText(tr("Например, «Моя сборка»"));
    groupCombo_ = new QComboBox(this);
    groupCombo_->setEditable(true);
    groupCombo_->addItems(knownGroups);
    groupCombo_->setCurrentText(QString());
    topForm->addRow(tr("Название:"), nameEdit_);
    topForm->addRow(tr("Группа:"),   groupCombo_);

    // Центр: слева версии, справа модлоадер
    auto* center = new QHBoxLayout;

    auto* leftBox = new QGroupBox(tr("Версия Minecraft"), this);
    auto* leftLay = new QVBoxLayout(leftBox);
    versionCombo_ = new QComboBox(leftBox);
    versionCombo_->setEditable(false);
    versionCombo_->addItems(mcVersions);
    if (versionCombo_->count()>0) versionCombo_->setCurrentIndex(0);
    leftLay->addWidget(versionCombo_);

    auto* rightBox = new QGroupBox(tr("Модлоадер"), this);
    auto* rightLay = new QFormLayout(rightBox);
    loaderKindCombo_ = new QComboBox(rightBox);
    loaderKindCombo_->addItem(tr("Нет"), "none");
    loaderKindCombo_->addItem("Fabric", "fabric");
    loaderKindCombo_->addItem("Quilt",  "quilt");
    loaderKindCombo_->addItem("Forge",  "forge");
    loaderKindCombo_->addItem("NeoForge","neoforge");

    loaderVersionCombo_ = new QComboBox(rightBox);
    loaderVersionCombo_->setEditable(true); // можно вручную вбить
    loaderVersionCombo_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    loaderVersionCombo_->setMinimumContentsLength(20);    // ширина самого комбобокса
    loaderVersionCombo_->setMinimumWidth(320);
    loaderStatus_ = new QLabel(this);
    loaderStatus_->setStyleSheet("color: gray;");

    rightLay->addRow(tr("Тип:"), loaderKindCombo_);
    rightLay->addRow(tr("Версия лоадера:"), loaderVersionCombo_);
    rightLay->addRow(QString(), loaderStatus_);

    center->addWidget(leftBox, 1);
    center->addWidget(rightBox, 1);

    // Низ: кнопки
    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(btns, &QDialogButtonBox::accepted, this, [this]{
        if (nameEdit_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Ошибка"), tr("Укажите название сборки."));
            return;
        }
        if (versionCombo_->currentText().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Ошибка"), tr("Выберите версию Minecraft."));
            return;
        }
        accept();
    });
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);

    root->addLayout(topForm);
    root->addLayout(center, 1);
    root->addWidget(btns);

    // Дебаунс запросов
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(200);
    connect(debounce_, &QTimer::timeout, this, &CreateInstanceDialog::refreshLoaderList);

    connect(loaderKindCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CreateInstanceDialog::onLoaderKindChanged);
    connect(versionCombo_,    QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CreateInstanceDialog::onVersionChanged);

    onLoaderKindChanged(loaderKindCombo_->currentIndex());
}

QString CreateInstanceDialog::name() const { return nameEdit_->text().trimmed(); }
QString CreateInstanceDialog::group() const { return groupCombo_->currentText().trimmed(); }
QString CreateInstanceDialog::versionId() const { return versionCombo_->currentText().trimmed(); }
QString CreateInstanceDialog::loaderKind() const { return loaderKindCombo_->currentData().toString(); }
QString CreateInstanceDialog::loaderVersion() const {
    if (loaderKind() == "none") return QString();
    return loaderVersionCombo_->currentText().trimmed();
}

void CreateInstanceDialog::onLoaderKindChanged(int) {
    const bool useLoader = (currentKind() != "none");
    loaderVersionCombo_->setEnabled(useLoader);
    if (!useLoader) {
        populateLoaderVersions(QStringList() << QString(), QString());
        loaderStatus_->clear();
        return;
    }
    debounce_->start();
}

void CreateInstanceDialog::onVersionChanged(int) {
    if (currentKind() == "none") return;
    debounce_->start();
}

QString CreateInstanceDialog::currentMC() const { return versionCombo_->currentText().trimmed(); }
QString CreateInstanceDialog::currentKind() const { return loaderKindCombo_->currentData().toString(); }

void CreateInstanceDialog::setLoaderBusy(bool busy, const QString& msg) {
    loaderVersionCombo_->setEnabled(!busy && currentKind() != "none");
    if (busy) {
        if (loaderVersionCombo_->count()==0) loaderVersionCombo_->addItem("…");
        loaderStatus_->setText(msg.isEmpty() ? tr("Загрузка списка версий…") : msg);
    } else {
        loaderStatus_->clear();
    }
}

void CreateInstanceDialog::populateLoaderVersions(const QStringList& items, const QString& select) {
    const QString prev = loaderVersionCombo_->currentText();
    loaderVersionCombo_->blockSignals(true);
    loaderVersionCombo_->clear();
    loaderVersionCombo_->addItems(items);
    if (!select.isEmpty()) {
        int idx = loaderVersionCombo_->findText(select);
        if (idx>=0) loaderVersionCombo_->setCurrentIndex(idx);
        else loaderVersionCombo_->setCurrentText(select);
    } else if (!prev.isEmpty()) {
        loaderVersionCombo_->setCurrentText(prev);
    }
    loaderVersionCombo_->blockSignals(false);
}

void CreateInstanceDialog::refreshLoaderList() {
    const QString kind = currentKind();
    const QString mc   = currentMC();
    if (kind == "none") return;

    // кеш сначала
    if (auto cached = loadListFromCache(kind, mc)) {
        populateLoaderVersions(*cached);
        loaderStatus_->setText(tr("Из кеша"));
        return;
    }

    setLoaderBusy(true);

    QStringList result;
    bool ok=false;
    if (kind=="fabric")        result = fetchFabricVersions(mc, ok);
    else if (kind=="quilt")    result = fetchQuiltVersions (mc, ok);
    else if (kind=="forge")    result = fetchForgeExactVersions(mc, ok);
    else if (kind=="neoforge") result = fetchNeoForgeVersions (mc, ok);

    if (!ok || result.isEmpty()) {
        // Фоллбек
        if (kind=="forge") {
            result = QStringList() << "recommended" << "latest";
        } else if (kind=="neoforge") {
            result = QStringList() << "latest";
        } else { // fabric/quilt
            result = QStringList() << "latest";
        }
        populateLoaderVersions(result);
        loaderStatus_->setText(tr("Сеть недоступна — показаны стандартные варианты"));
        setLoaderBusy(false);
        return;
    }

    // ok → показать, сохранить в кеш
    populateLoaderVersions(result);
    saveListToCache(kind, mc, result);
    loaderStatus_->setText(tr("Обновлено из сети"));
    setLoaderBusy(false);
}

// ====================== NETWORK PARSERS =========================

QStringList CreateInstanceDialog::fetchFabricVersions(const QString& mc, bool& ok) const {
    ok=false;
    // https://meta.fabricmc.net/v2/versions/loader/<mc>
    QUrl url(QString("https://meta.fabricmc.net/v2/versions/loader/%1").arg(mc));
    QString err;
    const auto body = httpGet(url, 15000, &err);
    if (body.isEmpty()) return {};
    const auto doc = QJsonDocument::fromJson(body);
    if (!doc.isArray()) return {};
    QSet<QString> uniq;
    for (const auto& v : doc.array()) {
        if (!v.isObject()) continue;
        const auto o = v.toObject().value("loader").toObject();
        const QString ver = o.value("version").toString();
        if (!ver.isEmpty()) uniq.insert(ver);
    }
    QStringList out = QStringList(uniq.cbegin(), uniq.cend());
    out.sort(Qt::CaseInsensitive);
    std::reverse(out.begin(), out.end()); // последние сверху
    if (!out.isEmpty()) out.prepend("latest");
    ok = !out.isEmpty();
    return out;
}

QStringList CreateInstanceDialog::fetchQuiltVersions(const QString& mc, bool& ok) const {
    ok=false;
    // Основной: https://meta.quiltmc.org/v3/versions/loader/<mc>
    // Резерв:   https://meta.quiltmc.org/v3/versions/loader
    auto parseList = [](const QJsonDocument& jd)->QStringList{
        QSet<QString> uniq;
        if (!jd.isArray()) return {};
        for (const auto& v : jd.array()) {
            if (!v.isObject()) continue;
            const auto o = v.toObject().value("loader").toObject();
            const QString ver = o.value("version").toString();
            if (!ver.isEmpty()) uniq.insert(ver);
        }
        QStringList list(uniq.cbegin(), uniq.cend());
        list.sort(Qt::CaseInsensitive);
        std::reverse(list.begin(), list.end());
        return list;
    };

    QString err;
    auto b1 = httpGet(QUrl(QString("https://meta.quiltmc.org/v3/versions/loader/%1").arg(mc)), 15000, &err);
    QJsonDocument d1 = QJsonDocument::fromJson(b1);
    QStringList out = parseList(d1);
    if (out.isEmpty()) {
        auto b2 = httpGet(QUrl("https://meta.quiltmc.org/v3/versions/loader"), 15000, &err);
        QJsonDocument d2 = QJsonDocument::fromJson(b2);
        out = parseList(d2);
    }
    if (!out.isEmpty()) out.prepend("latest");
    ok = !out.isEmpty();
    return out;
}

// little helper: split semver "47.1.3" -> tuple(47,1,3) for numeric sort
static std::tuple<int,int,int> parseSem3(const QString& v) {
    const auto parts = v.split('.', Qt::SkipEmptyParts);
    int a=0,b=0,c=0;
    if (parts.size() > 0) a = parts[0].toInt();
    if (parts.size() > 1) b = parts[1].toInt();
    if (parts.size() > 2) c = parts[2].toInt();
    return std::make_tuple(a,b,c);
}

QStringList CreateInstanceDialog::fetchForgeExactVersions(const QString& mc, bool& ok) const {
    ok=false;

    // 1) promotions: узнаём наличие recommended/latest
    QString err;
    QStringList channels;
    {
        auto body = httpGet(QUrl("https://files.minecraftforge.net/net/minecraftforge/forge/promotions_slim.json"), 15000, &err);
        if (!body.isEmpty()) {
            const auto jd = QJsonDocument::fromJson(body);
            if (jd.isObject()) {
                const auto promos = jd.object().value("promos").toObject();
                if (promos.contains(mc + "-recommended")) channels << "recommended";
                if (promos.contains(mc + "-latest"))      channels << "latest";
            }
        }
    }
    if (channels.isEmpty()) channels << "recommended" << "latest"; // фоллбек

    // 2) maven-metadata: список ВСЕХ артефактов "net.minecraftforge:forge"
    // https://maven.minecraftforge.net/net/minecraftforge/forge/maven-metadata.xml
    auto body = httpGet(QUrl("https://maven.minecraftforge.net/net/minecraftforge/forge/maven-metadata.xml"), 20000, &err);
    QStringList forgeExact; // "47.1.3", "46.0.14", ...
    if (!body.isEmpty()) {
        QXmlStreamReader xml(body);
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QLatin1String("version")) {
                const QString ver = xml.readElementText().trimmed(); // "1.20.1-47.1.3"
                if (ver.startsWith(mc + "-")) {
                    const int dash = ver.indexOf('-');
                    if (dash > 0) {
                        const QString onlyForge = ver.mid(dash+1); // "47.1.3"
                        if (!onlyForge.isEmpty()) forgeExact << onlyForge;
                    }
                }
            }
        }
        forgeExact.removeDuplicates();
        std::sort(forgeExact.begin(), forgeExact.end(),
                  [](const QString& A, const QString& B){ return parseSem3(A) > parseSem3(B); });
    }

    QStringList out = channels;
    out << forgeExact;
    ok = !out.isEmpty();
    return out;
}

// NeoForge mapping: version "21.1.x" -> MC "1.21.1"; "21.0.x" -> "1.21.0", "20.4.x" -> "1.20.4", etc.
static std::pair<int,int> mcToNeoKey(const QString& mc) {
    // "1.21.1" -> (21,1); "1.21" -> (21,0)
    const auto parts = mc.split('.', Qt::SkipEmptyParts);
    int major = (parts.size() > 1) ? parts[1].toInt() : 0; // 21 / 20 / 19...
    int patch = (parts.size() > 2) ? parts[2].toInt() : 0;
    return {major, patch};
}

QStringList CreateInstanceDialog::fetchNeoForgeVersions(const QString& mc, bool& ok) const {
    ok=false;
    QString err;
    // https://maven.neoforged.net/releases/net/neoforged/neoforge/maven-metadata.xml
    auto body = httpGet(QUrl("https://maven.neoforged.net/releases/net/neoforged/neoforge/maven-metadata.xml"), 20000, &err);
    if (body.isEmpty()) return {};

    const auto [needMajor, needMinor] = mcToNeoKey(mc);
    const QString prefix = QString("%1.%2.").arg(needMajor).arg(needMinor); // e.g., "21.1."

    QXmlStreamReader xml(body);
    QSet<QString> uniq;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("version")) {
            const QString ver = xml.readElementText().trimmed(); // "21.1.45"
            if (ver.startsWith(prefix)) uniq.insert(ver);
        }
    }

    QStringList list = QStringList(uniq.cbegin(), uniq.cend());
    std::sort(list.begin(), list.end(),
              [](const QString& A, const QString& B){ return parseSem3(A) > parseSem3(B); });

    QStringList out;
    out << "latest";
    out << list;

    ok = !out.isEmpty();
    return out;
}

// ====================== CACHE =========================

QString CreateInstanceDialog::cacheFilePath() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(base);
    return QDir(base).filePath("modloader_cache.json");
}

QJsonObject CreateInstanceDialog::readCache() {
    QFile f(cacheFilePath());
    if (!f.open(QIODevice::ReadOnly)) return {};
    const auto jd = QJsonDocument::fromJson(f.readAll());
    return jd.isObject() ? jd.object() : QJsonObject{};
}

void CreateInstanceDialog::writeCache(const QJsonObject& root) {
    QFile f(cacheFilePath());
    QDir().mkpath(QFileInfo(f).dir().absolutePath());
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool CreateInstanceDialog::isFresh(qint64 tsMs, qint64 ttlMs) {
    if (tsMs <= 0) return false;
    return (QDateTime::currentMSecsSinceEpoch() - tsMs) < ttlMs;
}

void CreateInstanceDialog::saveListToCache(const QString& kind, const QString& mc, const QStringList& list) const {
    auto root = readCache();
    auto cat  = root.value(kind).toObject();
    QJsonObject pack;
    pack.insert("ts", double(QDateTime::currentMSecsSinceEpoch()));
    QJsonArray arr; for (const auto& s : list) arr.append(s);
    pack.insert("list", arr);
    cat.insert(mc.isEmpty() ? QStringLiteral("_") : mc, pack);
    root.insert(kind, cat);
    writeCache(root);
}

std::optional<QStringList> CreateInstanceDialog::loadListFromCache(const QString& kind, const QString& mc) const {
    static const qint64 kTTL = 12LL * 60 * 60 * 1000; // 12 часов
    const auto root = readCache();
    const auto cat  = root.value(kind).toObject();
    const auto pack = cat.value(mc.isEmpty() ? "_" : mc).toObject();
    const qint64 ts = (qint64)pack.value("ts").toDouble(0);
    if (!isFresh(ts, kTTL)) return std::nullopt;
    QStringList out;
    for (const auto& v : pack.value("list").toArray()) out << v.toString();
    if (out.isEmpty()) return std::nullopt;
    return out;
}
