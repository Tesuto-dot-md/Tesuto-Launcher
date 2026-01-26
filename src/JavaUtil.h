#pragma once
#include <QtCore>
#include <optional>

namespace JavaUtil {

// Кандидаты из JAVA_HOME, PATH и типовых путей (Linux)
QStringList candidatesFromEnv();

// Проверка: исполняемый файл?
bool isExecutable(const QString& path);

// Парсинг major из `java -version` (понимает и "1.8" как 8)
int javaMajorVersion(const QString& javaPath);

// Автодетект Java (минимум minMajor), вернёт пусто если не нашлось
std::optional<QString> detectJava(int minMajor = 17);

} // namespace JavaUtil
