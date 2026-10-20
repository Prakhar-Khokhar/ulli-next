// ui/MainWindow.cpp
#include "ui/MainWindow.h"
#include "ui/DistroSelector.h"
#include "ui/LogView.h"
#include "ui/PlanDialog.h"
#include "ui/RestartCountdownDialog.h"

#include "platform/Platform.h"

#include <QApplication>
#include <QtConcurrent/QtConcurrent>
#include <QDateTime>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <utility>

namespace ulli::ui {

namespace {

QString makeLogPath() {
    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    return QString::fromStdString(
        (std::filesystem::path(ulli::platform::cacheDir()) /
         ("ulli-install-" + stamp.toStdString() + ".log")).string());
}

// Upper bound for how long the GUI waits for the install worker to notice
// the cancel request. The worker may be inside a non-cancellable external
// command (PowerShell, robocopy, diskpart), so the wait is bounded and the
// worker is detached if it does not stop in time.
constexpr int kEngineStopTimeoutMs = 5000;

// Creates the platform backend for the current platform.
std::unique_ptr<core::IPlatformBackend> createPlatformBackend() {
    return std::make_unique<platform::DiskOps>();
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("ULLI — USB-less Linux Installer"));
    resize(900, 640);

    // ─── Build UI ────────────────────────────────────────────────────────
    auto* root = new QWidget(this);
    auto* layout = new QVBoxLayout(root);

    distro_ = new DistroSelector(root);
    statusLabel_ = new QLabel(tr("Ready"), root);
    statusLabel_->setStyleSheet("font-weight: bold; color: #335;");
    progress_ = new QProgressBar(root);
    progress_->setRange(0, 100);
    log_ = new LogView(root);
    startBtn_ = new QPushButton(tr("Start installation"), root);
    exitBtn_ = new QPushButton(tr("Exit"), root);
    startBtn_->setMinimumHeight(36);
    startBtn_->setStyleSheet("background:#4a8; color:white; font-weight:bold;");

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    btnRow->addWidget(startBtn_);
    btnRow->addWidget(exitBtn_);

    layout->addWidget(distro_);
    layout->addWidget(statusLabel_);
    layout->addWidget(progress_);
    layout->addWidget(log_, 1);
    layout->addLayout(btnRow);
    setCentralWidget(root);

    logBackendOwner_ = std::make_unique<core::ProgressLog>();
    logBackend_ = logBackendOwner_.get();
    logBackend_->openFile(makeLogPath());
    log_->bindLog(logBackend_);

    // ─── Load catalog ────────────────────────────────────────────────────
    catalogOwner_ = std::make_unique<core::Catalog>(core::Catalog::load(&fallbackUsed_));
    catalog_ = catalogOwner_.get();
    distro_->setCatalog(catalog_);
    if (fallbackUsed_) {
        logBackend_->append(
            tr("distros.json not found next to the executable. Using built-in "
               "catalog — some mirrors may be dead and newer distros are missing."),
            core::ProgressLog::Severity::Warn);
        statusLabel_->setText(tr("Warning: distros.json missing — using built-in catalog"));
    }

    // ─── Wire up ─────────────────────────────────────────────────────────
    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(exitBtn_,  &QPushButton::clicked, this, &MainWindow::onExitClicked);
    connect(distro_, &DistroSelector::isoReadinessChanged,
            this, &MainWindow::onIsoReadinessChanged);

    // ─── Wire up ISO download callback ────────────────────────────────────
    distro_->setDownloadCallback([](const core::Distro& distro,
                                    const std::filesystem::path& destPath,
                                    std::function<void(int, const QString&)> progressCb,
                                    std::function<void(core::Result<std::filesystem::path>)> finishedCb) {
        // Run the (blocking) download off the UI thread. progressCb and
        // finishedCb are responsible for marshalling back to the UI thread.
        QtConcurrent::run([distro, destPath, progressCb, finishedCb]() {
            auto backend = createPlatformBackend();
            auto result = backend->downloadIso(distro, destPath, progressCb);
            finishedCb(result);
        });
    });

