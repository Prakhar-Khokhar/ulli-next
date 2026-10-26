// core/InstallPlan.h
//
// Immutable description of one install run. The UI builds this and
// passes it to InstallEngine::run(). All values are validated by
// InstallEngine before any disk mutation starts; if validation fails
// the plan is rejected without side effects.

#pragma once

#include "core/Distro.h"
#include "core/DiskInfo.h"

#include <QString>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ulli::core {

constexpr std::uint64_t kStagingSizeBytes = 7ull * 1024 * 1024 * 1024;
constexpr std::uint64_t kMinFullInstallShrinkBytes = 30ull * 1024 * 1024 * 1024;

// Distro key used when the user points at an ISO that is not in the catalog.
// The catalog lookup legitimately fails for such a plan; the ISO path carried
// by the plan is used instead (see InstallEngine::runStages).
constexpr const char* kCustomDistroKey = "custom";

// Sentinel for "no target disk selected". Windows disk numbers are 0-based
// (Get-Disk / diskpart "select disk 0"), so 0 is a *valid* disk number and
// must not double as the "unset" marker.
constexpr int kUnsetDiskNumber = -1;

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
    int targetDiskNumber = kUnsetDiskNumber;  // kUnsetDiskNumber == "not chosen yet"
    std::optional<char> shrinkDriveLetter; // only set for OtherDriveShrink
    std::optional<int> shrinkPartitionNumber; // stable partition identity, set for ShrinkAll/UseFreeBoot/OtherDriveShrink
    std::uint64_t shrinkAmountBytes = 0;  // for ShrinkAll / UseFreeBoot / OtherDriveShrink

    // Identity of the target disk at the time the plan was approved.
    // A disk number alone is not enough: the same number can point at a
    // different device after a reboot, a dock change or a re-enumeration,
    // so the device identity and the model and the size are re-checked
    // before anything is written.
    std::string targetDiskModel;
    std::uint64_t targetDiskSizeBytes = 0;
    // The stable identity of the approved device (unique id / serial).
    // It is re-validated against a fresh enumeration before the first
    // destructive step; an empty identity makes the platform backend
    // refuse the operation.
    DiskIdentity targetDiskIdentity;

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
