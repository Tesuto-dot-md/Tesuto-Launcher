#pragma once
#include <QtCore>

class QUrl;
struct VersionResolved;
struct LoaderPatch;

class Launcher {
public:
    explicit Launcher(QString gameDir, QString javaPath)
        : gameDir_(std::move(gameDir)), java_(std::move(javaPath)) {}

    void setJavaPath(const QString& p)  { java_   = p; }
    void setGameDir (const QString& gd) { gameDir_ = gd; }

    // ОФФЛАЙН
    void launch(const VersionResolved& v,
                const QString& playerName,
                const QStringList& extraJvm,
                const LoaderPatch* modPatch = nullptr);

    void launch(const VersionResolved& v,
                const QString& playerName,
                const QString&  extraJvm,
                const LoaderPatch* modPatch = nullptr);

    // ОНЛАЙН
    int launchOnline(const VersionResolved& v,
                     const QString& playerName,
                     const QString& accessToken,
                     const QStringList& extraJvm,
                     const LoaderPatch* modPatch = nullptr);

    int launchOnline(const VersionResolved& v,
                     const QString& playerName,
                     const QString& accessToken,
                     const QString&  extraJvm,
                     const LoaderPatch* modPatch = nullptr);

private:
    QStringList classpathFor(const VersionResolved& v) const;
    QString     nativesDirFor(const VersionResolved& v) const;
    static QString assetIndexIdFromUrl(const QUrl& url);

private:
    QString gameDir_;
    QString java_;
};
