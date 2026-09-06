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

#include <filesystem>
#include <vector>

namespace ulli::platform::windows {

class DiskOps final : public core::IPlatformBackend {
public:
    DiskOps();
    ~DiskOps() override;

    std::vector<core::Disk> enumerateDisks() override;

    core::Result<void> preflight(const core::InstallPlan& plan) override;
    core::Result<void> validatePlanForDisk(const core::InstallPlan& plan) override;
    core::Result<std::filesystem::path> resolveIso(const core::Distro& d) override;
    core::Result<std::uint64_t> shrinkPartition(char driveLetter,
                                                std::uint64_t newSizeBytes) override;
    core::Result<void> wipeDisk(int diskNumber) override;
    core::Result<void> createLayout(const core::InstallPlan& plan,
                                    std::filesystem::path& bootMount,
                                    std::filesystem::path& refindMount) override;
    core::Result<std::filesystem::path> mountIso(
        const std::filesystem::path& iso) override;
    void unmountIso(const std::filesystem::path& mount) override;
    core::Result<void> copyFiles(const std::filesystem::path& src,
                                 const std::filesystem::path& dst) override;
    core::Result<void> patchDistroBootConfig(const core::Distro& d,
                                             const core::InstallPlan& plan) override;
    core::Result<void> installRefind(const core::InstallPlan& plan) override;
    core::Result<void> createBootEntry(const core::InstallPlan& plan) override;
    void rollbackBootEntry(const core::InstallPlan& plan) override;
    void restartSystem() override;
};

}  // namespace ulli::platform::windows
