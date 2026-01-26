#include "InstanceEditorDialog.h"
#include "ModManagerWidget.h"

#include <QtWidgets>
#include <QtConcurrent>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonArray>
#include <QEvent>
#include <QImageReader>
#include <QImageWriter>
#include <QRandomGenerator>
#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>
#include <QDir>
#include <QFileInfo>

#include "../InstanceStore.h"
#include "../LoaderPatchIO.h"
#include "../Settings.h"
#include "../ModLoader.h"
#include "../Net.h"

// ───── JSON helpers ─────────────────────────────────────────
QString InstanceEditorDialog::metaPath(const QString& instDir) {
    return QDir(instDir).filePath("instance.json");
}
QJsonObject InstanceEditorDialog::loadMeta(const QString& instDir) {
    QFile f(metaPath(instDir));
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return {};
    const auto doc = QJsonDocument::fromJson(f.readAll());
    return doc.isObject() ? doc.object() : QJsonObject{};
}
bool InstanceEditorDialog::saveMeta(const QString& instDir, const QJsonObject& o) {
    QFile f(metaPath(instDir));
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

// ───── ctor ────────────────────────────────────────────────
InstanceEditorDialog::InstanceEditorDialog(QString instanceDir,
                                           QString mcVersion,
                                           QString javaDefault,
                                           QWidget* parent)
    : QDialog(parent),
      instanceDir_(std::move(instanceDir)),
      mcVersion_(std::move(mcVersion)),
      javaDefault_(std::move(javaDefault))
{
    setWindowTitle(tr("Изменить сборку"));
    resize(820, 560);

    auto* lay = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);
    tabs_->setTabPosition(QTabWidget::North);
    lay->addWidget(tabs_, 1);

    tabs_->addTab(buildTabGeneral(),   tr("Общее"));
    tabs_->addTab(buildTabModloader(), tr("Модлоадер"));
    tabs_->addTab(buildTabJavaMem(),   tr("Java и память"));
    tabs_->addTab(buildTabGame(),      tr("Игра"));
    tabs_->addTab(buildTabFiles(),     tr("Файлы"));

    // «Моды» — создаём здесь между "Общее" и "Модлоадер"
    QString loaderKind = "none";
    {
        QFile f(QDir(instanceDir_).filePath("instance.json"));
        if (f.open(QIODevice::ReadOnly)) {
            const auto jd = QJsonDocument::fromJson(f.readAll());
            if (jd.isObject()) {
                const auto ml = jd.object().value("modloader").toObject();
                loaderKind = ml.value("kind").toString("none");
            }
        }
    }
    auto* mods = new ModManagerWidget(instanceDir_, mcVersion_, loaderKind, this);
    tabs_->insertTab(1, mods, tr("Моды"));
    connect(mods, &ModManagerWidget::modsChanged, this, [this]{ /* hook if needed */ });
}

