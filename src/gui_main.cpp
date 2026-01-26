#include <QApplication>
#include <QCoreApplication>
#include "ui/MainWindow.h"

int main(int argc, char** argv)
{
    QCoreApplication::setOrganizationName("Tesuto");
    QCoreApplication::setApplicationName("TesutoLauncher");
    QApplication app(argc, argv);

    MainWindow w;
    w.resize(720, 480);
    w.show();
    return app.exec();
}
