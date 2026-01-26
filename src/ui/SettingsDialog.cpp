#include "SettingsDialog.h"
#include "Settings.h"
#include "JavaUtil.h"
#include "Installer.h"
#include "Net.h"
#include "MojangAPI.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QStandardPaths>
#include <QMessageBox>
#include <QFileInfo>
#include <QDir>

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Настройки"));
    resize(680, 520);

    auto* lay = new QVBoxLayout(this);
    tabs_ = new QTabWidget(this);
    lay->addWidget(tabs_, 1);

    tabs_->addTab(buildTabCustomization(), tr("Кастомизация"));
    tabs_->addTab(buildTabJava(),          tr("Java и память"));
    tabs_->addTab(buildTabGeneral(),       tr("Общие"));
    tabs_->addTab(buildTabNetwork(),       tr("Сеть"));

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(btns);

    loadSettings();
    populateJavaCandidates(); // заполним список путей сразу

    connect(btns, &QDialogButtonBox::accepted, this, [this]{ applyAndClose(); });
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QWidget* SettingsDialog::buildTabCustomization()
{
    auto* w = new QWidget(this);
    auto* v = new QVBoxLayout(w);
    v->addStretch(1);
    w->setLayout(v);
    return w;
}

QWidget* SettingsDialog::buildTabJava()
{
    auto* w = new QWidget(this);
    auto* f = new QFormLayout(w);

    // Путь к Java (выпадающий список + ручное редактирование)
    cbJavaPath_ = new QComboBox(w);
    cbJavaPath_->setEditable(true);
    cbJavaPath_->setInsertPolicy(QComboBox::NoInsert);
    cbJavaPath_->setDuplicatesEnabled(false);

    // Доп. JVM
    cbJvmArgs_ = new QComboBox(w);
    cbJvmArgs_->setEditable(true);
    cbJvmArgs_->setInsertPolicy(QComboBox::NoInsert);

    // Память
    cbAutoRam_ = new QCheckBox(tr("Автоматически выбирать память"), w);
    sbMaxRam_  = new QSpinBox(w);
    sbMaxRam_->setRange(512, 65536);

    f->addRow(tr("Путь к Java:"), cbJavaPath_);
    f->addRow(tr("Доп. JVM аргументы:"), cbJvmArgs_);
    f->addRow(tr("Авто-RAM:"), cbAutoRam_);
    f->addRow(tr("Максимум RAM, MiB"), sbMaxRam_);

    // Блок установки Java: список версий + кнопка
    cbJavaVersion_ = new QComboBox(w);
    cbJavaVersion_->addItem("21", 21);
    cbJavaVersion_->addItem("17", 17);
    btnGetJava_ = new QPushButton(tr("Скачать выбранную (Temurin JRE)"), w);

    auto* hb = new QHBoxLayout();
    hb->addWidget(new QLabel(tr("Версия JRE:")));
    hb->addWidget(cbJavaVersion_, 1);
    hb->addWidget(btnGetJava_);

    auto* wrap = new QWidget(w);
    wrap->setLayout(hb);
    f->addRow(wrap);

    // Кнопка автодетекта
    auto* btnDetect = new QPushButton(tr("Автодетект Java"), w);
    f->addRow(btnDetect);

    // Подключения
    connect(btnDetect,   &QPushButton::clicked, this, &SettingsDialog::onDetectJava);
    connect(btnGetJava_, &QPushButton::clicked, this, &SettingsDialog::onDownloadJavaSelected);

    w->setLayout(f);
    return w;
}

QWidget* SettingsDialog::buildTabGeneral()
{
    auto* w = new QWidget(this);
    auto* f = new QFormLayout(w);

    // Язык — минимальная реализация "system" / "ru" / "en"
    cbLanguage_ = new QComboBox(w);
    cbLanguage_->addItem(tr("Системный"), "system");
    cbLanguage_->addItem("Русский", "ru");
    cbLanguage_->addItem("English", "en");
    f->addRow(tr("Язык интерфейса:"), cbLanguage_);

    w->setLayout(f);
    return w;
}

QWidget* SettingsDialog::buildTabNetwork()
{
    auto* w = new QWidget(this);
    auto* f = new QFormLayout(w);

    cbUseSystemProxy_ = new QCheckBox(tr("Использовать системный прокси"), w);
    leNoProxy_        = new QLineEdit(w);
    leNoProxy_->setPlaceholderText(tr("напр.: localhost,127.0.0.1,::1,example.com"));

    f->addRow(cbUseSystemProxy_);
    f->addRow(tr("NO_PROXY:"), leNoProxy_);

    w->setLayout(f);
    return w;
}

