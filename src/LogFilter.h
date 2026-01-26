#pragma once
#include <QString>

namespace LogFilter {

// Установить/снять перехватчик логов Qt. Маскирует токены/UUID/XUID/домашний каталог.
void install(bool enabled, const QString& homeDir);

// Во время работы можно сменить каталог сборки для маскировки путей.
void setGameDir(const QString& dir);

// Включить/выключить уже установленный фильтр.
void setEnabled(bool enabled);

} // namespace LogFilter
