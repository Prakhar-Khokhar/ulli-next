// platform/windows/DiskOps.h
//
// Concrete IPlatformBackend for Windows. Wraps:
//   * parted     — partition tables, partition creation, resize (new path)
//   * bcdedit    — UEFI boot entry management
//   * robocopy   — recursive file copy from mounted ISO to FAT32
//   * manage-bde / PowerShell  — BitLocker preflight
//
// Phase 1 implements parity with the PowerShell script. New features
// (resize ext4 / btrfs, advanced manual editor) come in Phase 2.

#pragma once

#include "core/DiskInfo.h"
#include "core/InstallEngine.h"
#include "core/Result.h"

#include <QString>

#include <atomic>
#include <filesystem>
#include <vector>

namespace ulli::platform::windows {

class DiskOps final : public core::IPlatformBackend {
public:
    DiskOps();
    ~DiskOps() override;

    std::vector<core::Disk> enumerateDisks(std::string* errorMessage = nullptr) override;

    core::Result<void> preflight(const core::InstallPlan& plan) override;
    core::Result<void> validatePlanForDisk(const core::InstallPlan& plan) override;
    core::Result<std::filesystem::path> resolveIso(const core::Distro& d) override;
    core::Result<core::IsoDownload> downloadIso(
        const core::Distro& distro,
        const std::filesystem::path& destPath,
        std::function<void(int percent, const QString& status)> progressCallback,
        const std::atomic<bool>* cancelFlag = nullptr) override;
    core::Result<std::uint64_t> shrinkPartition(int diskNumber,
                                                char driveLetter,
                                                int partitionNumber,
                                                std::uint64_t requestedShrinkBytes) override;

    // Builds the PowerShell probe script that validates a shrink plan and
    // reports the requested final size. Separated from execution so the
    // generated script can be inspected (and regression-tested) without
    // running PowerShell or touching any disk.
    static QString buildShrinkProbeScript(int diskNumber,
                                          char driveLetter,
                                          int partitionNumber,
                                          std::uint64_t requestedShrinkBytes);

    core::Result<void> wipeDisk(int diskNumber) override;
core::Result<void> createLayout(const core::InstallPlan& plan,
                                     std::filesystem::path& bootMount,
                                     std::filesystem::path& refindMount,
                                     std::function<bool()> cancelCallback = nullptr) override;
    core::Result<std::filesystem::path> mountIso(
        const std::filesystem::path& iso) override;
    void unmountIso(const std::filesystem::path& iso) override;
core::Result<void> copyFiles(const std::filesystem::path& src,
                                  const std::filesystem::path& dst,
                                  const core::Distro* distro,
                                  const core::InstallPlan& plan,
                                  std::function<bool()> cancelCallback = nullptr,
                                  core::CopyProgressCallback progress = nullptr) override;
    core::Result<void> patchDistroBootConfig(const core::Distro& d,
                                             const core::InstallPlan& plan) override;
    core::Result<void> installRefind(const core::InstallPlan& plan) override;
    core::Result<void> createBootEntry(const core::InstallPlan& plan) override;
    void rollbackBootEntry(const core::InstallPlan& plan) override;
    void rollbackStagingPartition(const core::InstallPlan& plan) override;
    void restartSystem() override;

private:
    // Run a PowerShell script from a temp file, return trimmed stdout.
    core::Result<QString> runPowerShellScript(const QString& script);

    // Check BitLocker lock state on a given partition via PowerShell.
    // Returns Ok(true) when locked/unsafe, Ok(false) when unlocked, or an
    // error when the query is indeterminate.
    core::Result<bool> isBitLockerLockedOnPartition(int diskNumber, int partitionNumber);

    // True when this installation attached the ISO image itself. An image
    // that was already attached (or that the user mounted) is left alone
    // when the installation ends.
    bool isoAttachedByUs_ = false;
};

}  // namespace ulli::platform::windows
