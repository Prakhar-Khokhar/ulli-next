// core/InstallEngine.h
//
// Top-level state machine for the install pipeline. Runs on a worker
// thread (use moveToThread). Emits progress signals and appends to a
// shared ProgressLog. The platform backend (platform::DiskOps etc.) is
// provided via the constructor so tests can inject mocks.
//
// Stages (mirror the PowerShell Start-Installation flow):
//   1. Pre-flight     (BitLocker, sudo, disk access)
//   2. Resolve ISO    (download + checksum if needed)
//   3. Resize/shrink  (only for strategies that touch an existing volume)
//   4. Wipe disk      (only for WipeDisk strategy)
//   5. Create layout  (partitions + filesystems)
//   6. Mount ISO + copy files
//   7. Distro-specific config patches (Fedora LABEL, CachyOS archisolabel)
//   8. Install rEFInd (if requested)
//   9. Copy bootloader into Windows ESP (only for WipeDisk)
//  10. Create UEFI boot entry
//  11. Cleanup (unmount, delete ISO if asked)
//  12. Restart countdown (if asked)

#pragma once

#include "core/DiskInfo.h"
#include "core/Distro.h"
#include "core/InstallPlan.h"
#include "core/ProgressLog.h"
#include "core/Result.h"

#include <QAtomicInt>
#include <QObject>

#include <functional>
#include <memory>

namespace ulli::core {

class Catalog;

// Abstract backend. The Windows and Linux ports provide concrete
// implementations. Tests provide a mock.
class IPlatformBackend {
public:
    virtual ~IPlatformBackend() = default;

    // Enumerate all physical disks visible to the installer.
    virtual std::vector<Disk> enumerateDisks() = 0;

    // Pre-flight: check BitLocker, sudo, etc. Returns NotElevated if
    // the user is not admin/root. Implementations should also call
    // validatePlanForDisk() internally to reject unsafe plans early.
    virtual Result<void> preflight(const InstallPlan& plan) = 0;

    // Validate a plan against the current disk state without
    // mutating anything. Used by the engine as a defense-in-depth
    // check before each mutation stage. Implementations should reject
    // plans that target a missing disk, a non-system disk for
    // WipeDisk, or other clearly-unsafe combinations.
    virtual Result<void> validatePlanForDisk(const InstallPlan& plan) = 0;

    // ISO download / verification. Returns the resolved ISO path.
    virtual Result<std::filesystem::path> resolveIso(const Distro& distro) = 0;

    // Download ISO from mirrors with progress reporting. Returns the
    // path to the downloaded file on success.
    virtual Result<std::filesystem::path> downloadIso(
        const Distro& distro,
        const std::filesystem::path& destPath,
        std::function<void(int percent, const QString& status)> progressCallback) = 0;

    // Resize a partition (shrink only). Returns the new size.
    virtual Result<std::uint64_t> shrinkPartition(char driveLetter,
                                                 std::uint64_t newSizeBytes) = 0;

    // Wipe a disk (Clear-Disk / mklabel). For WipeDisk strategy.
    virtual Result<void> wipeDisk(int diskNumber) = 0;

    // Create the install layout (boot partition + linux free space,
    // optionally rEFInd). Returns the mount paths.
    // The cancelCallback is invoked between major steps to check for
    // user cancellation. Return true to abort, false to continue.
    virtual Result<void> createLayout(const InstallPlan& plan,
                                      std::filesystem::path& bootMount,
                                      std::filesystem::path& refindMount,
                                      std::function<bool()> cancelCallback = nullptr) = 0;

    // Mount the ISO and return the mount path.
    virtual Result<std::filesystem::path> mountIso(
        const std::filesystem::path& iso) = 0;

    // Unmount the ISO. Best-effort; never returns an error to the
    // caller (caller cannot recover).
    virtual void unmountIso(const std::filesystem::path& mount) = 0;

    // Recursive copy from src to dst. Windows: robocopy /E. Linux: cp -a.
    // cancelCallback is invoked between files to check for user cancellation.
    // distro is provided for post-copy verification of required files.
    virtual Result<void> copyFiles(const std::filesystem::path& src,
                                   const std::filesystem::path& dst,
                                   const Distro* distro,
                                   std::function<bool()> cancelCallback = nullptr) = 0;

    // Distro-specific config patches (LABEL= etc.). On Windows the
    // patches happen on the FAT32 boot partition; on Linux they
    // happen on the mounted live partition.
    virtual Result<void> patchDistroBootConfig(const Distro& distro,
                                               const InstallPlan& plan) = 0;

    // Install rEFInd onto the dedicated FAT32 partition.
    virtual Result<void> installRefind(const InstallPlan& plan) = 0;

    // Create a UEFI boot entry. On success, plan.bcdGuid is set so the
    // engine can roll back on later failure.
    virtual Result<void> createBootEntry(const InstallPlan& plan) = 0;

    // Roll back a boot entry by GUID. Idempotent.
    virtual void rollbackBootEntry(const InstallPlan& plan) = 0;

    // Restart the system.
    virtual void restartSystem() = 0;

    // Rollback the staging partition created during createLayout.
    // Called when a later stage fails after partition creation.
    virtual void rollbackStagingPartition(const InstallPlan& plan) = 0;
};

class InstallEngine : public QObject {
    Q_OBJECT
public:
    InstallEngine(std::unique_ptr<IPlatformBackend> backend,
                  const Catalog* catalog,
                  ProgressLog* log,
                  QObject* parent = nullptr);
    ~InstallEngine() override;

    // Start the install. Returns immediately; listen to stageChanged,
    // progressChanged, finished().
    void run(InstallPlan plan);

    // Request cancellation. Idempotent and thread-safe.
    void requestCancel();

signals:
    void stageChanged(QString stage);
    void progressChanged(int percent);          // 0..100
    void finished(bool success, QString message, bool autoRestart);

private:
    Result<void> runStages(InstallPlan& plan);

    std::unique_ptr<IPlatformBackend> backend_;
    const Catalog* catalog_;
    ProgressLog* log_;
    QAtomicInt cancelFlag_ = 0;
};

}  // namespace ulli::core
