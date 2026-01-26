#include "i18n.h"
#include <QCoreApplication>
#include <QLocale>

namespace I18n {

// === 1) ЖЁСТКИЙ СПИСОК ВСТРОЕННЫХ ЯЗЫКОВ ===
// Меняйте список только вы: добавляете .qm в ресурсы и строку в массив ниже.
static const LangDef kBuiltin[] = {
    // code     , title     , appQm         , qtQm (можно nullptr, если не нужно)
    { "system" , "System"   , nullptr       , nullptr }, // псевдоязык
    { "ru_RU"  , "Русский"  , "tesuto_ru.qm", "qtbase_ru.qm" },
    { "en_US"  , "English"  , "tesuto_en.qm", "qtbase_en.qm" },
    // { "uk_UA"  , "Українська", "tesuto_uk.qm", "qtbase_uk.qm" },
};

static const LangDef* findByCode(const QString& code) {
    const QString needle = code.trimmed();
    for (const auto& x : kBuiltin)
        if (needle == QString::fromLatin1(x.code))
            return &x;
    return nullptr;
}
static const LangDef* findByLocale(const QLocale& loc) {
    const QString full = loc.name();        // "ru_RU"
    const QString lang = QLocale::languageToCode(loc.language()); // "ru"
    // 1) Полное совпадение
    if (auto* p = findByCode(full)) return p;
    // 2) Совпадение по языку (первые 2 буквы)
    for (const auto& x : kBuiltin) {
        const QString c = QString::fromLatin1(x.code);
        if (c.startsWith(lang) && c != "system") return &x;
    }
    return nullptr;
}

QVector<QPair<QString, QString>> available() {
    QVector<QPair<QString, QString>> out;
    out.reserve(int(std::size(kBuiltin)));
    for (const auto& x : kBuiltin)
        out.push_back({ QString::fromLatin1(x.code), QString::fromLatin1(x.title) });
    return out;
}

QString normalize(const QString& code) {
    if (code.isEmpty()) return "system";
    if (findByCode(code)) return code;
    // Пробуем привести "ru" → "ru_RU" по системной локали/языку
    const auto lang = QLocale(code).language();
    const auto sys  = findByLocale(QLocale(lang));
    return sys ? QString::fromLatin1(sys->code) : QStringLiteral("en_US");
}

QString resolveSystemOrDefault() {
    const auto* p = findByLocale(QLocale::system());
    return p ? QString::fromLatin1(p->code) : QStringLiteral("en_US");
}

bool install(const QString& code, QTranslator& appTr, QTranslator& qtTr)
{
    // Снимаем ранее установленные
    qApp->removeTranslator(&appTr);
    qApp->removeTranslator(&qtTr);

    QString target = code.trimmed();
    if (target.isEmpty() || target == "system")
        target = resolveSystemOrDefault();
    else
        target = normalize(target);

    const LangDef* lang = findByCode(target);
    if (!lang) lang = findByCode("en_US"); // жёсткий fallback

    bool okApp = true, okQt = true;

    if (lang->appQm) {
        const QString path = QString(":/i18n/%1").arg(lang->appQm);
        okApp = appTr.load(path);
        if (okApp) qApp->installTranslator(&appTr);
    }
    if (lang->qtQm) {
        const QString path = QString(":/i18n/%1").arg(lang->qtQm);
        okQt = qtTr.load(path);
        if (okQt) qApp->installTranslator(&qtTr);
    }
    return okApp && okQt;
}

} // namespace I18n
