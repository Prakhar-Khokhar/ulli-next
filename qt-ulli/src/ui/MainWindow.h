// ui/MainWindow.h
//
// ULLI main window. Hosts the distro selector, status, progress bar,
// log view, and Start / Exit buttons. Starts and supervises the
// QThread that runs the install.

#pragma once

#include "core/Catalog.h"
#include "core/InstallEngine.h"
#include "core/InstallPlan.h"
#include "core/ProgressLog.h"

#include <QMainWindow>
#include <QPointer>
#include <QThread>

class QLabel;
class QProgressBar;
class QPushButton;

namespace ulli::ui {

class DistroSelector;
class LogView;
class RestartCountdownDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onStartClicked();
    void onExitClicked();
    void onEngineStageChanged(QString stage);
    void onEngineProgressChanged(int percent);
    void onEngineFinished(bool success, QString message);
    void onRestartCountdownFinished();
    void onRestartCountdownCancelled();

private:
    void setBusy(bool busy);
    void startEngine(core::InstallPlan plan);
    void showRestartCountdown();

    core::Catalog catalog_;
    bool fallbackUsed_ = false;

    DistroSelector* distro_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QProgressBar* progress_ = nullptr;
    LogView* log_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QPushButton* exitBtn_ = nullptr;

    core::ProgressLog logBackend_;
    QThread* engineThread_ = nullptr;
    QPointer<core::InstallEngine> engine_;

    RestartCountdownDialog* restartDialog_ = nullptr;
};

}  // namespace ulli::ui
