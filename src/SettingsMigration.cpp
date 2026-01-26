#include "SettingsMigration.h"
#include <QSettings>
#include <QDir>

namespace SettingsMigration {

static QStringList keysToCopy()
{
    return {
        "mw/geom",
        "mw/state",
        "network/useSystemProxy",
        "network/noProxy",
        "ui/language",
        "java/defaultPath",
        "java/jvmArgs",
        "java/autoRam",
        "java/maxRamMiB"
    };
}

void migrate()
{
    QSettings legacy("tesuto", "launcher");
    QSettings now("Tesuto", "TesutoLauncher");

    if (legacy.allKeys().isEmpty())
        return;

    const auto keys = keysToCopy();
    int moved = 0;
    for (const auto& k : keys) {
        if (!now.contains(k) && legacy.contains(k)) {
            now.setValue(k, legacy.value(k));
            ++moved;
        }
    }
    if (moved > 0) {
        qInfo() << "[SettingsMigration] migrated" << moved
                << "keys from 'tesuto/launcher' to 'Tesuto/TesutoLauncher'";
    }
}

} // namespace SettingsMigration
