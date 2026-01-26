#include "ModManagerWidget.h"

#include <QPushButton>
#include <QMimeData>
#include <QFileDialog>
#include <QDesktopServices>
#include <QScrollBar>
#include <QStandardPaths>
#include <QFileInfo>
#include <QUrlQuery>
#include <QEventLoop>
#include <QPixmap>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QDir>
#include <QMessageBox>
#include <QInputDialog>
#include <QAction>
#include <QTableWidget>
#include <QHeaderView>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QProgressDialog>
#include <QImage>
#include <QHash>
#include <QRegularExpression>
#include <atomic>
#include <algorithm>

namespace {
    QPointer<QProgressDialog> installProgress_;
    std::atomic_bool cancelInstall_{false};

    // роли
    static constexpr int RoleSource      = Qt::UserRole + 1;
    static constexpr int RoleProjectId   = Qt::UserRole + 2;
    static constexpr int RoleSlug        = Qt::UserRole + 3;
    static constexpr int RoleIconUrl     = Qt::UserRole + 4;
    static constexpr int RoleTitle       = Qt::UserRole + 5;
    static constexpr int RoleDesc        = Qt::UserRole + 6;

    static constexpr int RoleChosenDbg   = Qt::UserRole + 20;
    static constexpr int RoleInstalled   = Qt::UserRole + 30;   // bool
    static constexpr int RoleSelected    = Qt::UserRole + 31;   // bool (выделен в QListView)
    static constexpr int RoleIconNormal  = Qt::UserRole + 40;   // QIcon original

    static QString prettySize(qint64 bytes) {
        const char* units[] = {"B","KB","MB","GB"};
        double v = bytes; int i=0;
        while (v > 999.0 && i < 3) { v /= 1024.0; ++i; }
        return QString::number(v, 'f', (i==0?0:1)) + " " + units[i];
    }

    static QIcon grayIconCached(const QIcon& src)
    {
        static QHash<quint64, QIcon> cache;
        QPixmap pm = src.pixmap(64, 64);
        quint64 key = pm.cacheKey();
        if (cache.contains(key)) return cache.value(key);

        QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < img.height(); ++y) {
            QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < img.width(); ++x) {
                const QColor c = QColor::fromRgba(line[x]);
                int g = qRound(0.299*c.red() + 0.587*c.green() + 0.114*c.blue());
                QColor out(g, g, g, c.alpha());
                line[x] = out.rgba();
            }
        }
        QIcon out(QPixmap::fromImage(img));
        cache.insert(key, out);
        return out;
    }

    static QString sanitizeFileName(const QString& s) {
        QString out = s;
        out.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");
        out = out.simplified();
        out.replace(' ', '-');
        return out;
    }
}

// ───────────────────────────────────────────────────────────
// CatalogProxyModel (declarations in ModManagerWidget.h)

CatalogProxyModel::CatalogProxyModel(QObject* parent)
    : QSortFilterProxyModel(parent)
{
}

void CatalogProxyModel::setHideInstalled(bool v)
{
    if (hideInstalled_ == v) return;
    beginFilterChange();
    hideInstalled_ = v;
    endFilterChange();
}

void CatalogProxyModel::refilter()
{
    beginFilterChange();
    endFilterChange();
}

bool CatalogProxyModel::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const
{
    if (hideInstalled_) {
        QModelIndex idx = sourceModel()->index(source_row, 0, source_parent);
        const bool installed = idx.data(RoleInstalled).toBool();
        if (installed) return false;
    }
    return QSortFilterProxyModel::filterAcceptsRow(source_row, source_parent);
}

// ───────────────────────────────────────────────────────────
// ctor
ModManagerWidget::ModManagerWidget(const QString& instanceDir,
                                   const QString& mcVersion,
                                   const QString& loaderKind,
                                   QWidget* parent)
    : QWidget(parent),
      instanceDir_(instanceDir),
      mcVersion_(mcVersion),
      loaderKind_(loaderKind)
{
    buildUi();
    connectSignals();

    // дебаунс поиска
    searchDebounce_.setInterval(300);
    searchDebounce_.setSingleShot(true);
    connect(&searchDebounce_, &QTimer::timeout, this, &ModManagerWidget::onSearchSubmitted);

    // постепенное добавление
    appendTimer_.setInterval(0);
    appendTimer_.setSingleShot(false);
    connect(&appendTimer_, &QTimer::timeout, this, &ModManagerWidget::populateOneQueued);

    QTimer::singleShot(0, this, [this]{ ensureFirstPageIfNeeded(); });
}

// ───────────────────────────────────────────────────────────
// UI