    // The install may only start once an ISO is present and verified.
    isoReady_ = distro_->isoReady();
    updateStartButtonState();
}

MainWindow::~MainWindow() {
    // Stop the worker before any member this worker may still touch (the
    // catalog and the log) is destroyed.
    stopEngineThread();
}

std::unique_ptr<core::IPlatformBackend> MainWindow::createBackend() {
    return createPlatformBackend();
}

bool MainWindow::isoReadyForInstall() const {
    return isoReady_;
}

bool MainWindow::hasInstallPrivileges() const {
    return ulli::platform::isElevated();
}

void MainWindow::showInstallResult(bool success, const QString& message) {
    if (success) {
        QMessageBox::information(this, tr("ULLI"),
            tr("Installation completed successfully.\n"
               "You can restart your computer manually to boot into the new system."));
    } else {
        QMessageBox::critical(this, tr("ULLI"),
            tr("Installation failed:\n\n%1").arg(message));
    }
}

void MainWindow::setBusy(bool busy) {
    busy_ = busy;
    exitBtn_->setEnabled(!busy);
    distro_->setEnabled(!busy && !scanning_);
    updateStartButtonState();
}

void MainWindow::updateStartButtonState() {
    const bool ready = !busy_ && !scanning_ && isoReady_;
    startBtn_->setEnabled(ready);
    if (!ready && !busy_ && !scanning_ && !isoReady_) {
        startBtn_->setToolTip(
            tr("The ISO has to be downloaded and checksum-verified before "
               "the installation can start."));
    } else {
        startBtn_->setToolTip(QString());
    }
}

void MainWindow::onIsoReadinessChanged(bool ready) {
    isoReady_ = ready;
    updateStartButtonState();
}

void MainWindow::onStartClicked() {
    // 1. Resolve distro + ISO.
    if (distro_->isCustom()) {
        if (distro_->customIsoPath().isEmpty()) {
            QMessageBox::warning(this, tr("ULLI"),
                tr("Please pick a custom ISO file or uncheck the custom option."));
            return;
        }
    } else {
        if (distro_->selectedKey().isEmpty()) {
            QMessageBox::warning(this, tr("ULLI"),
                tr("Please pick a distribution."));
            return;
        }
    }
    if (!isoReadyForInstall()) {
        QMessageBox::warning(this, tr("ULLI"),
            tr("The ISO is not ready yet. Download it and wait for the "
               "checksum verification to finish before installing."));
        return;
    }

    // 2. Enumerate the disks. This blocks on the platform backend (a
    //    PowerShell round trip on Windows), so it must not run on the GUI
    //    thread — otherwise the window freezes before the planning screen
    //    can be shown.
    beginDiskScan();
}

void MainWindow::beginDiskScan() {
    scanning_ = true;
    updateStartButtonState();
    distro_->setEnabled(false);
    statusLabel_->setText(tr("Scanning disks…"));
    progress_->setValue(0);

    // The backend is created here (cheap, no I/O) and moved into the
    // worker so the worker never touches an object owned by the GUI thread.
    std::shared_ptr<core::IPlatformBackend> backend(createBackend());

    // QPointer guard: if the window is closed while the scan runs, the
    // result is dropped instead of calling into a destroyed object.
    QPointer<MainWindow> guard(this);
    QtConcurrent::run([guard, backend]() {
        std::string error;
        std::vector<core::Disk> disks = backend->enumerateDisks(&error);
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, disks, error]() {
            if (!guard) return;
            guard->onDisksEnumerated(disks, error);
        }, Qt::QueuedConnection);
    });
}