void SettingsDialog::loadSettings()
{
    QSettings s;

    // java
    const QString javaPath = s.value("java/defaultPath", s.value("paths/javaPath")).toString();
    cbJavaPath_->setEditText(javaPath);
    cbJvmArgs_->setEditText(s.value("java/jvmArgs").toString());
    cbAutoRam_->setChecked(s.value("java/autoRam", true).toBool());
    sbMaxRam_->setValue(s.value("java/maxRamMiB", 2048).toInt());

    // язык
    const QString lang = s.value("ui/language", "system").toString();
    int idx = cbLanguage_->findData(lang);
    if (idx < 0) idx = 0;
    cbLanguage_->setCurrentIndex(idx);

    // сеть
    cbUseSystemProxy_->setChecked(s.value("network/useSystemProxy", true).toBool());
    leNoProxy_->setText(s.value("network/noProxy").toString());
}

void SettingsDialog::applyAndClose()
{
    QSettings s;

    // java: пишем и новый, и старый ключ для обратной совместимости
    const QString jp = cbJavaPath_->currentText().trimmed();
    s.setValue("java/defaultPath", jp);
    s.setValue("paths/javaPath",    jp);
    s.setValue("java/jvmArgs",      cbJvmArgs_->currentText().trimmed());
    s.setValue("java/autoRam",      cbAutoRam_->isChecked());
    s.setValue("java/maxRamMiB",    sbMaxRam_->value());

    // язык
    s.setValue("ui/language", cbLanguage_->currentData().toString());

    // сеть
    s.setValue("network/useSystemProxy", cbUseSystemProxy_->isChecked());
    s.setValue("network/noProxy",        leNoProxy_->text().trimmed());

    emit settingsChanged();
    accept();
}

void SettingsDialog::populateJavaCandidates()
{
    QSet<QString> seen;
    auto uniqAdd = [&](const QString& x){
        const QString v = QFileInfo(x).canonicalFilePath();
        if (!v.isEmpty() && !seen.contains(v) && QFileInfo(v).isExecutable())
            { cbJavaPath_->addItem(v); seen.insert(v); }
    };

    cbJavaPath_->clear();

    // 1) текущее значение из настроек
    QString current = cbJavaPath_->currentText().trimmed();
    if (!current.isEmpty()) uniqAdd(current);

    // 2) кандидаты из JavaUtil
    for (const auto& p : JavaUtil::candidatesFromEnv()) uniqAdd(p);

    // 3) локальные инсталляции в <gameDir>/runtime/java-*
    QSettings s;
    const QString gameDir = s.value("paths/gameDir").toString();
    if (!gameDir.isEmpty()) {
        const QDir rt(QDir(gameDir).filePath("runtime"));
        if (rt.exists()) {
            for (const auto& d : rt.entryList(QStringList() << "java-*", QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QString bin = QDir(rt.filePath(d)).filePath("bin/java");
                uniqAdd(bin);
            }
        }
    }

    // 4) PATH / common уже покрыты JavaUtil, но оставим запасной
#ifdef Q_OS_LINUX
    uniqAdd("/usr/bin/java");
    uniqAdd("/usr/local/bin/java");
#endif
}

void SettingsDialog::onDetectJava()
{
    const QString cur = cbJavaPath_->currentText().trimmed();
    if (!cur.isEmpty() && QFileInfo(cur).isExecutable()) {
        QMessageBox::information(this, tr("Готово"), tr("Java уже указана: %1").arg(cur));
        return;
    }

    auto found = JavaUtil::detectJava(17);
    if (found.has_value()) {
        cbJavaPath_->setEditText(found.value());
        QSettings s;
        s.setValue("java/defaultPath", found.value());
        s.setValue("paths/javaPath",   found.value());
        populateJavaCandidates();
        QMessageBox::information(this, tr("Готово"), tr("Найдена Java: %1").arg(found.value()));
    } else {
        QMessageBox::warning(this, tr("Не найдено"), tr("Подходящая Java не обнаружена."));
    }
}

void SettingsDialog::onDownloadJavaSelected()
{
    QSettings s;
    const QString gameDir = s.value("paths/gameDir").toString();
    if (gameDir.isEmpty()) {
        QMessageBox::warning(this, tr("Ошибка"), tr("Сначала укажите папку игры на вкладке «Общие»."));
        return;
    }

    const int major = cbJavaVersion_->currentData().toInt();
    try {
        Net net;
        MojangAPI api(net);
        Installer inst(api, gameDir);

        const QString destBase = QDir(gameDir).filePath("runtime");
        const QString java = inst.installTemurinJre(major, destBase);
        if (!java.isEmpty()) {
            cbJavaPath_->setEditText(java);
            s.setValue("java/defaultPath", java);
            s.setValue("paths/javaPath",   java);
            populateJavaCandidates();
            QMessageBox::information(this, tr("Готово"), tr("Установлена Java %1: %2").arg(major).arg(java));
        } else {
            QMessageBox::warning(this, tr("Не поддерживается"),
                                 tr("Установка Java для этой ОС не реализована."));
        }
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Ошибка"),
                              QString("Не удалось установить Java: %1").arg(e.what()));
    }
}
