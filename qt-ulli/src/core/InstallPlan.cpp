// core/InstallPlan.cpp
#include "core/InstallPlan.h"

#include "core/DiskInfo.h"

namespace ulli::core {

QString InstallPlan::summary() const {
    QString s;
    s += QString::fromStdString("Distro: " + distroKey + "\n");
    s += QString::fromStdString("ISO:    " + isoPath.string() + "\n");
    s += QString("Disk:   #%1\n").arg(targetDiskNumber);
    switch (strategy) {
        case Strategy::ShrinkAll: s += "Plan:   Shrink target by Linux + boot + rEFInd\n"; break;
        case Strategy::UseFreeAll: s += "Plan:   Use existing unallocated, do not touch OS\n"; break;
        case Strategy::UseFreeBoot: s += "Plan:   Shrink for Linux, use free for boot\n"; break;
        case Strategy::OtherDrive: s += "Plan:   Use free on a different disk\n"; break;
        case Strategy::OtherDriveShrink: s += "Plan:   Shrink a non-OS partition on another disk\n"; break;
        case Strategy::WipeDisk: s += "Plan:   WIPE the target disk (DESTRUCTIVE)\n"; break;
    }
    s += QString("Linux:  %1\n").arg(formatBytes(linuxSizeBytes));
    s += QString("Boot:   %1  (%2)\n")
             .arg(formatBytes(bootSizeBytes),
                  bootMode == BootMode::Refind ? "with rEFInd" : "direct");
    return s;
}

bool InstallPlan::valid() const {
    if (distroKey.empty()) return false;
    if (isoPath.empty()) return false;
    if (targetDiskNumber <= 0) return false;
    if (linuxSizeBytes < (20ull * 1024 * 1024 * 1024)) return false;  // 20 GiB min
    if (bootSizeBytes < (1ull * 1024 * 1024 * 1024)) return false;   // 1 GiB min
    return true;
}

}  // namespace ulli::core
