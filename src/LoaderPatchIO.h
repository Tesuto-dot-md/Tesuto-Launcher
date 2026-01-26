#pragma once
#include <QString>
#include <optional>
#include "ModLoader.h"   // для LoaderPatch

struct LoaderPatchIO {
    static bool save(const QString& instanceDir, const LoaderPatch& p);
    static std::optional<LoaderPatch> tryLoad(const QString& instanceDir);

    // Эти методы есть в .cpp — объявляем их тоже,
    // можно оставить public для простоты.
    static QString filePathFor(const QString& instanceDir);
    static LoaderPatch load(const QString& instanceDir);
};