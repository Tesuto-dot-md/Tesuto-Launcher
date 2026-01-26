// src/LoaderPatchIO.cpp
#include "LoaderPatchIO.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>

QString LoaderPatchIO::filePathFor(const QString& instanceDir) {
    return QDir(instanceDir).filePath("loader.patch.json");
}

LoaderPatch LoaderPatchIO::load(const QString& instanceDir) {
    QFile f(filePathFor(instanceDir));
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error(QString("Cannot open %1").arg(f.fileName()).toStdString());

    const auto doc = QJsonDocument::fromJson(f.readAll());
    const auto obj = doc.object();

    LoaderPatch p;
    p.mainClass = obj.value("mainClass").toString();

    for (const auto& v : obj.value("classpath").toArray())
        p.classpath << v.toString();

    for (const auto& v : obj.value("jvmArgs").toArray())
        p.jvmArgs << v.toString();

    return p;
}

std::optional<LoaderPatch> LoaderPatchIO::tryLoad(const QString& instanceDir) {
    QFile f(filePathFor(instanceDir));
    if (!f.exists()) return std::nullopt;
    try {
        return load(instanceDir);
    } catch (...) {
        return std::nullopt;
    }
}

bool LoaderPatchIO::save(const QString& instanceDir, const LoaderPatch& p) {
    QJsonObject root;
    root["mainClass"] = p.mainClass;

    QJsonArray cp;
    for (const auto& s : p.classpath) cp.push_back(s);
    root["classpath"] = cp;

    QJsonArray jvm;
    for (const auto& s : p.jvmArgs) jvm.push_back(s);
    root["jvmArgs"] = jvm;

    QDir().mkpath(instanceDir);
    QSaveFile f(filePathFor(instanceDir));
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return f.commit();
}