void ModManagerWidget::buildUi()
{
    auto* lay = new QVBoxLayout(this);

    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setChildrenCollapsible(false);
    lay->addWidget(splitter_, 1);

    // ===== left: installed =====
    auto* left = new QWidget(this);
    auto* lv   = new QVBoxLayout(left);

    installedTitle_ = new QLabel(tr("Установленные моды"), left);
    installedTitle_->setStyleSheet("font-weight:600;");

    installedView_ = new QListView(left);
    installedView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    installedView_->setModel(&installedModel_);
    installedView_->setUniformItemSizes(true);
    installedView_->setViewMode(QListView::ListMode);
    installedView_->setWordWrap(true);
    installedView_->setContextMenuPolicy(Qt::CustomContextMenu);

    auto* ltbar = new QHBoxLayout;
    btnRemove_  = new QToolButton(left);  btnRemove_->setText(tr("Удалить выбранные"));
    btnRefresh_ = new QToolButton(left);  btnRefresh_->setText(tr("Обновить"));
    ltbar->addWidget(btnRemove_);
    ltbar->addWidget(btnRefresh_);
    ltbar->addStretch();

    lv->addWidget(installedTitle_);
    lv->addWidget(installedView_, 1);
    lv->addLayout(ltbar);

    splitter_->addWidget(left);

    // ===== right: catalog =====
    rightPanel_ = new QWidget(this);
    auto* rv = new QVBoxLayout(rightPanel_);

    // поиск
    {
        auto* bar = new QHBoxLayout;
        searchEdit_ = new QLineEdit(rightPanel_);
        searchEdit_->setPlaceholderText(tr("Поиск модов…"));
        btnSearch_ = new QToolButton(rightPanel_); btnSearch_->setText(tr("Найти"));
        bar->addWidget(searchEdit_, 1);
        bar->addWidget(btnSearch_);
        rv->addLayout(bar);
    }

    // сорт/источники/скрыть установленные/кнопки
    {
        auto* bar2 = new QHBoxLayout;
        sortBox_ = new QComboBox(rightPanel_);
        sortBox_->addItem(tr("По популярности"), "downloads");
        sortBox_->addItem(tr("По релевантности"), "relevance");
        sortBox_->addItem(tr("По обновлению"),    "updated");
        sortBox_->addItem(tr("По названию"),      "title");

        btnInstall_ = new QToolButton(rightPanel_); btnInstall_->setText(tr("Установить выбранные"));
        btnCancelInstall_ = new QToolButton(rightPanel_); btnCancelInstall_->setText(tr("Отмена"));

        btnFilters_ = new QToolButton(rightPanel_);
        btnFilters_->setText(tr("Источники"));
        btnFilters_->setPopupMode(QToolButton::InstantPopup);
        filtersMenu_ = new QMenu(btnFilters_);
        actModrinth_ = filtersMenu_->addAction("Modrinth");
        actModrinth_->setCheckable(true); actModrinth_->setChecked(true);
        actCurse_    = filtersMenu_->addAction("CurseForge");
        actCurse_->setCheckable(true); actCurse_->setChecked(false);
        btnFilters_->setMenu(filtersMenu_);

        hideInstalledBox_ = new QCheckBox(tr("Скрывать установленные"), rightPanel_);
        hideInstalledBox_->setChecked(false);

        bar2->addWidget(sortBox_);
        bar2->addSpacing(12);
        bar2->addWidget(btnFilters_);
        bar2->addSpacing(12);
        bar2->addWidget(hideInstalledBox_);
        bar2->addStretch();
        bar2->addWidget(btnInstall_);
        bar2->addWidget(btnCancelInstall_);

        rv->addLayout(bar2);
    }

    catalogInfo_ = new QLabel(tr(""), rightPanel_);
    catalogInfo_->setStyleSheet("color:gray;");

    catalogView_ = new QListView(rightPanel_);
    catalogView_->setUniformItemSizes(true);
    catalogView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    catalogView_->setViewMode(QListView::ListMode);
    catalogView_->setIconSize(QSize(48,48));
    catalogView_->setSpacing(4);
    catalogView_->setContextMenuPolicy(Qt::CustomContextMenu);

    catalogProxy_ = new CatalogProxyModel(this);
    catalogProxy_->setSourceModel(&catalogModel_);
    catalogProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    catalogView_->setModel(catalogProxy_);

    rv->addWidget(catalogInfo_);
    rv->addWidget(catalogView_, 1);

    splitter_->addWidget(rightPanel_);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 2);

    refreshInstalledList();
    setCatalogInfo(tr("Популярные моды Modrinth…"));
}

void ModManagerWidget::connectSignals()
{
    connect(btnRefresh_, &QToolButton::clicked, this, &ModManagerWidget::onRefreshInstalled);
    connect(btnRemove_,  &QToolButton::clicked, this, &ModManagerWidget::onRemoveSelected);
    connect(btnInstall_, &QToolButton::clicked, this, &ModManagerWidget::onInstallSelected);
    connect(btnCancelInstall_, &QToolButton::clicked, this, &ModManagerWidget::onCancelInstall);

    connect(hideInstalledBox_, &QCheckBox::toggled, this, [this](bool on){
        if (catalogProxy_) catalogProxy_->setHideInstalled(on);
    });

    // выбранность через галочки
    connect(&catalogModel_, &QStandardItemModel::itemChanged, this, [this](QStandardItem* it){
        if (!it) return;
        if (syncingCatalogChecks_) return;

        const QString proj = it->data(RoleProjectId).toString();
        if (proj.isEmpty()) return;

        syncingCatalogChecks_ = true;

        const bool checked = (it->checkState() == Qt::Checked);
        if (checked) chosenIds_.insert(proj);
        else chosenIds_.remove(proj);

        applyChosenDecor(it, checked);
        applyInstalledVisual(it); // ✅ не ломаем “серость/чёрный” для installed

        syncingCatalogChecks_ = false;
    });

    connect(searchEdit_, &QLineEdit::textEdited, this, &ModManagerWidget::onSearchTextEdited);
    connect(btnSearch_,  &QToolButton::clicked,  this, &ModManagerWidget::onSearchSubmitted);

    connect(sortBox_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ModManagerWidget::onSortChanged);

    connect(catalogView_, &QListView::doubleClicked, this, [this](const QModelIndex& idx){
        toggleChosen(idx);
    });

    connect(catalogView_, &QWidget::customContextMenuRequested, this, [this](const QPoint& p){
        const auto idx = catalogView_->indexAt(p);
        QMenu m;
        m.addAction(tr("Выбрать/снять выбор"));
        QAction* chosen = m.exec(catalogView_->viewport()->mapToGlobal(p));
        if (chosen && idx.isValid()) toggleChosen(idx);
    });

    connect(catalogView_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &ModManagerWidget::onCatalogSelectionChanged);

    connect(installedView_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &ModManagerWidget::onInstalledSelectionChanged);

    connect(catalogView_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]{
        onCatalogNearBottom();
    });

    // DnD в installed
    installedView_->setAcceptDrops(true);
    installedView_->setDragDropMode(QAbstractItemView::DropOnly);
    installedView_->installEventFilter(this);
}


