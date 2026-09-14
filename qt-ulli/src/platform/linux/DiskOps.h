// platform/linux/DiskOps.h
//
// Linux IPlatformBackend. Phase 2 implements the real one. This Phase 1
// stub returns NotImplemented from every method so the build links
// cleanly on Linux dev hosts.

#pragma once

#include "core/DiskInfo.h"
#include "core/InstallEngine.h"
#include "core/Result.h"

#include <filesystem>
#include <vector>

namespace ulli::platform::linux {

class DiskOps final : public core::IPlatformBackend {
public:
    std::vector<core::Disk> enumerateDisks() override;
    core::Result<void> preflight(const core::InstallPlan& plan) override;
    core::Result<void> validatePlanForDisk(const core::InstallPlan& plan) override;
    core::Result<std::filesystem::path> resolveIso(const core::Distro& d) override;
    core::Result<std::filesystem::path> downloadIso(
        const core::Distro& distro,
        const std::filesystem::path& destPath,
        std::function<void(int percent, const QString& status)> progressCallback) override;
    core::Result<std::uint64_t> shrinkPartition(char driveLetter,
                                                std::uint64_t newSizeBytes) override;
    core::Result<void> wipeDisk(int diskNumber) override;
core::Result<void> createLayout(const core::InstallPlan& plan,
                                     std::filesystem::path& bootMount,
                                     std::filesystem::path& refindMount,
                                     std::function<bool()> cancelCallback = nullptr) override;
    core::Result<std::filesystem::path> mountIso(
        const std::filesystem::path& iso) override;
    void unmountIso(const std::filesystem::path& mount) override;
core::Result<void> copyFiles(const std::filesystem::path& src,
                                 const std::filesystem::path& dst,
                                 const core::Distro* distro,
                                 std::function<bool()> cancelCallback = nullptr) override;
    core::Result<void> patchDistroBootConfig(const core::Distro& d,
                                             const core::InstallPlan& plan) override;
    core::Result<void> installRefind(const core::InstallPlan& plan) override;
    core::Result<void> createBootEntry(const core::InstallPlan& plan) override;
    void rollbackBootEntry(const core::InstallPlan& plan) override;
    void restartSystem() override;
};

}  // namespace ulli::platform::linux