void MainWindow::onDisksEnumerated(std::vector<core::Disk> disks,
                                    const std::string& error) {
    scanning_ = false;
    updateStartButtonState();
    distro_->setEnabled(!busy_);

    if (!error.empty()) {
        logBackend_->append(
            tr("Disk scan failed: %1").arg(QString::fromStdString(error)),
            core::ProgressLog::Severity::Error);
        statusLabel_->setText(tr("Ready"));
        QMessageBox::warning(this, tr("ULLI"),
            tr("Could not read the disk list:\n\n%1")
                .arg(QString::fromStdString(error)));
        return;
    }

    if (disks.empty()) {
        statusLabel_->setText(tr("Ready"));
        QMessageBox::warning(this, tr("ULLI"),
            tr("No disks were detected. Run ULLI as Administrator / root."));
        return;
    }

    // The distro selection cannot change while the scan runs, so it is
    // safe to read it now.
    const std::string distroKey = distro_->isCustom()
        ? std::string(core::kCustomDistroKey)
        : distro_->selectedKey().toStdString();
    const std::filesystem::path isoPath = distro_->isCustom()
        ? std::filesystem::path(distro_->customIsoPath().toStdString())
        : std::filesystem::path();

    // 3. Planning screen.
    core::InstallPlan plan;
    if (!showPlanDialog(disks, distroKey, isoPath, plan)) {
        statusLabel_->setText(tr("Ready"));
        return;
    }
    if (!plan.valid()) {
        statusLabel_->setText(tr("Ready"));
        QMessageBox::warning(this, tr("ULLI"),
            tr("The install plan is invalid (missing distro or ISO)."));
        return;
    }

    // 4. Pre-flight: elevation.
    if (!hasInstallPrivileges()) {
        statusLabel_->setText(tr("Ready"));
        QMessageBox::warning(this, tr("ULLI"),
            tr("ULLI must be run as Administrator / root."));
        return;
    }

    // 5. Hand off to engine.
    startEngine(std::move(plan));
}

bool MainWindow::showPlanDialog(const std::vector<core::Disk>& disks,
                                 const std::string& distroKey,
                                 const std::filesystem::path& isoPath,
                                 core::InstallPlan& outPlan) {
    PlanDialog dlg(this);
    dlg.setDisks(disks);
    if (dlg.exec() != QDialog::Accepted) return false;
    outPlan = dlg.buildPlan(distroKey, isoPath);
    return true;
}

void MainWindow::startEngine(core::InstallPlan plan) {
    if (engineThread_ || workerDetached_) {
        logBackend_->append(tr("An install is already running."),
                            core::ProgressLog::Severity::Warn);
        return;
    }
    setBusy(true);
    progress_->setValue(0);
    statusLabel_->setText(tr("Starting..."));

    auto backend = createBackend();

    // The thread is intentionally created without a parent: this window
    // owns it explicitly and must be able to keep it alive (undeleted) if
    // the worker refuses to stop.
    shutdownFlag_->store(0);
    engineThread_ = new QThread();
    engine_ = new core::InstallEngine(std::move(backend), catalog_, logBackend_);
    engine_->moveToThread(engineThread_);

    // The lambda deliberately does not capture `this`: it may still run
    // while the window is being destroyed. The engine pointer stays valid
    // because the connection's context object is the engine itself.
    core::InstallEngine* enginePtr = engine_.data();
    auto shutdown = shutdownFlag_;
    connect(engineThread_, &QThread::started, enginePtr,
            [enginePtr, shutdown, p = std::move(plan)]() mutable {
                // The window may be tearing down while the worker starts.
                if (shutdown->load() != 0) return;
                enginePtr->run(std::move(p));
            });
    connect(engine_, &core::InstallEngine::stageChanged,
            this, &MainWindow::onEngineStageChanged);
    connect(engine_, &core::InstallEngine::progressChanged,
            this, &MainWindow::onEngineProgressChanged);
    connect(engine_, &core::InstallEngine::finished,
            this, &MainWindow::onEngineFinished);
    // The engine lives in the worker thread and is destroyed there once the
    // thread's event loop has stopped (deferred deletion is still processed
    // after the loop ends).
    connect(engineThread_, &QThread::finished, engine_.data(), &QObject::deleteLater);

    engineThread_->start();
}

