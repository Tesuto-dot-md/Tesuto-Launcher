#include "Settings.h"
#ifdef Q_OS_LINUX
#include <QFile>
#endif

static inline QSettings makeQs()
{
    // Use explicit org/app to stay consistent even if called before main().
    return QSettings("Tesuto", "TesutoLauncher");
}

AppSettings AppSettings::load()
{
    QSettings qs = makeQs();
    AppSettings s;
    
    // network
    s.useSystemProxy = qs.value("network/useSystemProxy", true).toBool();
    s.noProxy        = qs.value("network/noProxy").toString();

    // java
    s.autoRam   = qs.value("java/autoRam", true).toBool();
    s.maxRamMiB = qs.value("java/maxRamMiB", 0).toInt();
    s.jvmArgs   = qs.value("java/jvmArgs", "").toString();
    return s;
}

void AppSettings::save(const AppSettings& s)
{
    QSettings qs = makeQs();
    
    qs.setValue("network/useSystemProxy", s.useSystemProxy);
    qs.setValue("network/noProxy",        s.noProxy);

    qs.setValue("java/autoRam",   s.autoRam);
    qs.setValue("java/maxRamMiB", s.maxRamMiB);
    qs.setValue("java/jvmArgs",   s.jvmArgs);
    qs.sync();
}

int AppSettings::detectTotalRamMiB()
{
#ifdef Q_OS_LINUX
    QFile f("/proc/meminfo");
    if (f.open(QIODevice::ReadOnly)) {
        while (!f.atEnd()) {
            const auto line = f.readLine();
            if (line.startsWith("MemTotal:")) {
                // пример: "MemTotal:       32764828 kB"
                QList<QByteArray> parts = line.simplified().split(' ');
                for (int i = 0; i < parts.size(); ++i) {
                    if (parts[i].endsWith("kB")) {
                        bool ok=false;
                        long kb = parts[i-1].toLong(&ok);
                        if (ok) return int(kb / 1024);
                    }
                }
            }
        }
    }
#endif
    // простой фолбэк
    return 4096;
}

int AppSettings::recommendedMaxRamMiB(int totalMiB)
{
    // простое правило: 50% ОЗУ, но не ниже 2048 и оставить ~1024 системе
    int half = totalMiB / 2;
    int cap  = qMax(2048, half);
    cap = qMin(cap, totalMiB - 1024);
    return qMax(1024, cap);
}

QStringList AppSettings::splitJvmArgs(const QString& s)
{
    // грубенький разбор по пробелу, без кавычек (достаточно для -Xmx/-XX:…)
    QStringList out;
    for (const auto& part : s.split(' ', Qt::SkipEmptyParts))
        out << part.trimmed();
    return out;
}