// ───────────────────────────────────────────────────────────
// installed list

QString ModManagerWidget::modsDir() const {
    return QDir(instanceDir_).filePath("mods");
}

QString ModManagerWidget::loaderForApi() const {
    QString l = loaderKind_.toLower();
    if (l == "neoforge") l = "forge";
    return l;
}


QString ModManagerWidget::iconCachePathFor(const QString& url) const
{
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/icons";
    QDir().mkpath(cacheDir);
    const auto hash = QString::number(qHash(url), 16);
    return QDir(cacheDir).filePath(hash + ".png");
}

void ModManagerWidget::refreshInstalledList()
{
    installedModel_.clear();
    installedModel_.setHorizontalHeaderLabels({tr("Мод"), tr("Размер")});

    installedProjectIds_.clear();

    const QDir md(modsDir());
    md.mkpath(".");

    const QString metaDir = QDir(modsDir()).filePath(".tesuto/mods_meta");
    QDir().mkpath(metaDir);

    const auto files = md.entryInfoList(QStringList() << "*.jar" << "*.zip" << "*.disabled",
                                        QDir::Files, QDir::Name);

    for (const QFileInfo& fi : files) {
        const QString baseName = fi.fileName();
        const QString metaPath = QDir(metaDir).filePath(baseName + ".json");

        QString title = baseName;
        QIcon   icon  = QIcon::fromTheme("application-zip");

        if (QFile::exists(metaPath)) {
            QFile f(metaPath);
            if (f.open(QIODevice::ReadOnly)) {
                const auto doc = QJsonDocument::fromJson(f.readAll());
                if (doc.isObject()) {
                    const auto o = doc.object();
                    title = o.value("title").toString(title);
                    const QString pid = o.value("project_id").toString();
                    if (!pid.isEmpty()) installedProjectIds_.insert(pid);
                    const QString iconFile = o.value("icon_file").toString();
                    if (!iconFile.isEmpty() && QFile::exists(iconFile))
                        icon = QIcon(iconFile);
                }
            }
        }

        auto* name = new QStandardItem(icon, title);
        name->setEditable(false);
        name->setCheckable(true);
        name->setCheckState(Qt::Unchecked);

        // имя файла
        name->setData(baseName, Qt::UserRole + 1);

        // project_id
        if (QFile::exists(metaPath)) {
            QFile f(metaPath);
            if (f.open(QIODevice::ReadOnly)) {
                const auto doc = QJsonDocument::fromJson(f.readAll());
                if (doc.isObject()) {
                    const QString pid = doc.object().value("project_id").toString();
                    if (!pid.isEmpty()) name->setData(pid, Qt::UserRole + 2);
                }
            }
        }

        auto* size = new QStandardItem(prettySize(fi.size()));
        size->setEditable(false);
        size->setToolTip(fi.lastModified().toString(QLocale().dateTimeFormat(QLocale::ShortFormat)));

        installedModel_.appendRow({name, size});
    }

    installedTitle_->setText(tr("Установленные моды (%1)").arg(files.size()));
    updateCatalogInstalledDecor();
}

void ModManagerWidget::applyInstalledVisual(QStandardItem* it)
{
    if (!it) return;

    const QString pid = it->data(RoleProjectId).toString();
    const bool installed = !pid.isEmpty() && installedProjectIds_.contains(pid);
    const bool checked   = (it->checkState() == Qt::Checked);
    const bool selected  = it->data(RoleSelected).toBool();

    QIcon normal = it->data(RoleIconNormal).value<QIcon>();
    if (normal.isNull()) {
        normal = it->icon();
        it->setData(normal, RoleIconNormal);
    }

    it->setData(installed, RoleInstalled);

    if (installed) {
        it->setIcon(grayIconCached(normal));

        // ВАЖНО:
        // - установленный серый, НО
        // - если выделен или выбран галочкой => текст ЧЁРНЫЙ
        if (selected || checked) it->setForeground(QBrush(Qt::black));
        else it->setForeground(QBrush(Qt::gray));
    } else {
        it->setIcon(normal);
        it->setForeground(QBrush(Qt::black));
    }
}

void ModManagerWidget::updateCatalogInstalledDecor()
{
    for (int r = 0; r < catalogModel_.rowCount(); ++r) {
        if (auto* it = catalogModel_.item(r))
            applyInstalledVisual(it);
    }
    if (catalogProxy_) catalogProxy_->refilter();
}

void ModManagerWidget::onRefreshInstalled()
{
    refreshInstalledList();
}

void ModManagerWidget::onRemoveSelected()
{
    // галочки предпочтительнее, иначе выделение
    QList<int> rowsToRemove;
    for (int r = 0; r < installedModel_.rowCount(); ++r) {
        QStandardItem* it = installedModel_.item(r, 0);
        if (it && it->isCheckable() && it->checkState() == Qt::Checked)
            rowsToRemove << r;
    }
    if (rowsToRemove.isEmpty()) {
        const auto rows = installedView_->selectionModel()->selectedRows();
        for (const auto& idx : rows) rowsToRemove << idx.row();
    }

    QSet<int> uniq;
    for (int r : rowsToRemove) uniq.insert(r);
    rowsToRemove = QList<int>(uniq.begin(), uniq.end());

    if (rowsToRemove.isEmpty()) return;

    if (QMessageBox::question(this, {}, tr("Удалить %1 мод(ов)?").arg(rowsToRemove.size())) != QMessageBox::Yes)
        return;

    for (int r : rowsToRemove) {
        QString fileName = installedModel_.item(r, 0)->data(Qt::UserRole + 1).toString();
        if (fileName.isEmpty())
            fileName = installedModel_.item(r, 0)->text();

        QFile::remove(QDir(modsDir()).filePath(fileName));
        QFile::remove(QDir(modsDir()).filePath(".tesuto/mods_meta/" + fileName + ".json"));
    }

    refreshInstalledList();
    emit modsChanged();
}

