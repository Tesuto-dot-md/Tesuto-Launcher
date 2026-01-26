#pragma once

#include <QWidget>
#include <QSplitter>
#include <QListView>
#include <QToolButton>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QMenu>
#include <QAction>
#include <QCheckBox>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>
#include <QSet>
#include <QHash>
#include <QIcon>
#include <QDateTime>
#include <QList>

class CatalogProxyModel : public QSortFilterProxyModel
{
public:
    explicit CatalogProxyModel(QObject* parent = nullptr);

    void setHideInstalled(bool on);
    bool hideInstalled() const { return hideInstalled_; }

    // trigger filter refresh (Qt6-friendly)
    void refilter();

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
        QString source;           // "modrinth" / "curseforge" (future)
        QString projectId;
        QString slug;
        QString title;
        QString description;
        QString iconUrl;
        qint64  downloads = 0;
        QDateTime updated;

        // resolved version/file
        QString versionId;
        QString versionNumber;
        QString fileUrl;
        QString fileNameSuggested;
    };

    ModManagerWidget(const QString& instanceDir,
                     const QString& mcVersion,
                     const QString& loaderKind,
                     QWidget* parent = nullptr);

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

    void onCatalogSelectionChanged();
    void onInstalledSelectionChanged();

private:
    // UI
    void buildUi();
    void connectSignals();

    // installed
    void refreshInstalledList();
    void updateCatalogInstalledDecor();
    void applyInstalledVisual(QStandardItem* it);

    // catalog
    void ensureFirstPageIfNeeded();
    void startCatalogSearch(const QString& query);
    void fetchNextCatalogPage();
    void onCatalogNearBottom();
    void queueAppend(const QList<ModCatalogEntry>& list);
    void populateOneQueued();

    QString buildModrinthFacets(const QString& mcVersion, const QString& loaderKind);

    // selection
    void toggleChosen(const QModelIndex& proxyIdx);
    void applyChosenDecor(QStandardItem* it, bool chosen);

    // helpers
    QString modsDir() const;
    QString loaderForApi() const;

    QByteArray httpGet(const QUrl& url, int timeoutMs,
                       QString* errOut,
                       const QList<QPair<QByteArray,QByteArray>>& headers = {});

    QByteArray httpDownloadToFile(const QUrl& url, const QString& outPath,
                                 int timeoutMs, QString* errOut,
                                 const QList<QPair<QByteArray,QByteArray>>& headers = {});

    QIcon loadIconFor(const QString& url);
    QString iconCachePathFor(const QString& url) const;

    bool downloadToModsDir(const ModCatalogEntry& entry, QString* savedPath);

    bool isModrinthEnabled() const { return actModrinth_ && actModrinth_->isChecked(); }

    void setCatalogInfo(const QString& s) { if (catalogInfo_) catalogInfo_->setText(s); }

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

    // selection
    QSet<QString> chosenIds_;
    QSet<QString> installedProjectIds_;
    bool syncingCatalogChecks_ = false;

    // catalog paging
    QTimer searchDebounce_;
    QTimer appendTimer_;
    QList<ModCatalogEntry> appendQueue_;

    QNetworkAccessManager nam_;
    QPointer<QNetworkReply> currentReply_;
    QHash<QString, QIcon> iconCache_;

    bool fetching_ = false;
    bool endReached_ = false;
    int nextOffset_ = 0;
    int pageSize_ = 30;

    QString currentQuery_;
    QString apiIndex_; // currently unused, reserved for future
};
