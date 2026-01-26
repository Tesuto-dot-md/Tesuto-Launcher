#include "Launcher.h"
#include "ModLoader.h"
#include "LoaderPatchIO.h"
#include "Util.h"
#include "MojangAPI.h"

#include <QProcess>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <stdexcept>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <optional>
#include <QRegularExpression>
#include <QSettings>
#include <QUrl>
#include "Settings.h"
#include <QStandardPaths>
#include <QDirIterator>


// ----- локальные помощники путей
static QString versionsPath (const QString& base) { return joinPath(base, "versions"); }
static QString librariesPath(const QString& base) { return joinPath(base, "libraries"); }
static QString assetsPath   (const QString& base) { return joinPath(base, "assets");  }

// --- helpers for java exec validation ---
static bool ensureExecutableFile(const QString& path, QString* why)
{
    QFileInfo fi(path);
    if (!fi.exists()) { if (why) *why = "java not found"; return false; }
    if (fi.isDir())   { if (why) *why = "java points to a directory"; return false; }
    QFile f(path);
    auto p = f.permissions();
    auto need = QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther |
                QFileDevice::ReadOwner| QFileDevice::ReadGroup| QFileDevice::ReadOther;
    if ((p & (QFileDevice::ExeOwner|QFileDevice::ExeGroup|QFileDevice::ExeOther)) == 0) {
        if (!f.setPermissions(p | need)) {
            if (why) *why = "failed to set +x permissions";
            return false;
        }
    }
    return true;
}

static bool isNoexecMountFor(const QString& absPath)
{
    QFile m("/proc/mounts");
    if (!m.open(QIODevice::ReadOnly|QIODevice::Text)) return false;
    const QString path = QFileInfo(absPath).absoluteFilePath();
    QString bestMp; QString opts;
    while (!m.atEnd()) {
        const QByteArray line = m.readLine();
        QList<QByteArray> parts = line.split(' ');
        if (parts.size() < 4) continue;
        const QString mp = QString::fromUtf8(parts[1]);
        const QString o  = QString::fromUtf8(parts[3]);
        if (path.startsWith(mp) && mp.size() > bestMp.size()) { bestMp = mp; opts = o; }
    }
    return !bestMp.isEmpty() && opts.contains("noexec");
}

static QString normalizeWorkingDir(const QString& gameDir)
{
    QString workingDir = QDir::cleanPath(gameDir);
    QFileInfo wfi(workingDir);
    if (wfi.isFile()) workingDir = wfi.dir().absolutePath();
    if (!QFileInfo(workingDir).isDir()) QDir().mkpath(workingDir);
    return workingDir;
}

// Поиск java по типовым относительным путям от каталога
static QString findJavaNear(const QString& baseDir)
{
#ifdef Q_OS_WIN
    const QString exe = "java.exe";
#else
    const QString exe = "java";
#endif
    const QStringList rels = {
        "bin/" + exe,
        "jre/bin/" + exe,
        "Contents/Home/bin/" + exe,  // macOS
        "home/bin/" + exe
    };
    for (const QString& rel : rels) {
        const QString cand = QDir(baseDir).filePath(rel);
        QString why;
        if (ensureExecutableFile(cand, &why)) return cand;
    }
    return {};
}

// Рекурсивный поиск java внутри каталога
static QString findJavaRecursive(const QString& baseDir)
{
#ifdef Q_OS_WIN
    const QString needle = "java.exe";
#else
    const QString needle = "java";
#endif
    QDirIterator it(baseDir,
                    QStringList() << needle,
                    QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString cand = it.next();
        // предпочитаем пути с /bin/java
        if (!cand.endsWith("/" + needle) && !cand.endsWith("\\\\" + needle))
            continue;
        QString why;
        if (ensureExecutableFile(cand, &why)) return cand;
    }
    return {};
}