// ───────────────────────────────────────────────────────────
// catalog: progressive

void ModManagerWidget::ensureFirstPageIfNeeded()
{
    if (catalogModel_.rowCount() == 0 && !fetching_) {
        startCatalogSearch(QString{});
    }
}

void ModManagerWidget::startCatalogSearch(const QString& query)
{
    if (currentReply_) { currentReply_->abort(); currentReply_.clear(); }
    fetching_   = false;
    endReached_ = false;
    nextOffset_ = 0;
    currentQuery_ = query.trimmed();

    appendQueue_.clear();
    appendTimer_.stop();

    catalogModel_.clear();
    setCatalogInfo(currentQuery_.isEmpty() ? tr("Популярные моды Modrinth…")
                                          : tr("Поиск: “%1”…").arg(currentQuery_));

    fetchNextCatalogPage();
}

QString ModManagerWidget::buildModrinthFacets(const QString& mcVersion, const QString& loaderKind)
{
    QStringList groups;
    groups << "[\"project_type:mod\"]";
    if (!mcVersion.isEmpty())
        groups << QString("[\"versions:%1\"]").arg(mcVersion);
    if (!loaderKind.isEmpty() && loaderKind != "none") {
        QString l = loaderKind.toLower();
        if (l == "neoforge") l = "forge";
        groups << QString("[\"categories:%1\"]").arg(l);
    }
    return "[" + groups.join(",") + "]";
}

void ModManagerWidget::fetchNextCatalogPage()
{
    if (fetching_ || endReached_ || !isModrinthEnabled())
        return;

    fetching_ = true;

    QUrl url("https://api.modrinth.com/v2/search");
    QUrlQuery q;
    q.addQueryItem("limit",  QString::number(pageSize_));
    q.addQueryItem("offset", QString::number(nextOffset_));
    q.addQueryItem("index",  currentQuery_.isEmpty() ? "downloads" : "relevance");
    q.addQueryItem("query",  currentQuery_);
    q.addQueryItem("facets", buildModrinthFacets(mcVersion_, loaderKind_));
    url.setQuery(q);

    QNetworkRequest req{ QUrl(url) };
    req.setRawHeader("User-Agent", "TesutoLauncher/1.0 (catalog)");
    currentReply_ = nam_.get(req);

    connect(currentReply_, &QNetworkReply::finished, this, [this]{
        auto guard = std::exchange(currentReply_, QPointer<QNetworkReply>{});
        fetching_ = false;
        if (!guard) return;

        if (guard->error() != QNetworkReply::NoError) {
            setCatalogInfo(tr("Ошибка сети: %1").arg(guard->errorString()));
            guard->deleteLater();
            return;
        }

        const auto doc = QJsonDocument::fromJson(guard->readAll());
        guard->deleteLater();
        if (!doc.isObject()) { setCatalogInfo(tr("Некорректный ответ Modrinth.")); return; }

        const auto root = doc.object();
        const auto hits = root.value("hits").toArray();

        QList<ModCatalogEntry> batch;
        batch.reserve(hits.size());
        for (const auto& v : hits) {
            const auto o = v.toObject();
            ModCatalogEntry e;
            e.source      = "modrinth";
            e.projectId   = o.value("project_id").toString();
            e.slug        = o.value("slug").toString();
            e.title       = o.value("title").toString();
            e.description = o.value("description").toString();
            e.iconUrl     = o.value("icon_url").toString();
            e.downloads   = (qint64)o.value("downloads").toDouble(0);
            e.updated     = QDateTime::fromString(o.value("date_modified").toString(), Qt::ISODate);
            batch.push_back(std::move(e));
        }

        nextOffset_ += hits.size();
        if (hits.isEmpty()) endReached_ = true;

        queueAppend(batch);

        if (catalogModel_.rowCount() == 0 && hits.isEmpty()) {
            setCatalogInfo(tr("Ничего не найдено."));
        } else {
            setCatalogInfo(tr("Найдено: %1+").arg(catalogModel_.rowCount()));
        }
    });
}

void ModManagerWidget::queueAppend(const QList<ModCatalogEntry>& list)
{
    if (list.isEmpty()) return;
    appendQueue_ += list;
    if (!appendTimer_.isActive())
        appendTimer_.start();
}

void ModManagerWidget::populateOneQueued()
{
    if (appendQueue_.isEmpty()) {
        appendTimer_.stop();
        return;
    }
    auto e = appendQueue_.takeFirst();

    QString subtitle = e.description;
    if (subtitle.size() > 120) { subtitle.truncate(117); subtitle += "…"; }

    QIcon icon = loadIconFor(e.iconUrl);

    auto* it = new QStandardItem(icon,
                                 QString("%1\n%2")
                                     .arg(e.title,
                                          subtitle.isEmpty() ? QString(" ") : subtitle));
    it->setEditable(false);
    it->setData(e.source,      RoleSource);
    it->setData(e.projectId,   RoleProjectId);
    it->setData(e.slug,        RoleSlug);
    it->setData(e.iconUrl,     RoleIconUrl);
    it->setData(e.title,       RoleTitle);
    it->setData(e.description, RoleDesc);

    it->setData(false, RoleSelected);
    it->setData(icon, RoleIconNormal);

    it->setCheckable(true);
    it->setCheckState(chosenIds_.contains(e.projectId) ? Qt::Checked : Qt::Unchecked);

    applyChosenDecor(it, it->checkState() == Qt::Checked);
    applyInstalledVisual(it);

    catalogModel_.appendRow(it);

    onCatalogNearBottom();
}

