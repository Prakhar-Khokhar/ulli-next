// platform/linux/DiskOps.cpp — Phase 1 stub. Phase 2 implements parted /
// mkfs.* / mount / grub-install / efibootmgr wrappers.
#include "platform/linux/DiskOps.h"

#include "platform/Platform.h"

namespace ulli::platform::linux {

std::vector<core::Disk> DiskOps::enumerateDisks() { return {}; }

core::Result<void> DiskOps::preflight(const core::InstallPlan&) {
    if (!platform::isElevated()) return core::makeNotElevated();
    return core::makeOk();
}

core::Result<void> DiskOps::validatePlanForDisk(const core::InstallPlan&) {
    return core::makeError(core::Error::Kind::Internal,
        "validatePlanForDisk: not implemented (Phase 2)");
}

core::Result<std::filesystem::path> DiskOps::resolveIso(const core::Distro&) {
    return core::makeError<std::filesystem::path>(core::Error::Kind::Internal,
        "resolveIso: not implemented (Phase 2)");
}

core::Result<std::filesystem::path> DiskOps::downloadIso(
    const core::Distro&, const std::filesystem::path&,
    std::function<void(int, const QString&)>) {
    return core::makeError<std::filesystem::path>(core::Error::Kind::Internal,
        "downloadIso: not implemented (Phase 2)");
}

core::Result<std::uint64_t> DiskOps::shrinkPartition(char, std::uint64_t) {
    return core::makeError<std::uint64_t>(core::Error::Kind::Internal,
        "shrinkPartition: not implemented (Phase 2)");
}

core::Result<void> DiskOps::wipeDisk(int) {
    return core::makeError(core::Error::Kind::Internal,
        "wipeDisk: not implemented (Phase 2)");
}

core::Result<void> DiskOps::createLayout(const core::InstallPlan&,
                                          std::filesystem::path&,
                                          std::filesystem::path&,
                                          std::function<bool()>) {
    return core::makeError(core::Error::Kind::Internal,
        "createLayout: not implemented (Phase 2)");
}

core::Result<std::filesystem::path> DiskOps::mountIso(const std::filesystem::path&) {
    return core::makeError<std::filesystem::path>(core::Error::Kind::Internal,
        "mountIso: not implemented (Phase 2)");
}

void DiskOps::unmountIso(const std::filesystem::path&) {}

core::Result<void> DiskOps::copyFiles(const std::filesystem::path&,
                                        const std::filesystem::path&,
                                        const core::Distro*,
                                        std::function<bool()>) {
    return core::makeError(core::Error::Kind::Internal,
        "copyFiles: not implemented (Phase 2)");
}

core::Result<void> DiskOps::patchDistroBootConfig(const core::Distro&,
                                                   const core::InstallPlan&) {
    return core::makeError(core::Error::Kind::Internal,
        "patchDistroBootConfig: not implemented (Phase 2)");
}

core::Result<void> DiskOps::installRefind(const core::InstallPlan&) {
    return core::makeError(core::Error::Kind::Internal,
        "installRefind: not implemented (Phase 2)");
}

core::Result<void> DiskOps::createBootEntry(const core::InstallPlan&) {
    return core::makeError(core::Error::Kind::Internal,
        "createBootEntry: not implemented (Phase 2)");
}

void DiskOps::rollbackBootEntry(const core::InstallPlan&) {}

void DiskOps::restartSystem() {}

}  // namespace ulli::platform::linux