// Главный резолвер пути к java
static QString resolveJavaExecutable(const QString& configured, const QString& gameDir)
{
    // 1) Явно заданный исполняемый файл
    {
        QString why;
        if (!configured.isEmpty() && ensureExecutableFile(configured, &why))
            return configured;
    }

    // 2) Задана директория → пробуем рядом и рекурсивно
    {
        QFileInfo fi(configured);
        if (fi.exists() && fi.isDir()) {
            QString cand = findJavaNear(fi.absoluteFilePath());
            if (!cand.isEmpty()) return cand;
            cand = findJavaRecursive(fi.absoluteFilePath());
            if (!cand.isEmpty()) return cand;
        }
    }

    // 3) runtime/ у инстанса
    {
        const QString rt = QDir(gameDir).filePath("runtime");
        QString cand = findJavaNear(rt);
        if (!cand.isEmpty()) return cand;
        cand = findJavaRecursive(rt);
        if (!cand.isEmpty()) return cand;

        const QStringList common = {
            "java-21","jre-21","temurin-21","jdk-21",
            "java-17","jre-17","temurin-17","jdk-17"
        };
        for (const auto& sub : common) {
            cand = findJavaNear(QDir(rt).filePath(sub));
            if (!cand.isEmpty()) return cand;
            cand = findJavaRecursive(QDir(rt).filePath(sub));
            if (!cand.isEmpty()) return cand;
        }
    }

    // 4) JAVA_HOME
    {
        const QByteArray jh = qgetenv("JAVA_HOME");
        if (!jh.isEmpty()) {
            const QString j = QString::fromLocal8Bit(jh);
            QString cand = findJavaNear(j);
            if (!cand.isEmpty()) return cand;
            cand = findJavaRecursive(j);
            if (!cand.isEmpty()) return cand;
        }
    }

    // 5) PATH
#ifdef Q_OS_WIN
    {
        const QString cand = QStandardPaths::findExecutable("java.exe");
        QString why;
        if (!cand.isEmpty() && ensureExecutableFile(cand, &why)) return cand;
    }
#else
    {
        const QString cand = QStandardPaths::findExecutable("java");
        QString why;
        if (!cand.isEmpty() && ensureExecutableFile(cand, &why)) return cand;
    }
#endif

#ifndef Q_OS_WIN
    // 6) /usr/lib/jvm/**/bin/java
    {
        QDir jvms("/usr/lib/jvm");
        if (jvms.exists()) {
            const auto entries = jvms.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const auto& sub : entries) {
                const QString base = jvms.filePath(sub);
                QString cand = findJavaNear(base);
                if (!cand.isEmpty()) return cand;
                cand = findJavaRecursive(base);
                if (!cand.isEmpty()) return cand;
            }
        }
    }
    // 7) /usr/bin/java
    {
        QString why;
        if (ensureExecutableFile("/usr/bin/java", &why)) return "/usr/bin/java";
    }
#endif

    return {}; // совсем не нашли
}