void ModManagerWidget::onCatalogNearBottom()
{
    auto* sb = catalogView_->verticalScrollBar();
    if (!sb) return;
    const int nearPx = 200;
    if (sb->maximum() - sb->value() <= nearPx) {
        fetchNextCatalogPage();
    }
}

// ───────────────────────────────────────────────────────────
// поиск

void ModManagerWidget::onSearchTextEdited(const QString&) {
    searchDebounce_.start();
}

void ModManagerWidget::onSearchSubmitted() {
    startCatalogSearch(searchEdit_->text());
}

void ModManagerWidget::onSortChanged(int) {
    apiIndex_ = sortBox_->currentData().toString();
    startCatalogSearch(searchEdit_->text());
}

// ───────────────────────────────────────────────────────────
// selection визуал (выделенный установленный => чёрный текст)

void ModManagerWidget::onCatalogSelectionChanged()
{
    if (!catalogView_ || !catalogView_->selectionModel()) return;

    // сбросим флаги RoleSelected для всех (проще и надёжнее)
    for (int r = 0; r < catalogModel_.rowCount(); ++r) {
        if (auto* it = catalogModel_.item(r)) {
            it->setData(false, RoleSelected);
        }
    }

    // отметить выделенные
    const auto selectedProxy = catalogView_->selectionModel()->selectedIndexes();
    for (const auto& pidx : selectedProxy) {
        const auto sidx = catalogProxy_->mapToSource(pidx);
        if (auto* it = catalogModel_.itemFromIndex(sidx)) {
            it->setData(true, RoleSelected);
        }
    }

    // применить визуал
    for (int r = 0; r < catalogModel_.rowCount(); ++r) {
        if (auto* it = catalogModel_.item(r))
            applyInstalledVisual(it);
    }
}

void ModManagerWidget::onInstalledSelectionChanged()
{
    // можно оставить пустым
}

// ───────────────────────────────────────────────────────────
// choose helpers

void ModManagerWidget::applyChosenDecor(QStandardItem* it, bool chosen)
{
    QFont f = it->font();
    f.setBold(chosen);
    it->setFont(f);
    it->setData(chosen, RoleChosenDbg);
}

void ModManagerWidget::toggleChosen(const QModelIndex& proxyIdx)
{
    if (!proxyIdx.isValid()) return;
    const auto src = catalogProxy_->mapToSource(proxyIdx);
    QStandardItem* it = catalogModel_.itemFromIndex(src);
    if (!it) return;

    const QString proj = it->data(RoleProjectId).toString();
    const bool now = (it->checkState() != Qt::Checked);

    syncingCatalogChecks_ = true;
    it->setCheckState(now ? Qt::Checked : Qt::Unchecked);

    if (now) chosenIds_.insert(proj);
    else chosenIds_.remove(proj);

    applyChosenDecor(it, now);
    applyInstalledVisual(it);
    syncingCatalogChecks_ = false;
}

// ───────────────────────────────────────────────────────────
// Version resolving (Prism-style batch dialog)

namespace {

struct MrRow {
    QString number;
    QString url;
    QString fname;
    QDateTime published;

    QString displayText() const
    {
        if (published.isValid())
            return QStringLiteral("%1  (%2)").arg(number, published.date().toString(Qt::ISODate));
        return number;
    }
};

static QVector<MrRow> parseModrinthCompatible(const QJsonArray& arr, const QString& mcVersion, const QString& loader)
{
    QVector<MrRow> out;
    out.reserve(arr.size());

    for (const auto& v : arr) {
        const auto o = v.toObject();

        bool okVer = false;
        for (const auto& gv : o.value("game_versions").toArray()) {
            if (gv.toString() == mcVersion) { okVer = true; break; }
        }
        if (!okVer) continue;

        bool okLoader = false;
        for (const auto& ld : o.value("loaders").toArray()) {
            if (ld.toString() == loader) { okLoader = true; break; }
        }
        if (!okLoader) continue;

        const auto files = o.value("files").toArray();
        if (files.isEmpty()) continue;

        QJsonObject fobj;
        bool foundPrimary = false;
        for (const auto& f : files) {
            const auto fo = f.toObject();
            if (fo.value("primary").toBool(false)) {
                fobj = fo;
                foundPrimary = true;
                break;
            }
        }
        if (!foundPrimary) fobj = files.first().toObject();

        MrRow r;
        r.number = o.value("version_number").toString();
        if (r.number.isEmpty()) r.number = o.value("name").toString();
        r.url    = fobj.value("url").toString();
        r.fname  = fobj.value("filename").toString();
        r.published = QDateTime::fromString(o.value("date_published").toString(), Qt::ISODate);

        if (!r.url.isEmpty()) out.push_back(std::move(r));
    }

    std::sort(out.begin(), out.end(), [](const MrRow& a, const MrRow& b){
        if (a.published.isValid() && b.published.isValid()) return a.published > b.published;
        if (a.published.isValid() != b.published.isValid()) return a.published.isValid();
        return a.number > b.number;
    });

    return out;
}

struct PendingPick {
    ModManagerWidget::ModCatalogEntry entry;
    QVector<MrRow> versions;
    int chosenIndex = 0; // default: latest
};

class BatchVersionDialog : public QDialog
{
public:
    explicit BatchVersionDialog(QVector<PendingPick>* picks, int totalMods, QWidget* parent)
        : QDialog(parent), picks_(picks), totalMods_(totalMods)
    {
        setWindowTitle(tr("Выбор версий"));
        resize(720, 420);

        auto* layout = new QVBoxLayout(this);
        auto* lbl = new QLabel(tr("Выберите версии для модов. По умолчанию выбрана последняя совместимая."), this);
        lbl->setWordWrap(true);
        layout->addWidget(lbl);

        auto* countLbl = new QLabel(this);
        const int need = picks_ ? picks_->size() : 0;
        countLbl->setText(tr("Модов к установке: %1. Требуют выбора версии: %2.")
                              .arg(totalMods_)
                              .arg(need));
        countLbl->setStyleSheet("color:gray;");
        layout->addWidget(countLbl);

        table_ = new QTableWidget(this);
        table_->setColumnCount(2);
        table_->setHorizontalHeaderLabels({tr("Мод"), tr("Версия")});
        table_->horizontalHeader()->setStretchLastSection(true);
        table_->verticalHeader()->setVisible(false);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setSelectionMode(QAbstractItemView::NoSelection);
        layout->addWidget(table_, 1);

        build();

        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        bb->button(QDialogButtonBox::Ok)->setText(tr("Установить"));
        bb->button(QDialogButtonBox::Cancel)->setText(tr("Отмена"));
        connect(bb, &QDialogButtonBox::accepted, this, &BatchVersionDialog::accept);
        connect(bb, &QDialogButtonBox::rejected, this, &BatchVersionDialog::reject);
        layout->addWidget(bb);
    }

