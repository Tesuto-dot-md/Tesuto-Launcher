// src/ModLoader.h
#pragma once
#include <QString>
#include <QStringList>
#include <QJsonObject>

class Net;

// Патч для лаунча
struct LoaderPatch {
    QString     mainClass;
    QStringList classpath;   // относительные пути от <instance>/libraries
    QStringList jvmArgs;
};

class ModloaderInstaller {
public:
    ModloaderInstaller(Net& net, QString gameDir)
        : net_(net), gameDir_(std::move(gameDir)) {}

    LoaderPatch installFabric(const QString& mcVersion, const QString& loaderVersion);
    LoaderPatch installQuilt (const QString& mcVersion, const QString& loaderVersion);

    LoaderPatch installForge   (const QString& /*mcVersion*/, const QString& /*loaderVersion*/);
    LoaderPatch installNeoForge(const QString& /*mcVersion*/, const QString& /*loaderVersion*/);

private:
    QString librariesDir() const;
    static QString mavenPathFromName(const QString& name);
    QString ensureJar(const QString& baseUrl, const QString& name);
    LoaderPatch installFromProfileJson(const QJsonObject& profile);

    QJsonObject fetchFabricProfileJson(const QString& mcVersion, const QString& loaderVersion);
    QJsonObject fetchQuiltProfileJson (const QString& mcVersion, const QString& loaderVersion);

private:
    Net&    net_;
    QString gameDir_;
};
