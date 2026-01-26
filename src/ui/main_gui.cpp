#include "i18n.h"
#include <QApplication>
#include "SettingsMigration.h"
#include <QTranslator>
#include <QLocale>
#include <QSettings>
#include <QDir>
#include <QStandardPaths>
#include <QFileInfo>




#include "MainWindow.h"

QTranslator g_appTr;
QTranslator g_qtTr;

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    // Ensure all QSettings() calls use a single, consistent scope.
    QCoreApplication::setOrganizationName("Tesuto");
    QCoreApplication::setApplicationName("TesutoLauncher");

    // Migrate legacy settings (tesuto/launcher -> Tesuto/TesutoLauncher)
    SettingsMigration::migrate();

    QSettings s;
    const QString lang = s.value("ui/language", "system").toString();
    I18n::install(lang, g_appTr, g_qtTr);

    MainWindow w;
    w.show();
    return app.exec();
}