    void accept() override
    {
        for (int r = 0; r < combos_.size(); ++r) {
            if (!picks_ || r >= picks_->size()) break;
            (*picks_)[r].chosenIndex = combos_[r]->currentIndex();
        }
        QDialog::accept();
    }

private:
    void build()
    {
        if (!picks_) return;
        table_->setRowCount(picks_->size());
        combos_.clear();
        combos_.reserve(picks_->size());

        for (int i = 0; i < picks_->size(); ++i) {
            const auto& p = (*picks_)[i];

            const QString modName = !p.entry.title.isEmpty() ? p.entry.title : p.entry.projectId;
            table_->setItem(i, 0, new QTableWidgetItem(modName));

            auto* combo = new QComboBox(table_);
            for (const auto& v : p.versions)
                combo->addItem(v.displayText());
            combo->setCurrentIndex(qBound(0, p.chosenIndex, combo->count()-1));
            table_->setCellWidget(i, 1, combo);
            combos_.push_back(combo);
        }

        table_->resizeColumnsToContents();
    }

    QVector<PendingPick>* picks_ = nullptr;
    int totalMods_ = 0;
    QTableWidget* table_ = nullptr;
    QVector<QComboBox*> combos_;
};

} // namespace

// ───────────────────────────────────────────────────────────
// install