static QJsonObject readAuthJson(const QString& gameDir) {
    QFile f(QDir(gameDir).filePath("auth.json"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    const auto doc = QJsonDocument::fromJson(f.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}

// Разбиение строки JVM-аргументов в список (с учётом кавычек/пробелов)
static QStringList splitJvm(const QString& s) {
    const QString trimmed = s.trimmed();
    if (trimmed.isEmpty()) return {};
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    return QProcess::splitCommand(trimmed);
#else
    return trimmed.split(' ', Qt::SkipEmptyParts);
#endif
}

// Спиготовский оффлайн UUID (v3 на MD5 "OfflinePlayer:<name>"), без дефисов
static QString offlineUuidNoDashes(const QString& name) {
    QByteArray ns = QByteArrayLiteral("OfflinePlayer:") + name.toUtf8();
    QByteArray md5 = QCryptographicHash::hash(ns, QCryptographicHash::Md5);
    md5[6] = (md5[6] & 0x0F) | 0x30; // версия v3
    md5[8] = (md5[8] & 0x3F) | 0x80; // вариант RFC 4122
    return QString::fromLatin1(md5.toHex());
}

// === Реализации вспомогательных методов ===
QString Launcher::assetIndexIdFromUrl(const QUrl& url)
{
    return QFileInfo(url.path()).completeBaseName();
}

QString Launcher::nativesDirFor(const VersionResolved& v) const
{
    return joinPath(versionsPath(gameDir_), v.id + "/natives");
}

QStringList Launcher::classpathFor(const VersionResolved& v) const
{
    QStringList cp;
    for (const auto& lib : v.libraries) {
        if (!lib.isNative)
            cp << joinPath(librariesPath(gameDir_), lib.path);
    }
    cp << joinPath(versionsPath(gameDir_), v.id + "/" + v.id + ".jar");
    return cp;
}

// Удаляем -Xmx/-Xms из произвольного списка аргументов
static QStringList sanitizeJvmArgs(const QStringList& in) {
    QStringList out;
    QRegularExpression re(R"(^-(?:Xmx|Xms)\b)", QRegularExpression::CaseInsensitiveOption);
    for (const auto& a : in) if (!re.match(a).hasMatch()) out << a;
    return out;
}

// ===================== ОФФЛАЙН (QStringList) =====================
void Launcher::launch(const VersionResolved& v,
                      const QString& playerName,
                      const QStringList& extraJvm,
                      const LoaderPatch* modPatch)
{
    if (java_.isEmpty())
        throw std::runtime_error("Java path is empty");
    if (!QFileInfo::exists(java_))
        qWarning().noquote() << "[LAUNCH] Warning: java path does not exist:" << java_;

    QStringList cp = classpathFor(v);

    std::optional<LoaderPatch> diskPatch;
    if (!modPatch) {
        diskPatch = LoaderPatchIO::tryLoad(gameDir_);
        if (diskPatch)
            qInfo().noquote() << "[LAUNCH] loader.patch.json picked up from instance dir.";
    }
    const LoaderPatch* patch = modPatch ? modPatch : (diskPatch ? &*diskPatch : nullptr);

    if (patch && !patch->classpath.isEmpty()) {
        QStringList patched;
        for (const auto& rel : patch->classpath)
            patched << QDir(librariesPath(gameDir_)).filePath(rel);
        cp = patched + cp;
    }

    QString mainClass = "net.minecraft.client.main.Main";
    if (!v.mainClass.isEmpty())    mainClass = v.mainClass;
    if (patch && !patch->mainClass.isEmpty()) mainClass = patch->mainClass;

    QStringList args;
    args << extraJvm;

    const QString nativesDir = nativesDirFor(v);
    if (QFileInfo(nativesDir).isDir())
        args << (QString("-Djava.library.path=%1").arg(nativesDir));

    if (!args.contains("-XX:+UseG1GC"))
        args << "-XX:+UseG1GC";

    args << "-cp" << cp.join(QDir::listSeparator());
    args << mainClass;

    const QString assetsDir  = assetsPath(gameDir_);
    const QString assetIndex = assetIndexIdFromUrl(v.assetIndexUrl);

    const QString nick = playerName.isEmpty() ? QStringLiteral("Player") : playerName;
    const QString offlineUuid = offlineUuidNoDashes(nick);

    args << "--username"      << nick;
    args << "--version"       << v.id;
    args << "--gameDir"       << QDir::cleanPath(gameDir_);
    args << "--assetsDir"     << assetsDir;
    if (!assetIndex.isEmpty())
        args << "--assetIndex"    << assetIndex;
    args << "--accessToken"   << "0";
    args << "--uuid"          << offlineUuid;
    // Offline profiles should use legacy userType.
    args << "--userType"      << "legacy";
    args << "--userProperties"<< "{}";
    args << "--versionType"   << "release";

    qInfo().noquote() << "[LAUNCH] java =" << java_;
    qInfo().noquote() << "[LAUNCH] main =" << mainClass;
    qInfo().noquote() << "[LAUNCH] cp   =" << cp.join(QDir::listSeparator());
    qInfo().noquote() << "[LAUNCH] args =" << args.join(' ');

    // --- resolve & validate working dir & java executable ---
    QString wd = normalizeWorkingDir(gameDir_);

    // если в настройках папка/битый путь — автоисправим
    {
        const QString resolved = resolveJavaExecutable(java_, wd);
        if (!resolved.isEmpty() && resolved != java_) {
            qWarning().noquote() << "[LAUNCH] java path corrected:" << java_ << "->" << resolved;
            java_ = resolved;
        }
    }

    QString why;
    if (java_.isEmpty()) {
        QString msg = "[LAUNCH] Java executable not found. Install Temurin 17/21 or put JRE into instance runtime/.";
        qCritical().noquote() << msg;
        throw std::runtime_error(msg.toStdString());
    }
    if (!ensureExecutableFile(java_, &why)) {
        const bool noexec = isNoexecMountFor(java_);
        QString msg = QString("[LAUNCH] Invalid Java at %1: %2").arg(java_, why);
        if (noexec) msg += " (filesystem is mounted with noexec)";
        qCritical().noquote() << msg;
        throw std::runtime_error(msg.toStdString());
    }
    if (isNoexecMountFor(java_)) {
        QString msg = QString("[LAUNCH] Java is on a noexec mount: %1. Move JRE to a non-noexec path (e.g. %2/runtime/java-17/bin/java).")
                    .arg(java_, wd);
        qCritical().noquote() << msg;
        throw std::runtime_error(msg.toStdString());
    }
    qInfo().noquote() << "[LAUNCH] wd   =" << wd;

    // ----------------------------------------------------

    // === FINAL JVM MEM ENFORCE (+instance override) ===
    AppSettings app = AppSettings::load();
    const int totalMiB = AppSettings::detectTotalRamMiB();
    bool useAuto = app.autoRam;
    int  xmxMiB  = app.maxRamMiB;

    const QString instId = QFileInfo(gameDir_).fileName();
    QSettings s;
    const auto key = [&](const char* k){ return QString("instance/%1/java/%2").arg(instId, k); };
    const bool instOverride = s.value(key("override"), false).toBool();
    if (instOverride) {
        useAuto = s.value(key("autoRam"), useAuto).toBool();
        xmxMiB  = s.value(key("maxRamMiB"), xmxMiB).toInt();
        // при желании: args << sanitizeJvmArgs(splitJvm(s.value(key("jvmArgs")).toString()));
    }

    if (useAuto) xmxMiB = AppSettings::recommendedMaxRamMiB(totalMiB);
    xmxMiB = qMax(512, xmxMiB);

    args = sanitizeJvmArgs(args);
    args << QString("-Xmx%1m").arg(xmxMiB);
    args << QString("-Xms%1m").arg(qMax(512, xmxMiB/4));

    qInfo() << "[JVM mem final]" << QString("-Xmx%1m").arg(xmxMiB)
            << QString("-Xms%1m").arg(qMax(512, xmxMiB/4));
    // === END ENFORCE ===

    QProcess p;
    p.setProgram(java_);
    p.setArguments(args);
    p.setWorkingDirectory(wd);
    p.setProcessChannelMode(QProcess::ForwardedChannels);

    p.start();
    if (!p.waitForStarted(15000)) {
        const QString err = p.errorString();
        throw std::runtime_error(("Failed to start Java: " + err).toStdString());
    }
    p.waitForFinished(-1);
}


// ===================== ОФФЛАЙН (QString) =====================
void Launcher::launch(const VersionResolved& v,
                      const QString& playerName,
                      const QString& extraJvm,
                      const LoaderPatch* modPatch)
{
    launch(v, playerName, splitJvm(extraJvm), modPatch);
}


// ===================== ОНЛАЙН (QStringList) =====================
int Launcher::launchOnline(const VersionResolved& v,
                           const QString& /*playerNameIn*/,
                           const QString& accessToken,
                           const QStringList& extraJvm,
                           const LoaderPatch* modPatch)
{
    if (java_.isEmpty())
        throw std::runtime_error("Java path is empty");
    if (!QFileInfo::exists(java_))
        qWarning().noquote() << "[LAUNCH] Warning: java path does not exist:" << java_;

    QStringList cp = classpathFor(v);

    std::optional<LoaderPatch> diskPatch;
    if (!modPatch) {
        diskPatch = LoaderPatchIO::tryLoad(gameDir_);
        if (diskPatch)
            qInfo().noquote() << "[LAUNCH] loader.patch.json picked up from instance dir.";
    }
    const LoaderPatch* patch = modPatch ? modPatch : (diskPatch ? &*diskPatch : nullptr);

    if (patch && !patch->classpath.isEmpty()) {
        QStringList patched;
        for (const auto& rel : patch->classpath)
            patched << QDir(librariesPath(gameDir_)).filePath(rel);
        cp = patched + cp;
    }

    QString mainClass = "net.minecraft.client.main.Main";
    if (!v.mainClass.isEmpty())    mainClass = v.mainClass;
    if (patch && !patch->mainClass.isEmpty()) mainClass = patch->mainClass;

    const QJsonObject auth = readAuthJson(gameDir_);
    QString playerName = auth.value("name").toString();
    QString uuid       = auth.value("uuid").toString();
    QString xuid       = auth.value("xuid").toString();
    if (playerName.isEmpty()) playerName = QStringLiteral("Player");
    if (uuid.contains('-'))   uuid = uuid.replace("-", "");

    QStringList args;
    args << extraJvm;

    const QString nativesDir = nativesDirFor(v);
    if (QFileInfo(nativesDir).isDir())
        args << (QString("-Djava.library.path=%1").arg(nativesDir));

    if (!args.contains("-XX:+UseG1GC"))
        args << "-XX:+UseG1GC";

    args << "-cp" << cp.join(QDir::listSeparator());
    args << mainClass;

    const QString assetsDir  = assetsPath(gameDir_);
    const QString assetIndex = assetIndexIdFromUrl(v.assetIndexUrl);

    args << "--username"      << playerName;
    args << "--version"       << v.id;
    args << "--gameDir"       << QDir::cleanPath(gameDir_);
    args << "--assetsDir"     << assetsDir;
    if (!assetIndex.isEmpty())
        args << "--assetIndex"    << assetIndex;
    args << "--accessToken"   << accessToken;
    if (!uuid.isEmpty())
        args << "--uuid"      << uuid;
    else
        args << "--uuid"      << "00000000000000000000000000000000";
    args << "--userType"      << "msa";
    args << "--userProperties"<< "{}";
    args << "--versionType"   << "release";
    if (!xuid.isEmpty())
        args << "--xuid" << xuid;

    qInfo().noquote() << "[LAUNCH] java =" << java_;
    qInfo().noquote() << "[LAUNCH] main =" << mainClass;
    qInfo().noquote() << "[LAUNCH] cp   =" << cp.join(QDir::listSeparator());
    qInfo().noquote() << "[LAUNCH] args =" << args.join(' ');

    // --- validate working dir & java executable ---
    QString wd = normalizeWorkingDir(gameDir_);
    QString why;
    if (!ensureExecutableFile(java_, &why)) {
        const bool noexec = isNoexecMountFor(java_);
        QString msg = QString("[LAUNCH] Invalid Java at %1: %2").arg(java_, why);
        if (noexec) msg += " (filesystem is mounted with noexec)";
        qCritical().noquote() << msg;
        throw std::runtime_error(msg.toStdString());
    }
    if (isNoexecMountFor(java_)) {
        QString msg = QString("[LAUNCH] Java is on a noexec mount: %1. Move JRE to a non-noexec path (e.g. %2/runtime/java-17/bin/java).")
                      .arg(java_, QDir::cleanPath(gameDir_));
        qCritical().noquote() << msg;
        throw std::runtime_error(msg.toStdString());
    }
    qInfo().noquote() << "[LAUNCH] wd   =" << wd;
    // ----------------------------------------------------

    // === FINAL JVM MEM ENFORCE (+instance override) ===
    AppSettings app = AppSettings::load();
    const int totalMiB = AppSettings::detectTotalRamMiB();
    bool useAuto = app.autoRam;
    int  xmxMiB  = app.maxRamMiB;

    const QString instId = QFileInfo(gameDir_).fileName();
    QSettings s;
    const auto key = [&](const char* k){ return QString("instance/%1/java/%2").arg(instId, k); };
    const bool instOverride = s.value(key("override"), false).toBool();
    if (instOverride) {
        useAuto = s.value(key("autoRam"), useAuto).toBool();
        xmxMiB  = s.value(key("maxRamMiB"), xmxMiB).toInt();
        // при желании: args << sanitizeJvmArgs(splitJvm(s.value(key("jvmArgs")).toString()));
    }

    if (useAuto) xmxMiB = AppSettings::recommendedMaxRamMiB(totalMiB);
    xmxMiB = qMax(512, xmxMiB);

    args = sanitizeJvmArgs(args);
    args << QString("-Xmx%1m").arg(xmxMiB);
    args << QString("-Xms%1m").arg(qMax(512, xmxMiB/4));

    qInfo() << "[JVM mem final]" << QString("-Xmx%1m").arg(xmxMiB)
            << QString("-Xms%1m").arg(qMax(512, xmxMiB/4));
    // === END ENFORCE ===

    QProcess p;
    p.setProgram(java_);
    p.setArguments(args);
    p.setWorkingDirectory(wd);      // <- здесь тоже wd
    p.setProcessChannelMode(QProcess::ForwardedChannels);

    p.start();
    if (!p.waitForStarted(15000)) {
        const QString err = p.errorString();
        throw std::runtime_error(("Failed to start Java: " + err).toStdString());
    }

    p.waitForFinished(-1);
    return p.exitCode();
}


// ===================== ОНЛАЙН (QString) =====================
int Launcher::launchOnline(const VersionResolved& v,
                           const QString& playerName,
                           const QString& accessToken,
                           const QString& extraJvm,
                           const LoaderPatch* modPatch)
{
    Q_UNUSED(playerName);
    return launchOnline(v, playerName, accessToken, splitJvm(extraJvm), modPatch);
}