// ───── General ─────────────────────────────────────────────
static QPixmap rounded(const QPixmap& src, int radius) {
    if (src.isNull()) return {};
    QPixmap out(src.size()); out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path; path.addRoundedRect(out.rect(), radius, radius);
    p.setClipPath(path);
    p.drawPixmap(0,0,src);
    return out;
}
static int ensureIconHueLocal(const QString& instDir) {
    QFile f(QDir(instDir).filePath("instance.json"));
    QJsonObject meta;
    if (f.exists() && f.open(QIODevice::ReadOnly)) { meta = QJsonDocument::fromJson(f.readAll()).object(); f.close(); }
    auto icons = meta.value("icons").toObject();
    int hue = icons.value("hue").toInt(-1);
    if (hue < 0) {
        hue = QRandomGenerator::global()->bounded(360);
        icons.insert("hue", hue);
        meta.insert("icons", icons);
        if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(meta).toJson(QJsonDocument::Indented)); f.close(); }
    }
    return hue % 360;
}
static QPixmap makePlaceholderLocal(const QString& title, int size, int radius, const QString& instDir) {
    QPixmap pm(size, size); pm.fill(Qt::transparent);
    QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing, true);
    const QColor bg = QColor::fromHsv(ensureIconHueLocal(instDir), 170, 230);
    p.setBrush(bg); p.setPen(Qt::NoPen);
    QPainterPath path; path.addRoundedRect(QRect(0,0,size,size), radius, radius);
    p.drawPath(path);
    const QString letter = title.trimmed().isEmpty() ? "?" : title.trimmed().left(1).toUpper();
    QFont f = p.font(); f.setBold(true); f.setPointSizeF(size*0.44); p.setFont(f);
    p.setPen(Qt::white); p.drawText(QRect(0,0,size,size), Qt::AlignCenter, letter);
    return pm;
}
static QPixmap currentIconPreview(const QString& instDir, const QString& title, int size, int radius) {
    const QString iconPath = QDir(instDir).filePath("icon.png");
    if (QFile::exists(iconPath)) {
        QPixmap src(iconPath);
        if (!src.isNull()) return rounded(src.scaled(size,size,Qt::KeepAspectRatioByExpanding,Qt::SmoothTransformation), radius);
    }
    return makePlaceholderLocal(title, size, radius, instDir);
}

QWidget* InstanceEditorDialog::buildTabGeneral() {
    auto* w = new QWidget(this);
    auto* f = new QFormLayout(w);

    const auto meta = loadMeta(instanceDir_);

    edName_  = new QLineEdit(this);
    edGroup_ = new QLineEdit(this);
    lblMcVersion_ = new QLabel(mcVersion_, this);
    lblInstDir_   = new QLabel(QDir::toNativeSeparators(instanceDir_), this);
    lblInstDir_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    const QString tailName = QFileInfo(instanceDir_).fileName();
    edName_->setText(meta.value("displayName").toString(tailName));
    edGroup_->setText(meta.value("group").toString());

    // --- Иконка ---
    auto* iconRow   = new QHBoxLayout;
    auto* iconPrev  = new QLabel(this);
    iconPrev->setFixedSize(96,96);
    iconPrev->setScaledContents(true);
    iconPrev->setPixmap(currentIconPreview(instanceDir_, edName_->text(), 96, 20));

    auto* btnPick   = new QPushButton(tr("Выбрать…"), this);
    auto* btnReset  = new QPushButton(tr("Сбросить"), this);

    iconRow->addWidget(iconPrev);
    auto* iconBtnCol = new QVBoxLayout;
    iconBtnCol->addWidget(btnPick);
    iconBtnCol->addWidget(btnReset);
    iconBtnCol->addStretch();
    iconRow->addLayout(iconBtnCol);

    // заметки и «Открыть папку»
    notes_ = new QTextEdit(this);
    notes_->setPlaceholderText(tr("Заметки к сборке…"));
    notes_->setPlainText(meta.value("notes").toString());

    auto* openDir = new QPushButton(tr("Открыть папку…"), this);
    connect(openDir, &QPushButton::clicked, this, [this]{ QDesktopServices::openUrl(QUrl::fromLocalFile(instanceDir_)); });

    // сборка формы
    f->addRow(tr("Название:"),   edName_);
    f->addRow(tr("Группа:"),     edGroup_);
    f->addRow(tr("Версия MC:"),  lblMcVersion_);
    f->addRow(tr("Каталог:"),    lblInstDir_);
    f->addRow(tr("Иконка:"),     iconRow);
    f->addRow(tr("Заметки:"),    notes_);
    f->addRow(QString(),         openDir);

    // обработчики иконки
    connect(btnPick, &QPushButton::clicked, this, [=]{
        const QString file = QFileDialog::getOpenFileName(this, tr("Выбрать иконку"),
                                                          QDir::homePath(),
                                                          tr("Изображения (*.png *.jpg *.jpeg *.bmp)"));
        if (file.isEmpty()) return;
        QImage img(file);
        if (img.isNull()) {
            QMessageBox::warning(this, {}, tr("Не удалось прочитать изображение."));
            return;
        }
        const QString outPath = QDir(instanceDir_).filePath("icon.png");
        if (!img.save(outPath, "PNG")) {
            QMessageBox::warning(this, {}, tr("Не удалось сохранить иконку."));
            return;
        }
        iconPrev->setPixmap(currentIconPreview(instanceDir_, edName_->text(), 96, 20));
    });
    connect(btnReset, &QPushButton::clicked, this, [=]{
        QFile::remove(QDir(instanceDir_).filePath("icon.png"));
        iconPrev->setPixmap(currentIconPreview(instanceDir_, edName_->text(), 96, 20));
    });

    return w;
}

