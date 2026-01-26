#pragma once
#include <QtWidgets>

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

signals:
    void settingsChanged();

private:
    // Вкладки
    QTabWidget* tabs_ = nullptr;

    // Java & память
    QComboBox* cbJavaPath_ = nullptr;   // список доступных java + редактируемый путь
    QComboBox* cbJvmArgs_  = nullptr;
    QCheckBox* cbAutoRam_  = nullptr;
    QSpinBox*  sbMaxRam_   = nullptr;

    // Селектор версии для установки
    QComboBox* cbJavaVersion_ = nullptr; // 17, 21
    QPushButton* btnGetJava_  = nullptr; // "Скачать выбранную"

    // Язык
    QComboBox* cbLanguage_ = nullptr;

    // Сеть
    QCheckBox* cbUseSystemProxy_ = nullptr;
    QLineEdit* leNoProxy_ = nullptr;

    // Построители вкладок
    QWidget* buildTabCustomization();
    QWidget* buildTabJava();
    QWidget* buildTabGeneral();
    QWidget* buildTabNetwork();

    // Загрузка/сохранение настроек
    void loadSettings();
    void applyAndClose();

    // Наполнение списка путей к Java
    void populateJavaCandidates();

private slots:
    // Java
    void onDetectJava();
    void onDownloadJavaSelected();
};
