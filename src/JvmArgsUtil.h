#pragma once
#include <QtCore>

namespace JvmArgsUtil {

// Удаляет все -Xmx/-Xms и добавляет корректные флаги по заданному xmxMiB.
// Xms берём = max(512, Xmx/4).
void enforceMemoryFlags(QStringList& args, int xmxMiB);

}
