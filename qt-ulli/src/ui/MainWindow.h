// ui/MainWindow.h
//
// ULLI main window. Hosts the distro selector, status, progress bar,
// log view, and Start / Exit buttons.
//
// Workflow (one explicit state machine, see WorkflowState):
//
//   Idle ──▶ Scanning ──▶ AwaitingApproval ──▶ Downloading ──▶ Verifying
//                ▲                │                   │            │
//                │                │                   ▼            ▼
//                └──── selection changed ───────────  Ready ──▶ Installing
//
// The plan is reviewed and approved *before* an ISO is fetched: the ISO
// does not have to be present to reach the planning screen. Approving the
// plan starts the download (if needed), the verification and finally the
// installation.
//
// Threading contract:
//   * Disk enumeration and ISO transfers run on worker threads so the GUI
//     never blocks on the platform backend. Results are marshalled back to
//     the GUI thread through queued invocations.
//   * The install itself runs on a dedicated QThread owned by this window.
//     The pointer to that thread is kept for as long as the thread may
//     still be running; it is never discarded while the thread is alive
//     (destroying a running QThread aborts the process).

#pragma once

#include "core/Catalog.h"
#include "core/InstallEngine.h"
#include "core/InstallPlan.h"
#include "core/ProgressLog.h"

#include <QMainWindow>
#include <QPointer>
#include <QThread>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
    enum class WorkflowState {
        Idle,              // nothing to do yet
        Scanning,          // enumerating the disks
        AwaitingApproval,  // the plan is shown for review
        Downloading,       // the ISO is being transferred
        Verifying,         // the ISO is being checksum verified
        Ready,             // everything is in place, the engine may run
        Installing,        // the install engine is running
        Failed,            // the last step failed
        Cancelled,         // the user cancelled
    };
    Q_ENUM(WorkflowState)

    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // The current state of the install workflow.
    WorkflowState workflowState() const { return workflowState_; }
    QString workflowStateText() const;

protected:
    // ─── Extension / test seams ──────────────────────────────────────────
    // The platform backend is created through this hook so a test can
    // inject a mock instead of touching real disks.
    virtual std::unique_ptr<core::IPlatformBackend> createBackend();

    // Shows the planning screen and fills `outPlan`. Returns false when
    // the user cancels. Overridable so tests can run the flow without a
    // modal dialog.
    virtual bool showPlanDialog(const std::vector<core::Disk>& disks,
                                const std::string& distroKey,
                                const std::filesystem::path& isoPath,
                                core::InstallPlan& outPlan);

    // True when the selected ISO is present (and verified when the catalog
    // provides a digest).
    virtual bool isoReadyForInstall() const;

    // True when the process has the privileges an install needs.
    virtual bool hasInstallPrivileges() const;

    // Where the ISO of a distro is cached. Overridable so a test can use
    // its own directory instead of the user cache.
    virtual std::filesystem::path isoCachePathFor(const core::Distro& distro) const;

    // Presents the outcome of the install to the user. Overridable so
    // tests do not block on a modal message box.
    virtual void showInstallResult(bool success, const QString& message);

    // Hands a validated plan to the install worker thread.
    void startEngine(core::InstallPlan plan);

private slots:
    void onStartClicked();
    void onExitClicked();
    void onEngineStageChanged(QString stage);
    void onEngineProgressChanged(int percent);
    void onEngineFinished(bool success, QString message, bool autoRestart);
    void onRestartCountdownFinished();
    void onRestartCountdownCancelled();
    void onIsoReadinessChanged(bool ready);
    void onSelectionChanged();

private:
    void setBusy(bool busy);
    void updateStartButtonState();
    void setWorkflowState(WorkflowState state);
    void beginDiskScan();
    void onDisksEnumerated(std::vector<core::Disk> disks, const std::string& error);
    void onPlanApproved(core::InstallPlan plan);
    void ensureIsoAvailable(const core::InstallPlan& plan);
    void beginIsoDownload(const core::Distro& distro, const std::filesystem::path& destPath);
    void onIsoDownloadProgress(int percent, const QString& status);
    void onIsoDownloadFinished(core::Result<core::IsoDownload> result, quint64 generation);
    void verifyCachedIso(const core::Distro& distro, const std::filesystem::path& path);
    void onIsoVerificationFinished(bool ok, bool verified, quint64 generation);
    void startApprovedPlan();
    void invalidateApproval(const QString& reason);
    bool approvalMatchesSelection(const core::InstallPlan& plan) const;
    QString isoContextText() const;
    void stopEngineThread();
    void releaseWorkerOwnedData();
    void showRestartCountdown();

    // The catalog and the log are handed to the install worker as raw
    // pointers. When the worker cannot be stopped in time they are
    // intentionally leaked (see releaseWorkerOwnedData) so the worker
    // never touches freed memory while it finishes.
    std::unique_ptr<core::Catalog> catalogOwner_;
    core::Catalog* catalog_ = nullptr;
    bool fallbackUsed_ = false;

    DistroSelector* distro_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QProgressBar* progress_ = nullptr;
    LogView* log_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QPushButton* exitBtn_ = nullptr;

    std::unique_ptr<core::ProgressLog> logBackendOwner_;
    core::ProgressLog* logBackend_ = nullptr;

    // Owned by this window. Never deleted while the thread is running.
    QThread* engineThread_ = nullptr;
    QPointer<core::InstallEngine> engine_;

    // Set when the window starts tearing down so a worker that has not
    // started yet does not begin an install during shutdown. Shared with
    // the worker lambda so it stays valid even if the window goes away.
    std::shared_ptr<std::atomic<int>> shutdownFlag_ =
        std::make_shared<std::atomic<int>>(0);

    // The plan the user approved. Only valid between the approval and the
    // start of the engine.
    std::optional<core::InstallPlan> approvedPlan_;

    // The cached ISO that is currently being verified.
    std::filesystem::path cachedIsoPath_;

    // Bumped whenever the selection changes. Every asynchronous callback
    // carries the generation it was started with and is ignored when the
    // generation moved on.
    quint64 workflowGeneration_ = 0;

    // Cancellation flag of the running ISO transfer (shared with the
    // worker so it stays valid when the window is destroyed).
    std::shared_ptr<std::atomic<bool>> downloadCancelFlag_;

    WorkflowState workflowState_ = WorkflowState::Idle;
    bool busy_ = false;
    bool scanning_ = false;
    bool isoReady_ = false;
    // True when a worker could not be stopped and was detached. A new
    // install must not start while that worker may still touch the disks.
    bool workerDetached_ = false;

    RestartCountdownDialog* restartDialog_ = nullptr;
};

}  // namespace ulli::ui
