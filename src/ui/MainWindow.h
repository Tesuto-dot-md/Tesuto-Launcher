#pragma once

#include <QMainWindow>
class QLabel;
class QProgressBar;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

public slots:
    // Вызываются из MainWindow.cpp
    void installVersion();
    void launchOffline();

    // Управление "занято" (используется из фоновых задач через RAII-гарду)
    void beginBusy(const QString& msg = QString());
    void endBusy();

private slots:
    // Если есть QAction с objectName "actionPlay"
    void on_actionPlay_triggered();
    // Если есть QPushButton с objectName "playButton"
    void on_playButton_clicked();
    // Универсальный слот — можно связать вручную (connect)
    void onPlayClicked();

private:
    // Единая логика запуска (онлайн/оффлайн)
    void doPlay();

    // Применить текущее состояние "занято" к UI
    void applyBusyUi();

private:
    int         busyCount_ = 0;     // поддержка вложенных beginBusy()/endBusy()
    QLabel*     sbText_    = nullptr; // текст в статусбаре
    QProgressBar* sbProg_  = nullptr; // индикатор "крутилка" в статусбаре
    void removeLaunchActiveButtonIfAny();

protected:
    void changeEvent(QEvent* e) override;
};
