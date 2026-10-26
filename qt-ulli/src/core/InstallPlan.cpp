// core/InstallPlan.cpp
#include "core/InstallPlan.h"

#include "core/DiskInfo.h"

namespace ulli::core {

QString InstallPlan::summary() const {
    QString s;
    s += QString::fromStdString("Distro: " + distroKey + "\n");
    s += QString::fromStdString("ISO:    " + isoPath.string() + "\n");
    s += QString("Disk:   #%1\n").arg(targetDiskNumber < 0 ? QString("not selected")
                                                            : QString::number(targetDiskNumber));
    if (!targetDiskModel.empty()) {
        s += QString("        %1 (%2)\n")
                 .arg(QString::fromStdString(targetDiskModel),
                      formatBytes(targetDiskSizeBytes));
    }
    switch (strategy) {
        case Strategy::ShrinkAll: s += "Plan:   Shrink target by Linux + boot + rEFInd\n"; break;
        case Strategy::UseFreeAll: s += "Plan:   Use existing unallocated, do not touch OS\n"; break;
        case Strategy::UseFreeBoot: s += "Plan:   Shrink for Linux, use free for boot\n"; break;
        case Strategy::OtherDrive: s += "Plan:   Use free on a different disk\n"; break;
        case Strategy::OtherDriveShrink: s += "Plan:   Shrink a non-OS partition on another disk\n"; break;
        case Strategy::WipeDisk: s += "Plan:   WIPE the target disk (DESTRUCTIVE)\n"; break;
    }
    s += QString("Mode:   %1\n")
             .arg(allocationMode == AllocationMode::LiveOnly ? "Live Only (~7 GB staging)" : "Full Install (staging + Linux unallocated)");
    s += QString("Total shrink from NTFS: %1\n").arg(formatBytes(linuxSizeBytes));
    if (allocationMode == AllocationMode::FullInstall) {
        const std::uint64_t unallocated = (linuxSizeBytes > kStagingSizeBytes) ? (linuxSizeBytes - kStagingSizeBytes) : 0;
        s += QString("  Staging (FAT32): %1\n").arg(formatBytes(kStagingSizeBytes));
        s += QString("  Linux unallocated: %1\n").arg(formatBytes(unallocated));
    }
    s += QString("Boot:   %1  (%2)\n")
             .arg(formatBytes(bootSizeBytes),
                  bootMode == BootMode::Refind ? "with rEFInd" : "direct");
    return s;
}

bool InstallPlan::valid() const {
    if (distroKey.empty()) return false;
    // isoPath is resolved at install time by InstallEngine::runStages
    // (backend_->resolveIso). It must not be required here, otherwise a
    // catalog distro whose ISO is already cached would be rejected before
    // resolution — see Bug A (ISO rediscovery after restart).
    // The target disk must be chosen. Disk numbers are 0-based on Windows
    // (Get-Disk / diskpart), so only kUnsetDiskNumber (-1) means "unset".
    if (targetDiskNumber < 0) return false;
    if (bootSizeBytes < (1ull * 1024 * 1024 * 1024)) return false;   // 1 GiB min
    
    // Validate based on allocation mode
    if (allocationMode == AllocationMode::LiveOnly) {
        // Live-only mode: total shrink must be at least staging size (~7 GB)
        if (linuxSizeBytes < kStagingSizeBytes) return false;
        // For live-only, linuxSizeBytes IS the total shrink amount (staging only)
        // bootSizeBytes should match staging size
        if (bootSizeBytes < kStagingSizeBytes) return false;
    } else {
        // FullInstall mode: total shrink must be at least 30 GB
        if (linuxSizeBytes < kMinFullInstallShrinkBytes) return false;
        // bootSizeBytes should match staging size
        if (bootSizeBytes < kStagingSizeBytes) return false;
    }
    
    // Validate shrink amount if applicable
    if (strategy == Strategy::ShrinkAll || 
        strategy == Strategy::UseFreeBoot || 
        strategy == Strategy::OtherDriveShrink) {
        if (shrinkAmountBytes == 0) return false;
        // A shrink strategy must name the target partition by both drive
        // letter and stable on-disk partition number. The drive letter is
        // cross-checked against the partition number at execution time.
        if (!shrinkDriveLetter.has_value()) return false;
        if (!shrinkPartitionNumber.has_value()) return false;
        // shrinkAmount must be at least the required minimum for the mode
        const std::uint64_t minShrink = (allocationMode == AllocationMode::LiveOnly) 
            ? kStagingSizeBytes 
            : kMinFullInstallShrinkBytes;
        if (shrinkAmountBytes < minShrink) return false;
        // shrinkAmount should match linuxSizeBytes (the user-requested total)
        if (shrinkAmountBytes != linuxSizeBytes) return false;
    }
    return true;
}

}  // namespace ulli::core