void ModManagerWidget::onInstallSelected()
{
    QList<ModCatalogEntry> toInstall;

    if (!chosenIds_.isEmpty()) {
        for (int r=0; r<catalogModel_.rowCount(); ++r) {
            QStandardItem* it = catalogModel_.item(r);
            if (!it) continue;
            const QString proj = it->data(RoleProjectId).toString();
            if (!chosenIds_.contains(proj)) continue;

            ModCatalogEntry e;
            e.source      = it->data(RoleSource).toString();
            e.projectId   = proj;
            e.slug        = it->data(RoleSlug).toString();
            e.iconUrl     = it->data(RoleIconUrl).toString();
            e.title       = it->data(RoleTitle).toString();
            e.description = it->data(RoleDesc).toString();
            toInstall << e;
        }
    } else {
        const auto sel = catalogView_->selectionModel()->selectedIndexes();
        for (const auto& idx : sel) {
            const auto src = catalogProxy_->mapToSource(idx);
            auto* it = catalogModel_.itemFromIndex(src);
            if (!it) continue;

            ModCatalogEntry e;
            e.source      = it->data(RoleSource).toString();
            e.projectId   = it->data(RoleProjectId).toString();
            e.slug        = it->data(RoleSlug).toString();
            e.iconUrl     = it->data(RoleIconUrl).toString();
            e.title       = it->data(RoleTitle).toString();
            e.description = it->data(RoleDesc).toString();
            toInstall << e;
        }
    }

    if (toInstall.isEmpty()) {
        QMessageBox::information(this, {}, tr("Выберите моды для установки."));
        return;
    }

    cancelInstall_ = false;

    // resolve versions
    QProgressDialog resolveProg(tr("Подбор версий…"), tr("Отмена"), 0, toInstall.size(), this);
    resolveProg.setWindowModality(Qt::ApplicationModal);
    resolveProg.setMinimumDuration(0);
    connect(&resolveProg, &QProgressDialog::canceled, this, []{ cancelInstall_ = true; });

    QVector<ModCatalogEntry> resolved;
    resolved.reserve(toInstall.size());
    QVector<PendingPick> needsChoice;
    needsChoice.reserve(toInstall.size());
    QStringList errors;

    int step = 0;

    for (ModCatalogEntry entry : toInstall) {
        if (cancelInstall_.load()) break;

        if (!entry.source.isEmpty() && entry.source != QStringLiteral("modrinth")) {
            errors << tr("Источник мода не поддерживается: %1").arg(entry.title.isEmpty() ? entry.projectId : entry.title);
            resolveProg.setValue(++step);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
            continue;
        }

        if (entry.projectId.isEmpty()) {
            errors << tr("У мода нет projectId: %1").arg(entry.title);
            resolveProg.setValue(++step);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
            continue;
        }

        const QUrl url(QString("https://api.modrinth.com/v2/project/%1/version").arg(entry.projectId));
        QString httpErr;
        const QByteArray bin = httpGet(url, 20000, &httpErr, {{"User-Agent","TesutoLauncher/1.0 (resolve)"}});
        if (bin.isEmpty()) {
            errors << tr("Не удалось получить версии для %1: %2")
                         .arg(entry.title.isEmpty() ? entry.projectId : entry.title,
                              httpErr.isEmpty() ? tr("пустой ответ") : httpErr);
            resolveProg.setValue(++step);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
            continue;
        }

        const auto doc = QJsonDocument::fromJson(bin);
        if (!doc.isArray()) {
            errors << tr("Некорректный ответ API версий для %1")
                         .arg(entry.title.isEmpty() ? entry.projectId : entry.title);
            resolveProg.setValue(++step);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
            continue;
        }

        const QVector<MrRow> vers = parseModrinthCompatible(doc.array(), mcVersion_, loaderForApi());
        if (vers.isEmpty()) {
            errors << tr("Нет совместимых версий для %1 (MC %2, loader %3)")
                         .arg(entry.title.isEmpty() ? entry.projectId : entry.title,
                              mcVersion_, loaderForApi());
            resolveProg.setValue(++step);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
            continue;
        }

        if (vers.size() == 1) {
            entry.versionNumber     = vers[0].number;
            entry.fileUrl           = vers[0].url;
            entry.fileNameSuggested = vers[0].fname;
            resolved.push_back(std::move(entry));
        } else {
            PendingPick p;
            p.entry = std::move(entry);
            p.versions = vers;
            p.chosenIndex = 0;
            needsChoice.push_back(std::move(p));
        }

        resolveProg.setValue(++step);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }

    if (cancelInstall_.load()) {
        QMessageBox::information(this, {}, tr("Операция отменена."));
        return;
    }

    if (!needsChoice.isEmpty()) {
        BatchVersionDialog dlg(&needsChoice, toInstall.size(), this);
        if (dlg.exec() != QDialog::Accepted) return;

        for (auto& p : needsChoice) {
            const int idx = qBound(0, p.chosenIndex, p.versions.size() - 1);
            const auto& v = p.versions[idx];
            p.entry.versionNumber     = v.number;
            p.entry.fileUrl           = v.url;
            p.entry.fileNameSuggested = v.fname;
            resolved.push_back(std::move(p.entry));
        }
    }

    if (resolved.isEmpty()) {
        QMessageBox::warning(this, tr("Не удалось установить"),
                             errors.isEmpty() ? tr("Нет подходящих модов для установки.") : errors.first());
        return;
    }

    // download
    installProgress_ = new QProgressDialog(tr("Установка модов…"), tr("Отмена"), 0, resolved.size(), this);
    installProgress_->setWindowModality(Qt::ApplicationModal);
    installProgress_->setAutoClose(false);
    installProgress_->setAutoReset(false);
    installProgress_->setMinimumDuration(0);
    installProgress_->setValue(0);
    connect(installProgress_, &QProgressDialog::canceled, this, []{ cancelInstall_ = true; });

    QStringList saved;
    int done = 0;

    for (const ModCatalogEntry& entry : resolved) {
        if (cancelInstall_.load()) break;

        QString savedPath;
        if (!downloadToModsDir(entry, &savedPath)) {
            errors << tr("Ошибка загрузки: %1")
                         .arg(entry.title.isEmpty() ? entry.projectId : entry.title);
        } else {
            saved << savedPath;
        }

        ++done;
        if (installProgress_) installProgress_->setValue(done);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }

    if (installProgress_) {
        installProgress_->hide();
        installProgress_->deleteLater();
        installProgress_ = nullptr;
    }

    if (cancelInstall_.load()) {
        QMessageBox::information(this, {}, tr("Установка отменена. Установлено файлов: %1").arg(saved.size()));
    } else if (!errors.isEmpty()) {
        if (saved.isEmpty())
            QMessageBox::warning(this, tr("Часть модов не установлена"), errors.first());
        else
            QMessageBox::information(this, {}, tr("Установлено файлов: %1").arg(saved.size()));
    } else {
        QMessageBox::information(this, {}, tr("Установлено файлов: %1").arg(saved.size()));
    }

    refreshInstalledList();
    emit modsChanged();

    // ✅ После установки: сброс выделения и чеков
    if (catalogView_) catalogView_->clearSelection();
    if (installedView_) installedView_->clearSelection();

    syncingCatalogChecks_ = true;
    chosenIds_.clear();
    for (int r = 0; r < catalogModel_.rowCount(); ++r) {
        QStandardItem* it = catalogModel_.item(r);
        if (!it) continue;
        if (it->isCheckable()) it->setCheckState(Qt::Unchecked);
        it->setData(false, RoleSelected);
        applyChosenDecor(it, false);
        applyInstalledVisual(it);
    }
    syncingCatalogChecks_ = false;
}

void ModManagerWidget::onCancelInstall()
{
    cancelInstall_ = true;
}

// ───────────────────────────────────────────────────────────
// network / icons / download

QByteArray ModManagerWidget::httpGet(const QUrl& url, int timeoutMs,
                                     QString* errOut,
                                     const QList<QPair<QByteArray,QByteArray>>& headers)
{
    QNetworkRequest req{url};
    for (const auto& h : headers) req.setRawHeader(h.first, h.second);

    QNetworkReply* rp = nam_.get(req);
    if (!rp) { if (errOut) *errOut = QStringLiteral("network: get() returned nullptr"); return {}; }

    QEventLoop loop;
    QObject::connect(rp, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&](){
        if (rp && rp->isRunning()) rp->abort();
    });
    if (timeoutMs > 0) timer.start(timeoutMs);

    loop.exec();

    QByteArray data;
    if (rp->error() == QNetworkReply::NoError)
        data = rp->readAll();
    else if (errOut)
        *errOut = rp->errorString();

    rp->deleteLater();
    return data;
}

