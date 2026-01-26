#pragma once
#include <QtCore>

struct AppSettings {
    bool     autoRam = true;     // авто-режим подбора Xmx
    int      maxRamMiB = 0;      // если autoRam=false
    QString  jvmArgs;            // доп. JVM-аргументы, строкой

    // Network
    bool     useSystemProxy = true;
    QString  noProxy;            // comma/space-separated bypass list

    static AppSettings load();
    static void save(const AppSettings& s);

    // утилиты
    static int  detectTotalRamMiB();
    static int  recommendedMaxRamMiB(int totalMiB);
    static QStringList splitJvmArgs(const QString& s);
};