void InstanceEditorDialog::applyGeneral() {
    QJsonObject meta = loadMeta(instanceDir_);
    meta.insert("displayName", edName_->text().trimmed());
    meta.insert("group",       edGroup_->text().trimmed());
    meta.insert("notes",       notes_->toPlainText());
    saveMeta(instanceDir_, meta);
    emit instanceChanged();
}

// ───── Modloader ──────────────────────────────────────────
enum class Kind { None, Fabric, Quilt, Forge, NeoForge };

QWidget* InstanceEditorDialog::buildTabModloader() {
    auto* w = new QWidget(this);
    auto* v = new QVBoxLayout(w);

    auto* form = new QFormLayout;
    mlKind_ = new QComboBox(this);
    mlKind_->addItem(tr("Нет"),      (int)Kind::None);
    mlKind_->addItem("Fabric",       (int)Kind::Fabric);
    mlKind_->addItem("Quilt",        (int)Kind::Quilt);
    mlKind_->addItem("Forge",        (int)Kind::Forge);
    mlKind_->addItem("NeoForge",     (int)Kind::NeoForge);

    mlVersion_ = new QLineEdit(this);
    mlVersion_->setPlaceholderText(tr("Версия лоадера, оставить пусто = latest"));

    form->addRow(tr("Тип:"),    mlKind_);
    form->addRow(tr("Версия:"), mlVersion_);
    v->addLayout(form);

    mlStatus_ = new QLabel(this);
    mlStatus_->setWordWrap(true);
    mlStatus_->setStyleSheet("color: gray;");
    v->addWidget(mlStatus_);

    auto* row = new QHBoxLayout;
    btnMlInstall_ = new QPushButton(tr("Установить / обновить"), this);
    btnMlRemove_  = new QPushButton(tr("Снять лоадер"), this);
    row->addWidget(btnMlInstall_);
    row->addWidget(btnMlRemove_);
    row->addStretch();
    v->addLayout(row);

    v->addStretch();

    connect(btnMlInstall_, &QPushButton::clicked, this, &InstanceEditorDialog::installOrUpdateModloader);
    connect(btnMlRemove_,  &QPushButton::clicked, this, &InstanceEditorDialog::removeModloader);

    const auto meta = loadMeta(instanceDir_);
    const auto ml   = meta.value("modloader").toObject();
    const QString kind = ml.value("kind").toString();
    const QString ver  = ml.value("version").toString();
    mlVersion_->setText(ver);

    auto pickKind = [&](const QString& s){
        Kind k = Kind::None;
        if (s=="fabric") k=Kind::Fabric; else
        if (s=="quilt")  k=Kind::Quilt;  else
        if (s=="forge")  k=Kind::Forge;  else
        if (s=="neoforge") k=Kind::NeoForge; else k=Kind::None;
        for (int i=0;i<mlKind_->count();++i)
            if (mlKind_->itemData(i).toInt() == (int)k) { mlKind_->setCurrentIndex(i); break; }
    };
    pickKind(kind);

    refreshModloaderUi();
    return w;
}

