// ui/MainWindow.cpp
#include "ui/MainWindow.h"
#include "ui/DistroSelector.h"
#include "ui/LogView.h"
#include "ui/PlanDialog.h"

#include "platform/Platform.h"

#include <QApplication>
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

namespace ulli::ui {

namespace {

QString makeLogPath() {
    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    return QString::fromStdString(
        (std::filesystem::path(ulli::platform::cacheDir()) /
         ("ulli-install-" + stamp.toStdString() + ".log")).string());
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

    logBackend_.openFile(makeLogPath());
    log_->bindLog(&logBackend_);

    // ─── Load catalog ────────────────────────────────────────────────────
    catalog_ = core::Catalog::load(&fallbackUsed_);
    distro_->setCatalog(&catalog_);
    if (fallbackUsed_) {
        logBackend_.append(
            tr("distros.json not found next to the executable. Using built-in "
               "catalog — some mirrors may be dead and newer distros are missing."),
            core::ProgressLog::Severity::Warn);
        statusLabel_->setText(tr("Warning: distros.json missing — using built-in catalog"));
    }

    // ─── Wire up ─────────────────────────────────────────────────────────
    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(exitBtn_,  &QPushButton::clicked, this, &MainWindow::onExitClicked);
}

MainWindow::~MainWindow() {
    if (engineThread_) {
        engineThread_->quit();
        engineThread_->wait(3000);
    }
}

void MainWindow::setBusy(bool busy) {
    startBtn_->setEnabled(!busy);
    exitBtn_->setEnabled(!busy);
    distro_->setEnabled(!busy);
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

    // 2. Enumerate disks via the platform backend.
    std::unique_ptr<core::IPlatformBackend> backend;
#if defined(Q_OS_WIN)
    backend = std::make_unique<platform::windows::DiskOps>();
#else
    backend = std::make_unique<platform::linux::DiskOps>();
#endif
    const auto disks = backend->enumerateDisks();
    if (disks.empty()) {
        QMessageBox::warning(this, tr("ULLI"),
            tr("No disks were detected. Run ULLI as Administrator / root."));
        return;
    }

    // 3. Plan dialog.
    PlanDialog dlg(this);
    dlg.setDisks(disks);
    if (dlg.exec() != QDialog::Accepted) return;

    const std::string distroKey = distro_->isCustom()
        ? "custom" : distro_->selectedKey().toStdString();
    const std::filesystem::path isoPath = distro_->isCustom()
        ? std::filesystem::path(distro_->customIsoPath().toStdString())
        : std::filesystem::path();

    core::InstallPlan plan = dlg.buildPlan(distroKey, isoPath);
    if (!plan.valid()) {
        QMessageBox::warning(this, tr("ULLI"),
            tr("The install plan is invalid (missing distro or ISO)."));
        return;
    }

    // 4. Pre-flight: elevation.
    if (!ulli::platform::isElevated()) {
        QMessageBox::warning(this, tr("ULLI"),
            tr("ULLI must be run as Administrator / root."));
        return;
    }

    // 5. Hand off to engine.
    startEngine(std::move(plan));
}

void MainWindow::startEngine(core::InstallPlan plan) {
    setBusy(true);
    progress_->setValue(0);
    statusLabel_->setText(tr("Starting..."));

    std::unique_ptr<core::IPlatformBackend> backend;
#if defined(Q_OS_WIN)
    backend = std::make_unique<platform::windows::DiskOps>();
#else
    backend = std::make_unique<platform::linux::DiskOps>();
#endif

    engineThread_ = new QThread(this);
    engine_ = new core::InstallEngine(std::move(backend), &catalog_, &logBackend_);
    engine_->moveToThread(engineThread_);

    connect(engineThread_, &QThread::started, engine_.data(), [this, p = std::move(plan)]() mutable {
        engine_->run(std::move(p));
    });
    connect(engine_, &core::InstallEngine::stageChanged,
            this, &MainWindow::onEngineStageChanged);
    connect(engine_, &core::InstallEngine::progressChanged,
            this, &MainWindow::onEngineProgressChanged);
    connect(engine_, &core::InstallEngine::finished,
            this, &MainWindow::onEngineFinished);
    connect(engineThread_, &QThread::finished, engine_.data(), &QObject::deleteLater);
    connect(engineThread_, &QThread::finished, engineThread_, &QObject::deleteLater);

    engineThread_->start();
}

void MainWindow::onEngineStageChanged(QString stage) {
    statusLabel_->setText(stage);
    logBackend_.append(stage);
}

void MainWindow::onEngineProgressChanged(int percent) {
    progress_->setValue(percent);
}

void MainWindow::onEngineFinished(bool success, QString message) {
    setBusy(false);
    if (success) {
        statusLabel_->setText(tr("Done — ready to restart"));
        QMessageBox::information(this, tr("ULLI"),
            tr("Installation completed.\n\n%1").arg(message));
    } else {
        statusLabel_->setText(tr("Failed: %1").arg(message));
        QMessageBox::critical(this, tr("ULLI"),
            tr("Installation failed:\n\n%1").arg(message));
    }
    engineThread_ = nullptr;
    engine_ = nullptr;
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