void MainWindow::stopEngineThread() {
    if (!engineThread_) return;

    shutdownFlag_->store(1);
    if (engine_) engine_->requestCancel();
    engineThread_->quit();

    if (!engineThread_->wait(kEngineStopTimeoutMs)) {
        // The worker is stuck in an operation that cannot be interrupted.
        // Destroying a running QThread aborts the process, so the thread is
        // deliberately leaked instead. The worker still reads the catalog
        // and writes the log, so those are detached and leaked as well:
        // the process is on its way out and the worker must not touch
        // freed memory while it finishes.
        logBackend_->append(
            tr("Install worker did not stop within %1 ms; detaching it.")
                .arg(kEngineStopTimeoutMs),
            core::ProgressLog::Severity::Warn);
        releaseWorkerOwnedData();
        workerDetached_ = true;
        engineThread_ = nullptr;  // deliberately leaked — never deleted
        engine_ = nullptr;
        return;
    }

    // The worker stopped; the engine object was already deleted in the
    // worker thread. Deleting the thread object is safe now.
    delete engineThread_;
    engineThread_ = nullptr;
    engine_ = nullptr;
}

void MainWindow::releaseWorkerOwnedData() {
    // Hand ownership of the objects the detached worker may still use over
    // to the heap, and never free them: they have to stay valid for as long
    // as the worker runs. The raw pointers stay valid for this window too.
    if (catalogOwner_) {
        (void)catalogOwner_.release();
    }
    if (logBackendOwner_) {
        (void)logBackendOwner_.release();
    }
}

void MainWindow::onEngineStageChanged(QString stage) {
    statusLabel_->setText(stage);
    logBackend_->append(stage);
}

void MainWindow::onEngineProgressChanged(int percent) {
    progress_->setValue(percent);
}

void MainWindow::onEngineFinished(bool success, QString message, bool autoRestart) {
    // The worker has produced its result: stop the thread before anything
    // else so the window can be closed without a live worker behind it.
    stopEngineThread();

    setBusy(false);
    if (success) {
        statusLabel_->setText(tr("Done — ready to restart"));
        // Only show restart countdown if the plan requested auto-restart
        if (autoRestart) {
            showRestartCountdown();
        } else {
            showInstallResult(true, message);
        }
    } else {
        statusLabel_->setText(tr("Failed: %1").arg(message));
        showInstallResult(false, message);
    }
}

void MainWindow::showRestartCountdown() {
    restartDialog_ = new RestartCountdownDialog(this);
    connect(restartDialog_, &RestartCountdownDialog::countdownFinished,
            this, &MainWindow::onRestartCountdownFinished);
    connect(restartDialog_, &RestartCountdownDialog::countdownCancelled,
            this, &MainWindow::onRestartCountdownCancelled);
    restartDialog_->startCountdown(30);
    restartDialog_->exec();
}

void MainWindow::onRestartCountdownFinished() {
    // User didn't cancel - proceed with restart
    logBackend_->append("Restart countdown finished. Restarting system...",
                       core::ProgressLog::Severity::Warn);
    auto backend = createBackend();
    backend->restartSystem();
    qApp->quit();
}

void MainWindow::onRestartCountdownCancelled() {
    logBackend_->append("Restart cancelled by user.",
                       core::ProgressLog::Severity::Warn);
    QMessageBox::information(this, tr("ULLI"),
        tr("Restart cancelled. You can restart manually later."));
}

void MainWindow::onExitClicked() {
    if (engineThread_ && engineThread_->isRunning()) {
        if (engine_) engine_->requestCancel();
        const auto reply = QMessageBox::question(this, tr("ULLI"),
            tr("Cancel the install in progress and exit?"),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
    }
    qApp->quit();
}

}  // namespace ulli::ui