void InstanceEditorDialog::refreshModloaderUi() {
    auto opt = LoaderPatchIO::tryLoad(instanceDir_);
    if (opt.has_value()) {
        mlStatus_->setText(tr("Лоадер установлен (patch json найден). Будет использован при запуске."));
        btnMlRemove_->setEnabled(true);
    } else {
        mlStatus_->setText(tr("Лоадер не установлен. Можно установить выбранный выше."));
        btnMlRemove_->setEnabled(false);
    }
}

void InstanceEditorDialog::installOrUpdateModloader() {
    const Kind k = (Kind)mlKind_->currentData().toInt();
    const QString loaderVer = mlVersion_->text().trimmed(); // "" => latest
    if (k == Kind::None) {
        QMessageBox::information(this, {}, tr("Выберите тип лоадера."));
        return;
    }

    QJsonObject meta = loadMeta(instanceDir_);
    QJsonObject ml;
    QString kindStr = "none";
    if (k==Kind::Fabric) kindStr="fabric";
    else if (k==Kind::Quilt) kindStr="quilt";
    else if (k==Kind::Forge) kindStr="forge";
    else if (k==Kind::NeoForge) kindStr="neoforge";
    ml.insert("kind", kindStr);
    ml.insert("version", loaderVer);
    meta.insert("modloader", ml);
    saveMeta(instanceDir_, meta);

    mlStatus_->setText(tr("Установка…"));
    setEnabled(false);

    auto fut = QtConcurrent::run([=]{
        try {
            Net net;
            ModloaderInstaller mli(net, instanceDir_);
            LoaderPatch patch;
            if (k==Kind::Fabric)       patch = mli.installFabric(mcVersion_, loaderVer);
            else if (k==Kind::Quilt)   patch = mli.installQuilt (mcVersion_, loaderVer);
            else if (k==Kind::Forge)   patch = mli.installForge (mcVersion_, loaderVer);
            else if (k==Kind::NeoForge)patch = mli.installNeoForge(mcVersion_, loaderVer);

            if (!LoaderPatchIO::save(instanceDir_, patch))
                throw std::runtime_error("failed to write loader.patch.json");

            QMetaObject::invokeMethod((QObject*)this, [this]{
                setEnabled(true);
                refreshModloaderUi();
                QMessageBox::information(this, {}, tr("Готово! Лоадер установлен."));
            }, Qt::QueuedConnection);
        } catch (const std::exception& e) {
            const QString msg = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod((QObject*)this, [this, msg]{
                setEnabled(true);
                refreshModloaderUi();
                QMessageBox::warning(this, tr("Ошибка установки"),
                                     tr("Не удалось установить лоадер: %1").arg(msg));
            }, Qt::QueuedConnection);
        }
    });
    Q_UNUSED(fut);
}

void InstanceEditorDialog::removeModloader() {
    if (QMessageBox::question(this, {}, tr("Снять модлоадер?")) != QMessageBox::Yes) return;

    QFile::remove(QDir(instanceDir_).filePath("loader.patch.json"));
    QJsonObject meta = loadMeta(instanceDir_);
    meta.remove("modloader");
    saveMeta(instanceDir_, meta);

    refreshModloaderUi();
}

