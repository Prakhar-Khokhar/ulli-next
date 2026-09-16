// core/InstallPlan.h
//
// Immutable description of one install run. The UI builds this and
// passes it to InstallEngine::run(). All values are validated by
// InstallEngine before any disk mutation starts; if validation fails
// the plan is rejected without side effects.

#pragma once

#include "core/Distro.h"

#include <QString>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ulli::core {

constexpr std::uint64_t kStagingSizeBytes = 7ull * 1024 * 1024 * 1024;
constexpr std::uint64_t kMinFullInstallShrinkBytes = 30ull * 1024 * 1024 * 1024;

enum class Strategy {
    ShrinkAll,        // shrink C: by LinuxSize + boot + rEFInd
    UseFreeAll,       // use existing unallocated, do not touch C:
    UseFreeBoot,      // shrink C: for Linux, use free for boot partition
    OtherDrive,       // use free on a non-C: disk
    OtherDriveShrink, // shrink a non-C: NTFS volume
    WipeDisk,         // clear the target disk first
};

enum class BootMode {
    Direct,    // boot directly from the new FAT32 partition's
               // \EFI\BOOT\BOOTx64.EFI (works on most UEFI firmwares)
    Refind,    // install rEFInd on a separate 100 MB FAT32 partition
};

enum class AllocationMode {
    LiveOnly,   // Only create ~7 GB FAT32 staging partition for live environment
    FullInstall // Shrink for staging (~7 GB) + unallocated Linux space (min 30 GB total)
};

struct InstallPlan {
    // Distro + ISO
    std::string distroKey;             // matches Distro::key()
    std::filesystem::path isoPath;     // resolved ISO file

    // Target disk
    int targetDiskNumber = 0;
    std::optional<char> shrinkDriveLetter; // only set for OtherDriveShrink
    std::uint64_t shrinkAmountBytes = 0;  // for ShrinkAll / UseFreeBoot / OtherDriveShrink

    // Layout
    std::uint64_t linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
    std::uint64_t bootSizeBytes = 7ull * 1024 * 1024 * 1024;
    std::uint64_t refindSizeBytes = 100ull * 1024 * 1024;

    // Strategy
    Strategy strategy = Strategy::ShrinkAll;
    BootMode bootMode = BootMode::Direct;
    AllocationMode allocationMode = AllocationMode::FullInstall;

    // After-success
    bool deleteIsoAfter = false;
    bool autoRestart = false;

    // Computed at run time (not set by UI).
    std::optional<std::filesystem::path> bootPartitionMount;  // FAT32
    std::optional<std::filesystem::path> refindPartitionMount;
    std::optional<std::filesystem::path> windowsEspMount;     // for rEFInd bootloader copy
    mutable std::optional<std::string> bcdGuid;                // rollback token
                                                          // (mutable so the
                                                          // platform backend
                                                          // can clear it
                                                          // after delete)
    std::uint64_t actualFreedBytes = 0;  // Actual bytes freed by shrink (may differ from linuxSizeBytes due to alignment)

    // Staging partition identity for re-verification before boot entry creation
    std::optional<int> stagingPartitionNumber;
    std::optional<std::uint64_t> stagingPartitionOffset;
    std::optional<std::uint64_t> stagingPartitionSize;
    std::optional<std::string> stagingPartitionGptGuid;

    QString summary() const;
    bool valid() const;
};

}  // namespace ulli::core
