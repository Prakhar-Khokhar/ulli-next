// ui/MainWindow.cpp
#include "ui/MainWindow.h"
#include "ui/DistroSelector.h"
#include "ui/LogView.h"
#include "ui/PlanDialog.h"
#include "ui/RestartCountdownDialog.h"

#include "core/Sha256.h"
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
    connect(distro_, &DistroSelector::selectionChanged,
            this, &MainWindow::onSelectionChanged);

    // The install may only start once an ISO is present and verified.
    isoReady_ = distro_->isoReady();
    updateStartButtonState();
}

MainWindow::~MainWindow() {
    // Stop a running ISO transfer: the worker checks the flag while it
    // streams and removes its partial file.
    if (downloadCancelFlag_) downloadCancelFlag_->store(true);
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

std::filesystem::path MainWindow::isoCachePathFor(const core::Distro& distro) const {
    return ulli::platform::isoCachePath(distro);
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

QString MainWindow::workflowStateText() const {
    switch (workflowState_) {
        case WorkflowState::Idle: return tr("Ready");
        case WorkflowState::Scanning: return tr("Scanning disks…");
        case WorkflowState::AwaitingApproval: return tr("Waiting for plan approval");
        case WorkflowState::Downloading: return tr("Downloading ISO…");
        case WorkflowState::Verifying: return tr("Verifying ISO…");
        case WorkflowState::Ready: return tr("Ready to install");
        case WorkflowState::Installing: return tr("Installing…");
        case WorkflowState::Failed: return tr("Failed");
        case WorkflowState::Cancelled: return tr("Cancelled");
    }
    return QString();
}

void MainWindow::setBusy(bool busy) {
    busy_ = busy;
    exitBtn_->setEnabled(!busy);
    distro_->setEnabled(!busy && !scanning_);
    updateStartButtonState();
}

void MainWindow::setWorkflowState(WorkflowState state) {
    workflowState_ = state;
    statusLabel_->setText(workflowStateText());
    updateStartButtonState();
}

void MainWindow::updateStartButtonState() {
    // The start button only needs a selection. The ISO may still be
    // missing: it is fetched after the plan has been approved.
    const bool hasSelection = distro_->isCustom()
        ? !distro_->customIsoPath().isEmpty()
        : !distro_->selectedKey().isEmpty();
    const bool busyStage = workflowState_ == WorkflowState::Downloading ||
                           workflowState_ == WorkflowState::Verifying ||
                           workflowState_ == WorkflowState::Installing;
    const bool canStart = hasSelection && !busy_ && !scanning_ && !busyStage;
    startBtn_->setEnabled(canStart);
    if (!canStart && !hasSelection) {
        startBtn_->setToolTip(tr("Pick a distribution first."));
    } else {
        startBtn_->setToolTip(QString());
    }
}

void MainWindow::onIsoReadinessChanged(bool ready) {
    isoReady_ = ready;
    updateStartButtonState();
}

void MainWindow::onSelectionChanged() {
    // A changed selection invalidates an approved plan: an approval is
    // bound to the plan that was shown, never to a later selection.
    ++workflowGeneration_;
    if (approvedPlan_ || downloadCancelFlag_) {
        invalidateApproval(tr("The distribution or the installation mode changed."));
    }
    updateStartButtonState();
}

void MainWindow::invalidateApproval(const QString& reason) {
    if (downloadCancelFlag_) {
        downloadCancelFlag_->store(true);
        downloadCancelFlag_.reset();
    }
    const bool hadPlan = approvedPlan_.has_value();
    approvedPlan_.reset();
    if (hadPlan || workflowState_ != WorkflowState::Idle) {
        logBackend_->append(tr("The approved plan was invalidated: %1").arg(reason),
                            core::ProgressLog::Severity::Warn);
        statusLabel_->setText(reason);
    }
    if (workflowState_ != WorkflowState::Installing) {
        setWorkflowState(WorkflowState::Idle);
    }
}

void MainWindow::onStartClicked() {
    // 1. A distribution (or a custom ISO) must be selected. The ISO does
    //    NOT have to be present yet: the plan is reviewed first and the
    //    ISO is fetched afterwards.
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

    // 2. Enumerate the disks. This blocks on the platform backend (a
    //    PowerShell round trip on Windows), so it must not run on the GUI
    //    thread — otherwise the window freezes before the planning screen
    //    can be shown.
    beginDiskScan();
}

void MainWindow::beginDiskScan() {
    scanning_ = true;
    setWorkflowState(WorkflowState::Scanning);
    distro_->setEnabled(false);
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
    distro_->setEnabled(!busy_);
    setWorkflowState(WorkflowState::AwaitingApproval);

    if (!error.empty()) {
        logBackend_->append(
            tr("Disk scan failed: %1").arg(QString::fromStdString(error)),
            core::ProgressLog::Severity::Error);
        setWorkflowState(WorkflowState::Failed);
        QMessageBox::warning(this, tr("ULLI"),
            tr("Could not read the disk list:\n\n%1")
                .arg(QString::fromStdString(error)));
        return;
    }

    if (disks.empty()) {
        setWorkflowState(WorkflowState::Failed);
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

    // 3. Planning screen: the user reviews the plan and approves it.
    core::InstallPlan plan;
    if (!showPlanDialog(disks, distroKey, isoPath, plan)) {
        setWorkflowState(WorkflowState::Idle);
        return;
    }
    if (!plan.valid()) {
        setWorkflowState(WorkflowState::Idle);
        QMessageBox::warning(this, tr("ULLI"),
            tr("The install plan is invalid (missing distro or ISO)."));
        return;
    }

    // 4. Pre-flight: elevation.
    if (!hasInstallPrivileges()) {
        setWorkflowState(WorkflowState::Idle);
        QMessageBox::warning(this, tr("ULLI"),
            tr("ULLI must be run as Administrator / root."));
        return;
    }

    // 5. The plan was approved.
    onPlanApproved(std::move(plan));
}

bool MainWindow::showPlanDialog(const std::vector<core::Disk>& disks,
                                 const std::string& distroKey,
                                 const std::filesystem::path& isoPath,
                                 core::InstallPlan& outPlan) {
    PlanDialog dlg(this);
    dlg.setDisks(disks);
    dlg.setIsoContext(isoContextText());
    if (dlg.exec() != QDialog::Accepted) return false;
    outPlan = dlg.buildPlan(distroKey, isoPath);
    return true;
}

QString MainWindow::isoContextText() const {
    if (distro_->isCustom()) {
        return tr("ISO: custom file %1 — it is checked again before the "
                  "installation starts.").arg(distro_->customIsoPath());
    }
    if (distro_->isoReady() && distro_->isoVerified()) {
        return tr("ISO: %1 — verified and cached, it will be reused.")
            .arg(distro_->isoStatusText());
    }
    return tr("ISO: not present yet — it will be downloaded and verified "
              "after the plan is approved.");
}

bool MainWindow::approvalMatchesSelection(const core::InstallPlan& plan) const {
    if (plan.distroKey == core::kCustomDistroKey) {
        return distro_->isCustom() &&
               plan.isoPath == std::filesystem::path(distro_->customIsoPath().toStdString());
    }
    return !distro_->isCustom() && plan.distroKey == distro_->selectedKey().toStdString();
}

void MainWindow::onPlanApproved(core::InstallPlan plan) {
    approvedPlan_ = plan;
    setWorkflowState(WorkflowState::AwaitingApproval);

    // Revalidate the prerequisites before anything is fetched or written.
    if (!plan.valid()) {
        invalidateApproval(tr("The approved plan is not valid."));
        QMessageBox::warning(this, tr("ULLI"),
            tr("The install plan is invalid (missing distro or ISO)."));
        return;
    }
    if (!hasInstallPrivileges()) {
        invalidateApproval(tr("Administrator rights are required."));
        QMessageBox::warning(this, tr("ULLI"),
            tr("ULLI must be run as Administrator / root."));
        return;
    }
    if (!approvalMatchesSelection(plan)) {
        invalidateApproval(tr("The selection changed while the plan was reviewed."));
        QMessageBox::warning(this, tr("ULLI"),
            tr("The distribution changed while the plan was reviewed. "
               "Please review the plan again."));
        return;
    }

    ensureIsoAvailable(plan);
}

void MainWindow::ensureIsoAvailable(const core::InstallPlan& plan) {
    if (plan.distroKey == core::kCustomDistroKey) {
        // A custom ISO is already on disk. It is checked here and again by
        // the engine before anything is written.
        std::error_code ec;
        if (!std::filesystem::is_regular_file(plan.isoPath, ec) || ec) {
            setWorkflowState(WorkflowState::Failed);
            QMessageBox::warning(this, tr("ULLI"),
                tr("The custom ISO file is not available:\n%1")
                    .arg(QString::fromStdString(plan.isoPath.string())));
            return;
        }
        distro_->notifyIsoReady(false);
        if (approvedPlan_) approvedPlan_->isoPath = plan.isoPath;
        setWorkflowState(WorkflowState::Ready);
        startApprovedPlan();
        return;
    }

    const core::Distro* distro = catalog_->find(plan.distroKey);
    if (!distro) {
        setWorkflowState(WorkflowState::Failed);
        QMessageBox::warning(this, tr("ULLI"),
            tr("The distribution %1 is not in the catalog any more.")
                .arg(QString::fromStdString(plan.distroKey)));
        return;
    }

    const std::filesystem::path cached = isoCachePathFor(*distro);
    std::error_code ec;
    if (std::filesystem::exists(cached, ec) && !ec) {
        // A cached ISO is never reused because it was verified earlier: the
        // file can be replaced or modified after that verification, and a
        // matching file size proves nothing about the content. The cached
        // file is therefore checked against the digest from the catalog
        // again before it is used.
        verifyCachedIso(*distro, cached);
        return;
    }

    // The ISO is not cached: fetch it now - after the plan was approved.
    beginIsoDownload(*distro, cached);
}

void MainWindow::verifyCachedIso(const core::Distro& distro,
                                  const std::filesystem::path& path) {
    setWorkflowState(WorkflowState::Verifying);
    distro_->setIsoStage(tr("Verifying the cached ISO…"), 0, true);
    const quint64 generation = workflowGeneration_;
    cachedIsoPath_ = path;

    // Capture everything by value: the worker must not touch the catalog
    // after the window has been destroyed.
    const std::string expected = distro.sha256();
    const bool hasChecksum = distro.hasChecksum();
    QPointer<MainWindow> guard(this);
    QtConcurrent::run([guard, generation, path, expected, hasChecksum]() {
        bool ok = true;
        if (hasChecksum) {
            auto r = core::Sha256::verifyFile(path, expected);
            ok = static_cast<bool>(r);
        }
        // The file may only be claimed to be verified when it was checked
        // against a trusted digest. Without a digest nothing was verified.
        const bool verified = ok && hasChecksum;
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(),
            [guard, generation, ok, verified]() {
                if (!guard) return;
                guard->onIsoVerificationFinished(ok, verified, generation);
            }, Qt::QueuedConnection);
    });
}

void MainWindow::onIsoVerificationFinished(bool ok, bool verified,
                                            quint64 generation) {
    if (generation != workflowGeneration_) {
        // A stale verification result (the selection changed meanwhile).
        return;
    }
    if (workflowState_ != WorkflowState::Verifying) {
        return;
    }
    if (!ok) {
        approvedPlan_.reset();
        setWorkflowState(WorkflowState::Failed);
        // The cached file did not pass the digest check: it must not be
        // reported as ready - or as verified - any more.
        distro_->notifyIsoInvalid();
        const QString isoPath = QString::fromStdString(cachedIsoPath_.string());
        distro_->setIsoStage(
            tr("The cached ISO does not match the expected checksum: %1")
                .arg(isoPath), 0, false);
        QMessageBox::warning(this, tr("ULLI"),
            tr("The cached ISO does not match the checksum from the catalog:\n\n"
               "%1\n\n"
               "The installation was stopped and the cached file was not "
               "used. Remove the file and start the installation again to "
               "download a fresh copy.").arg(isoPath));
        return;
    }
    if (!approvedPlan_) return;
    const QString isoPath = QString::fromStdString(cachedIsoPath_.string());
    if (verified) {
        distro_->setIsoStage(tr("✓ ISO verified: %1").arg(isoPath), 0, false);
    } else {
        // The catalog has no digest for this ISO: the file is usable, but
        // it must not be claimed to be cryptographically verified.
        logBackend_->append(
            tr("The cached ISO could not be checksum verified: the catalog "
               "has no trusted digest for it."),
            core::ProgressLog::Severity::Warn);
        distro_->setIsoStage(
            tr("⚠ ISO present but not verified — the catalog has no checksum "
               "for it: %1").arg(isoPath), 0, false);
    }
    approvedPlan_->isoPath = cachedIsoPath_;
    distro_->notifyIsoReady(verified);
    setWorkflowState(WorkflowState::Ready);
    startApprovedPlan();
}

void MainWindow::beginIsoDownload(const core::Distro& distro,
                                  const std::filesystem::path& destPath) {
    setWorkflowState(WorkflowState::Downloading);
    distro_->setIsoStage(tr("Downloading %1…").arg(distro.qlabel()), 0, true);
    const quint64 generation = workflowGeneration_;

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    downloadCancelFlag_ = cancel;

    QPointer<MainWindow> guard(this);
    QtConcurrent::run([guard, distro, destPath, cancel, generation]() {
        std::unique_ptr<core::IPlatformBackend> backend;
        if (guard) backend = guard->createBackend();
        if (!backend) return;

        auto result = backend->downloadIso(
            distro, destPath,
            [guard, generation](int percent, const QString& status) {
                if (!guard) return;
                QMetaObject::invokeMethod(guard.data(),
                    [guard, generation, percent, status]() {
                        if (!guard) return;
                        if (generation != guard->workflowGeneration_) return;
                        guard->onIsoDownloadProgress(percent, status);
                    }, Qt::QueuedConnection);
            },
            cancel.get());

        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(),
            [guard, generation, result]() {
                if (!guard) return;
                guard->onIsoDownloadFinished(result, generation);
            }, Qt::QueuedConnection);
    });
}

void MainWindow::onIsoDownloadProgress(int percent, const QString& status) {
    if (workflowState_ != WorkflowState::Downloading) return;
    progress_->setValue(percent);
    distro_->setIsoStage(status, percent, true);
}

void MainWindow::onIsoDownloadFinished(core::Result<core::IsoDownload> result,
                                       quint64 generation) {
    downloadCancelFlag_.reset();
    if (generation != workflowGeneration_) {
        // The selection changed while the ISO was downloading: the result
        // must not change the state of the new selection.
        return;
    }
    if (workflowState_ != WorkflowState::Downloading) {
        return;
    }
    if (!result) {
        // The approval is void after a failed transfer. The (partial) file
        // was removed by the backend, so nothing is left behind.
        approvedPlan_.reset();
        const bool cancelled = result.error().kind() == core::Error::Kind::Cancelled;
        setWorkflowState(cancelled ? WorkflowState::Cancelled : WorkflowState::Failed);
        distro_->setIsoStage(
            cancelled ? tr("The ISO download was cancelled.")
                      : tr("The ISO download failed: %1").arg(result.error().qmessage()),
            0, false);
        logBackend_->append(tr("ISO download failed: %1").arg(result.error().qmessage()),
                            core::ProgressLog::Severity::Error);
        QMessageBox::critical(this, tr("ULLI"),
            tr("The ISO could not be downloaded:\n\n%1").arg(result.error().qmessage()));
        return;
    }

    const auto& download = result.value();
    if (!download.verified) {
        logBackend_->append(
            tr("The downloaded ISO could not be checksum verified: the catalog "
               "has no trusted digest for it."),
            core::ProgressLog::Severity::Warn);
    }
    if (approvedPlan_) approvedPlan_->isoPath = download.path;
    distro_->notifyIsoReady(download.verified);
    setWorkflowState(WorkflowState::Ready);
    startApprovedPlan();
}

void MainWindow::startApprovedPlan() {
    if (!approvedPlan_) return;
    core::InstallPlan plan = std::move(*approvedPlan_);
    approvedPlan_.reset();
    // The engine re-enumerates the disk and re-validates the plan - which
    // includes the identity of the target disk - before it modifies
    // anything.
    setWorkflowState(WorkflowState::Installing);
    startEngine(std::move(plan));
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
    setWorkflowState(success ? WorkflowState::Idle : WorkflowState::Failed);
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
    if (workflowState_ == WorkflowState::Downloading ||
        workflowState_ == WorkflowState::Verifying) {
        const auto reply = QMessageBox::question(this, tr("ULLI"),
            tr("Cancel the running ISO transfer and exit?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
        if (downloadCancelFlag_) downloadCancelFlag_->store(true);
    }
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