// ───── Java & Memory ──────────────────────────────────────
QWidget* InstanceEditorDialog::buildTabJavaMem() {
    auto* w = new QWidget(this);
    auto* f = new QFormLayout(w);

    cbJavaOverride_ = new QCheckBox(tr("Переопределить для этой сборки"), this);
    f->addRow(cbJavaOverride_);

    edJavaPath_ = new QLineEdit(javaDefault_, this);
    edJvmArgs_  = new QLineEdit(this);
    cbAutoRam_  = new QCheckBox(tr("Автоматически"), this);
    sbMaxRam_   = new QSpinBox(this);
    sbMaxRam_->setRange(512, 65536);
    sbMaxRam_->setSingleStep(256);

    const int total = AppSettings::detectTotalRamMiB();
    lblRamHint_ = new QLabel(tr("Всего RAM: ~%1 МБ. Рекомендуемое значение будет использовано при «Автоматически».")
                             .arg(total), this);
    lblRamHint_->setStyleSheet("color: gray");

    const auto meta = loadMeta(instanceDir_);
    const auto java = meta.value("java").toObject();
    const bool ov   = java.value("override").toBool(false);
    cbJavaOverride_->setChecked(ov);
    edJavaPath_->setText(java.value("path").toString(javaDefault_));
    edJvmArgs_->setText(java.value("jvmArgs").toString());
    cbAutoRam_->setChecked(java.value("autoRam").toBool(true));
    sbMaxRam_->setValue(qMax(512, java.value("maxRamMiB").toInt(2048)));

    auto setEnabled = [=](bool on){
        for (auto* w : { (QWidget*)edJavaPath_, (QWidget*)edJvmArgs_,
                         (QWidget*)cbAutoRam_, (QWidget*)sbMaxRam_ })
            w->setEnabled(on);
    };
    setEnabled(ov);
    connect(cbJavaOverride_, &QCheckBox::toggled, this, setEnabled);

    f->addRow(tr("Java:"),      edJavaPath_);
    f->addRow(tr("JVM args:"),  edJvmArgs_);
    f->addRow(tr("Макс. RAM:"), cbAutoRam_);
    f->addRow(QString(),        sbMaxRam_);
    f->addRow(QString(),        lblRamHint_);

    return w;
}

void InstanceEditorDialog::applyJavaMem() {
    QJsonObject meta = loadMeta(instanceDir_);
    QJsonObject java;
    java.insert("override", cbJavaOverride_->isChecked());
    java.insert("path",     edJavaPath_->text().trimmed());
    java.insert("jvmArgs",  edJvmArgs_->text().trimmed());
    java.insert("autoRam",  cbAutoRam_->isChecked());
    java.insert("maxRamMiB",sbMaxRam_->value());
    meta.insert("java", java);
    saveMeta(instanceDir_, meta);
}

// ───── Game ───────────────────────────────────────────────
QWidget* InstanceEditorDialog::buildTabGame() {
    auto* w = new QWidget(this);
    auto* f = new QFormLayout(w);

    cbFullscreen_ = new QCheckBox(tr("Полноэкранный режим"), this);
    sbW_ = new QSpinBox(this); sbW_->setRange(320, 8192); sbW_->setValue(854);
    sbH_ = new QSpinBox(this); sbH_->setRange(240, 8192); sbH_->setValue(480);
    edGameArgs_ = new QLineEdit(this);
    edGameArgs_->setPlaceholderText(tr("--demo, --disableMultiplayer и т.п."));

    const auto meta = loadMeta(instanceDir_);
    const auto game = meta.value("game").toObject();
    cbFullscreen_->setChecked(game.value("fullscreen").toBool(false));
    sbW_->setValue(game.value("width").toInt(854));
    sbH_->setValue(game.value("height").toInt(480));
    edGameArgs_->setText(game.value("args").toString());

    f->addRow(cbFullscreen_);
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Ширина:")));
    row->addWidget(sbW_);
    row->addSpacing(10);
    row->addWidget(new QLabel(tr("Высота:")));
    row->addWidget(sbH_);
    f->addRow(row);
    f->addRow(tr("Доп. аргументы игры:"), edGameArgs_);

    return w;
}
void InstanceEditorDialog::applyGame() {
    QJsonObject meta = loadMeta(instanceDir_);
    QJsonObject game;
    game.insert("fullscreen", cbFullscreen_->isChecked());
    game.insert("width",      sbW_->value());
    game.insert("height",     sbH_->value());
    game.insert("args",       edGameArgs_->text().trimmed());
    meta.insert("game", game);
    saveMeta(instanceDir_, meta);
}

