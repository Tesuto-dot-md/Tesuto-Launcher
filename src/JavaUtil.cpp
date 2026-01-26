#include "JavaUtil.h"
#include <QProcess>
#include <QFileInfo>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QDir>

namespace JavaUtil {

static QString fromEnv(const char* name) {
    return QString::fromUtf8(qgetenv(name));
}

QStringList candidatesFromEnv() {
    QStringList c;

    // 1) JAVA_HOME/bin/java
    const QString jh = fromEnv("JAVA_HOME");
    if (!jh.isEmpty()) c << QDir(jh).filePath("bin/java");
#ifdef Q_OS_WIN
    if (!jh.isEmpty()) c << QDir(jh).filePath("bin/java.exe");
#endif

    // 2) PATH
    const QString found = QStandardPaths::findExecutable("java");
    if (!found.isEmpty()) c << found;

#ifdef Q_OS_LINUX
    // 3) Частые пути
    c << "/usr/bin/java" << "/usr/local/bin/java";

    // Arch: попробовать archlinux-java status → /usr/lib/jvm/<default>/bin/java
    const QString alt = QStandardPaths::findExecutable("archlinux-java");
    if (!alt.isEmpty()) {
        QProcess p;
        p.start(alt, {"status"});
        p.waitForFinished(1500);
        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        QRegularExpression re(R"(default\s+([^\s]+))");
        auto m = re.match(out);
        if (m.hasMatch())
            c << ("/usr/lib/jvm/" + m.captured(1) + "/bin/java");
    }
#endif
    return c;
}

bool isExecutable(const QString& p) {
    QFileInfo fi(p);
    return fi.exists() && fi.isFile() && fi.isExecutable();
}

int javaMajorVersion(const QString& javaPath) {
    if (!isExecutable(javaPath)) return -1;
    QProcess p;
    p.start(javaPath, {"-version"});
    p.waitForFinished(2000);
    const QString err = QString::fromUtf8(p.readAllStandardError());
    const QString txt = err.isEmpty() ? QString::fromUtf8(p.readAllStandardOutput()) : err;

    // "version \"17.0.9\"" или "openjdk version \"1.8.0_392\""
    QRegularExpression re(R"(version\s+"(\d+)(?:\.(\d+))?)");
    auto m = re.match(txt);
    if (!m.hasMatch()) return -1;

    bool ok = false;
    int major = m.captured(1).toInt(&ok);
    if (!ok) return -1;
    if (major == 1) {
        bool ok2 = false;
        int minor = m.captured(2).toInt(&ok2);
        return ok2 ? minor : 8;
    }
    return major;
}

std::optional<QString> detectJava(int minMajor) {
    for (const auto& p : candidatesFromEnv()) {
        if (javaMajorVersion(p) >= minMajor)
            return p;
    }
    return std::nullopt;
}

} // namespace JavaUtil
