#pragma once
#include <QTranslator>
#include <QVector>
#include <QString>
#include <QPair>

namespace I18n {

// Описание встроенного языка
struct LangDef {
    const char* code;     // "ru_RU", "en_US", "system"
    const char* title;    // "Русский", "English", "System"
    const char* appQm;    // путь в ресурсах (без "://"): "tesuto_ru.qm"
    const char* qtQm;     // опциональный qtbase: "qtbase_ru.qm" или nullptr
};

// Вернуть список (код, заголовок) только из встроенных языков
QVector<QPair<QString, QString>> available();

// Установить (или переустановить) переводчики из ресурсов.
// code = "system" → подхватываем системный язык (если есть совпадение среди встроенных),
// иначе дефолт (например, "en_US").
bool install(const QString& code, QTranslator& appTr, QTranslator& qtTr);

// Нормализация кода ("ru" → "ru_RU" если такой есть, и т.п.)
QString normalize(const QString& code);

// Возвращает дефолтный код, если выбран "system"
QString resolveSystemOrDefault();

} // namespace I18n
