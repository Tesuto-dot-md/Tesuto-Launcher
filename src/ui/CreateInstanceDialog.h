#pragma once
#include <QDialog>
#include <QStringList>

class QLineEdit;
class QComboBox;
class QPushButton;
class QLabel;
class QTimer;

class CreateInstanceDialog : public QDialog {
    Q_OBJECT
public:
    explicit CreateInstanceDialog(const QStringList& mcVersions,
                                  const QStringList& knownGroups,
                                  QWidget* parent = nullptr);

    QString name() const;
    QString group() const;
    QString versionId() const;
    QString loaderKind() const;     // "none" | "fabric" | "quilt" | "forge" | "neoforge"
    QString loaderVersion() const;  // "latest"/"recommended" или конкретная версия

private slots:
    void onLoaderKindChanged(int);
    void onVersionChanged(int);
    void refreshLoaderList();  // дебаунс-обновление списка версий лоадера

private:
    // UI
    QLineEdit*  nameEdit_{};
    QComboBox*  groupCombo_{};
    QComboBox*  versionCombo_{};
    QComboBox*  loaderKindCombo_{};
    QComboBox*  loaderVersionCombo_{};
    QLabel*     loaderStatus_{};
    QTimer*     debounce_{};

    // сеть/кеш
    void setLoaderBusy(bool busy, const QString& msg = {});
    void populateLoaderVersions(const QStringList& items, const QString& select = {});
    QString currentMC() const;
    QString currentKind() const;

    // сетевые загрузчики по типам
    QStringList fetchFabricVersions(const QString& mc, bool& ok) const;
    QStringList fetchQuiltVersions (const QString& mc, bool& ok) const;
    QStringList fetchForgeExactVersions(const QString& mc, bool& ok) const; // "recommended","latest", exact Forge builds for MC
    QStringList fetchNeoForgeVersions (const QString& mc, bool& ok) const;  // "latest" + exact NeoForge builds matching MC

    // кеш
    static QString cacheFilePath();
    static QJsonObject readCache();
    static void writeCache(const QJsonObject& root);
    static bool isFresh(qint64 tsMs, qint64 ttlMs);
    void saveListToCache(const QString& kind, const QString& mc, const QStringList& list) const;
    std::optional<QStringList> loadListFromCache(const QString& kind, const QString& mc) const;
};
