#pragma once

#include <QWidget>
#include <QSplitter>
#include <QListView>
#include <QLineEdit>
#include <QToolButton>
#include <QLabel>
#include <QComboBox>
#include <QMenu>
#include <QTimer>
#include <QPointer>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QCheckBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSet>
#include <QHash>
#include <QIcon>
#include <QDateTime>

class CatalogProxyModel final : public QSortFilterProxyModel
{
public:
    explicit CatalogProxyModel(QObject* parent=nullptr) : QSortFilterProxyModel(parent) {}

    void setHideInstalled(bool v)
    {
        if (hideInstalled_ == v) return;
        beginFilterChange();
        hideInstalled_ = v;
        endFilterChange();
    }

    void refilter()
    {
        beginFilterChange();
        endFilterChange();
    }

    bool hideInstalled() const { return hideInstalled_; }

protected:
    bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

private:
    bool hideInstalled_ = false;
};

class ModManagerWidget : public QWidget
{
    Q_OBJECT
public:
    struct ModCatalogEntry {
        QString source;
        QString projectId;
        QString slug;
        QString title;
        QString description;
        QString iconUrl;

        // resolved version
        QString versionId;
        QString versionNumber;
        QString fileUrl;
        QString fileNameSuggested;

        qint64   downloads = 0;
        QDateTime updated;
    };

    explicit ModManagerWidget(const QString& instanceDir,
                             const QString& mcVersion,
                             const QString& loaderKind,
                             QWidget* parent=nullptr);

signals:
    void modsChanged();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private slots:
    void onRefreshInstalled();
    void onRemoveSelected();
    void onInstallSelected();
    void onCancelInstall();

    void onSearchTextEdited(const QString&);
    void onSearchSubmitted();
    void onSortChanged(int);

    void onCatalogNearBottom();
    void populateOneQueued();

    void onCatalogSelectionChanged();
    void onInstalledSelectionChanged();

private:
    // UI
    void buildUi();
    void connectSignals();
    void setCatalogInfo(const QString& text);

    // installed
    void refreshInstalledList();
    void updateCatalogInstalledDecor();
    void applyInstalledVisual(QStandardItem* it); // <- важно (серость/иконка/чёрный при выборе)

    // catalog
    void ensureFirstPageIfNeeded();
    void startCatalogSearch(const QString& query);
    void fetchNextCatalogPage();
    QString buildModrinthFacets(const QString& mcVersion, const QString& loaderKind);
    void queueAppend(const QList<ModCatalogEntry>& list);

    // selection helpers
    void toggleChosen(const QModelIndex& proxyIdx);
    void applyChosenDecor(QStandardItem* it, bool chosen);

    // network / file
    QByteArray httpGet(const QUrl& url, int timeoutMs, QString* errOut,
                       const QList<QPair<QByteArray,QByteArray>>& headers = {});
    QByteArray httpDownloadToFile(const QUrl& url, const QString& outPath,
                                  int timeoutMs, QString* errOut,
                                  const QList<QPair<QByteArray,QByteArray>>& headers = {});
    QIcon loadIconFor(const QString& url);
    QString iconCachePathFor(const QString& url) const;

    QString modsDir() const;
    QString loaderForApi() const;
    bool downloadToModsDir(const ModCatalogEntry& entry, QString* savedPath);

    bool isModrinthEnabled() const;

private:
    // state
    QString instanceDir_;
    QString mcVersion_;
    QString loaderKind_;

    // widgets
    QSplitter* splitter_ = nullptr;
    QWidget* rightPanel_ = nullptr;

    QLabel* installedTitle_ = nullptr;
    QListView* installedView_ = nullptr;
    QToolButton* btnRemove_ = nullptr;
    QToolButton* btnRefresh_ = nullptr;

    QLineEdit* searchEdit_ = nullptr;
    QToolButton* btnSearch_ = nullptr;
    QComboBox* sortBox_ = nullptr;
    QToolButton* btnInstall_ = nullptr;
    QToolButton* btnCancelInstall_ = nullptr;

    QToolButton* btnFilters_ = nullptr;
    QMenu* filtersMenu_ = nullptr;
    QAction* actModrinth_ = nullptr;
    QAction* actCurse_ = nullptr;

    QCheckBox* hideInstalledBox_ = nullptr;

    QLabel* catalogInfo_ = nullptr;
    QListView* catalogView_ = nullptr;

    // models
    QStandardItemModel installedModel_;
    QStandardItemModel catalogModel_;
    CatalogProxyModel* catalogProxy_ = nullptr;

    // network / timers
    QNetworkAccessManager nam_;
    QPointer<QNetworkReply> currentReply_;

    QTimer searchDebounce_;
    QTimer appendTimer_;

    // pagination / search
    bool fetching_ = false;
    bool endReached_ = false;
    int  nextOffset_ = 0;
    int  pageSize_ = 30;
    QString currentQuery_;
    QString apiIndex_ = "downloads";

    QList<ModCatalogEntry> appendQueue_;

    // caches
    QHash<QString, QIcon> iconCache_;

    // installed detection
    QSet<QString> installedProjectIds_;

    // chosen checkbox selection (catalog)
    QSet<QString> chosenIds_;
    bool syncingCatalogChecks_ = false;
};