// ───── Files ──────────────────────────────────────────────
QWidget* InstanceEditorDialog::buildTabFiles() {
    auto* w = new QWidget(this);
    auto* v = new QVBoxLayout(w);

    auto addBtn = [&](const QString& text, const QString& sub){
        auto* b = new QPushButton(text, this);
        connect(b, &QPushButton::clicked, this, [=]{
            QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(instanceDir_).filePath(sub)));
        });
        v->addWidget(b);
    };

    addBtn(tr("Открыть папку сборки"),  ".");
    addBtn(tr("Папка worlds"),          "saves");
    addBtn(tr("Папка mods"),            "mods");
    addBtn(tr("Папка resourcepacks"),   "resourcepacks");
    addBtn(tr("Папка shaderpacks"),     "shaderpacks");
    addBtn(tr("Папка logs"),            "logs");
    addBtn(tr("Папка screenshots"),     "screenshots");

    // Экспорт/Импорт списка модов
    auto* row = new QHBoxLayout;
    auto* btnExport = new QPushButton(tr("Экспорт списка модов…"), this);
    auto* btnImport = new QPushButton(tr("Импорт списка модов…"), this);
    row->addWidget(btnExport);
    row->addWidget(btnImport);
    row->addStretch();
    v->addLayout(row);

    connect(btnExport, &QPushButton::clicked, this, &InstanceEditorDialog::exportModsList);
    connect(btnImport, &QPushButton::clicked, this, &InstanceEditorDialog::importModsList);

    v->addStretch();
    return w;
}

// ───── Export/Import mods list ────────────────────────────
void InstanceEditorDialog::exportModsList()
{
    const QString modsPath = QDir(instanceDir_).filePath("mods");
    const QDir md(modsPath);
    const auto files = md.entryInfoList(QStringList() << "*.jar" << "*.zip", QDir::Files, QDir::Name);

    QJsonArray arr;
    for (const QFileInfo& fi : files) {
        QJsonObject o;
        o.insert("file", fi.fileName());
        o.insert("size", (qint64)fi.size());
        o.insert("mtime", fi.lastModified().toSecsSinceEpoch());
        arr.push_back(o);
    }
    QJsonObject root; root.insert("mods", arr);

    const QString out = QFileDialog::getSaveFileName(this, tr("Сохранить список модов"),
                                                     QDir::home().filePath("mods-list.json"),
                                                     tr("JSON (*.json)"));
    if (out.isEmpty()) return;

    QFile f(out);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, {}, tr("Не удалось открыть файл для записи."));
        return;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    QMessageBox::information(this, {}, tr("Сохранено: %1 мод(ов)").arg(files.size()));
}

void InstanceEditorDialog::importModsList()
{
    const QString in = QFileDialog::getOpenFileName(this, tr("Открыть список модов"),
                                                    QDir::homePath(),
                                                    tr("JSON (*.json)"));
    if (in.isEmpty()) return;

    QFile f(in);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, {}, tr("Не удалось прочитать файл."));
        return;
    }
    const auto doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) {
        QMessageBox::warning(this, {}, tr("Некорректный формат JSON."));
        return;
    }
    const auto mods = doc.object().value("mods").toArray();
    if (mods.isEmpty()) {
        QMessageBox::information(this, {}, tr("Список пуст."));
        return;
    }

    QDir md(QDir(instanceDir_).filePath("mods"));
    md.mkpath(".");

    int copied = 0;
    for (const auto& v : mods) {
        const auto o = v.toObject();
        const QString fname = o.value("file").toString();
        if (fname.isEmpty()) continue;

        // попросим выбрать исходный файл, если его нет рядом с JSON (удобно для переносов)
        QString srcCandidate = QFileInfo(in).dir().filePath(fname);
        if (!QFile::exists(srcCandidate)) {
            srcCandidate = QFileDialog::getOpenFileName(this, tr("Найти файл %1").arg(fname),
                                                        QDir::homePath(),
                                                        tr("Моды (*.jar *.zip)"));
            if (srcCandidate.isEmpty()) continue;
        }

        QFile::copy(srcCandidate, md.filePath(fname));
        ++copied;
    }
    QMessageBox::information(this, {}, tr("Импортировано файлов: %1").arg(copied));
}
