#pragma once
#include <QDialog>
#include <QJsonObject>

class QTabWidget;
class QTextEdit;
class QLabel;
class QLineEdit;
class QCheckBox;
class QSpinBox;

class InstanceEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit InstanceEditorDialog(QString instanceDir,
                                  QString mcVersion,
                                  QString javaDefault,
                                  QWidget* parent = nullptr);

signals:
    void instanceChanged();

private:
    // JSON helpers
    static QString    metaPath(const QString& instDir);
    static QJsonObject loadMeta(const QString& instDir);
    static bool        saveMeta(const QString& instDir, const QJsonObject& o);

    // tabs
    QWidget* buildTabGeneral();
    QWidget* buildTabModloader();
    QWidget* buildTabJavaMem();
    QWidget* buildTabGame();
    QWidget* buildTabFiles();

    // apply
    void applyGeneral();
    void applyJavaMem();
    void applyGame();

    // modloader actions
    void refreshModloaderUi();
    void installOrUpdateModloader();
    void removeModloader();

    // export/import list of mods
    void exportModsList();
    void importModsList();

private:
    QString instanceDir_;
    QString mcVersion_;
    QString javaDefault_;

    QTabWidget* tabs_{};

    // General
    QLineEdit* edName_{};
    QLineEdit* edGroup_{};
    QTextEdit* notes_{};
    QLabel*    lblMcVersion_{};
    QLabel*    lblInstDir_{};

    // Modloader
    class QComboBox* mlKind_{};
    class QLineEdit* mlVersion_{};
    class QLabel*    mlStatus_{};
    class QPushButton* btnMlInstall_{};
    class QPushButton* btnMlRemove_{};

    // Java/mem
    QCheckBox* cbJavaOverride_{};
    QLineEdit* edJavaPath_{};
    QLineEdit* edJvmArgs_{};
    QCheckBox* cbAutoRam_{};
    QSpinBox*  sbMaxRam_{};
    QLabel*    lblRamHint_{};

    // Game
    QCheckBox* cbFullscreen_{};
    QSpinBox*  sbW_{};
    QSpinBox*  sbH_{};
    QLineEdit* edGameArgs_{};
};
