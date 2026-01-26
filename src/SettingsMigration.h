#pragma once
#include <QtCore>

namespace SettingsMigration {
// Перенос ключей из старых имён ("tesuto","launcher")
// в новые ("Tesuto","TesutoLauncher"). Вызывать РАНО при старте.
void migrate();
}