QByteArray ModManagerWidget::httpDownloadToFile(const QUrl& url, const QString& outPath,
                                                int timeoutMs, QString* errOut,
                                                const QList<QPair<QByteArray,QByteArray>>& headers)
{
    QNetworkRequest req{url};
    for (const auto& h : headers) req.setRawHeader(h.first, h.second);

    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly)) {
        if (errOut) *errOut = tr("cannot write: %1").arg(outPath);
        return {};
    }

    QNetworkReply* rp = nam_.get(req);
    if (!rp) { if (errOut) *errOut = QStringLiteral("network: get() returned nullptr"); return {}; }

    QEventLoop loop;
    QObject::connect(rp, &QNetworkReply::readyRead, &loop, [&](){
        out.write(rp->readAll());
    });
    QObject::connect(rp, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&](){
        if (rp && rp->isRunning()) rp->abort();
    });
    if (timeoutMs > 0) timer.start(timeoutMs);

    loop.exec();

    QByteArray final;
    if (rp->error() == QNetworkReply::NoError) {
        out.flush();
        final = "ok";
    } else if (errOut) {
        *errOut = rp->errorString();
    }
    rp->deleteLater();
    return final;
}

QIcon ModManagerWidget::loadIconFor(const QString& url)
{
    if (url.isEmpty()) return QIcon();
    if (iconCache_.contains(url)) return iconCache_.value(url);

    QNetworkRequest req{ QUrl(url) };
    req.setRawHeader("User-Agent", "TesutoLauncher/1.0 (icons)");

    QNetworkReply* rp = nam_.get(req);
    if (!rp) return QIcon();

    QEventLoop loop;
    QObject::connect(rp, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    QIcon icon;
    if (rp->error() == QNetworkReply::NoError) {
        QPixmap pm;
        pm.loadFromData(rp->readAll());
        if (!pm.isNull()) icon.addPixmap(pm);
    }
    rp->deleteLater();

    if (!icon.isNull()) iconCache_.insert(url, icon);
    return icon;
}

bool ModManagerWidget::downloadToModsDir(const ModCatalogEntry& entry, QString* savedPath)
{
    QDir dir(modsDir());
    dir.mkpath(".");

    QString base = sanitizeFileName(entry.slug.isEmpty() ? entry.title : entry.slug);
    if (!entry.versionNumber.isEmpty())
        base += "-" + sanitizeFileName(entry.versionNumber);

    QString ext = "jar";
    const QString fromName = QFileInfo(entry.fileNameSuggested).suffix();
    if (!fromName.isEmpty()) ext = fromName;

    QString target = dir.filePath(base + "." + ext);
    int counter = 1;
    while (QFile::exists(target)) {
        target = dir.filePath(base + QString("(%1).%2").arg(++counter).arg(ext));
    }

    QString err;
    const QByteArray ok = httpDownloadToFile(QUrl(entry.fileUrl), target, 60000, &err,
                                             {{"User-Agent","TesutoLauncher/1.0 (download)"}});
    Q_UNUSED(ok);

    if (!err.isEmpty()) return false;

    // meta + icon for installed list
    const QString metaRoot = dir.filePath(".tesuto/mods_meta");
    QDir().mkpath(metaRoot);

    QJsonObject o;
    o["title"] = entry.title;
    o["project_id"] = entry.projectId;
    o["slug"] = entry.slug;
    o["icon_url"] = entry.iconUrl;

    QString iconFile;
    if (!entry.iconUrl.isEmpty()) {
        const QString iconPath = iconCachePathFor(entry.iconUrl);
        if (!QFile::exists(iconPath)) {
            QByteArray bin = httpGet(QUrl(entry.iconUrl), 10000, nullptr, {{"User-Agent","TesutoLauncher/1.0 (icons)"}});
            if (!bin.isEmpty()) { QFile f(iconPath); if (f.open(QIODevice::WriteOnly)) f.write(bin); }
        }
        if (QFile::exists(iconPath)) iconFile = iconPath;
    }
    o["icon_file"] = iconFile;

    QFile mf(QDir(metaRoot).filePath(QFileInfo(target).fileName() + ".json"));
    if (mf.open(QIODevice::WriteOnly))
        mf.write(QJsonDocument(o).toJson(QJsonDocument::Indented));

    if (savedPath) *savedPath = target;
    return true;
}

// ───────────────────────────────────────────────────────────
// DnD

bool ModManagerWidget::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == installedView_) {
        if (ev->type() == QEvent::DragEnter) {
            auto* de = static_cast<QDragEnterEvent*>(ev);
            if (de->mimeData()->hasUrls()) {
                bool ok=false;
                for (const auto& u : de->mimeData()->urls())
                    if (u.toLocalFile().endsWith(".jar")) { ok=true; break; }
                if (ok) { de->acceptProposedAction(); return true; }
            }
        } else if (ev->type() == QEvent::Drop) {
            auto* dd = static_cast<QDropEvent*>(ev);
            const QList<QUrl> urls = dd->mimeData()->urls();
            for (const auto& u : urls) {
                const QString src = u.toLocalFile();
                if (!src.endsWith(".jar")) continue;
                QFile::copy(src, QDir(modsDir()).filePath(QFileInfo(src).fileName()));
            }
            refreshInstalledList();
            emit modsChanged();
            return true;
        }
    }
    return QWidget::eventFilter(obj, ev);
}
