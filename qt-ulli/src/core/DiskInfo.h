// core/DiskInfo.h
//
// Value-typed description of one physical disk and its partitions.
// Filled by the platform backend's enumeration function. Used by
// InstallPlan and the UI.

#pragma once

#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ulli::core {

enum class PartitionStyle { Unknown, MBR, GPT };
enum class PartitionKind {
    Unknown,
    Linux, LinuxSwap, LinuxLvm, LinuxRaid,
    Esp, Recovery, MsReserved, WindowsNtfs, WindowsFat, BasicData
};
enum class FileSystem {
    Unknown, Fat32, Ntfs, Ext2, Ext3, Ext4, Btrfs, Xfs, F2fs, Iso9660
};

struct Partition {
    int number = 0;
    PartitionKind kind = PartitionKind::Unknown;
    PartitionStyle style = PartitionStyle::Unknown;
    FileSystem fs = FileSystem::Unknown;
    std::uint64_t sizeBytes = 0;
    std::uint64_t offsetBytes = 0;
    std::optional<char> driveLetter;     // Windows
    std::optional<std::string> mountpoint;  // Linux
    std::string label;
    bool isBoot = false;
    bool isSystem = false;
    bool isHidden = false;
};

struct Disk {
    int number = 0;
    std::string model;
    std::string busType;            // SCSI, NVMe, SATA, USB, ...
    PartitionStyle style = PartitionStyle::Unknown;
    std::uint64_t sizeBytes = 0;
    std::vector<Partition> partitions;
    std::uint64_t unallocatedBytes = 0;

    bool isRemovable() const { return busType == "USB"; }
    bool isSystem() const {
        for (const auto& p : partitions) if (p.isSystem) return true;
        return false;
    }
    QString qmodel() const { return QString::fromStdString(model); }
    QString qbusType() const { return QString::fromStdString(busType); }
};

inline QString formatBytes(std::uint64_t b) {
    constexpr double k = 1024.0;
    constexpr double k2 = k * k;
    constexpr double k3 = k2 * k;
    constexpr double k4 = k3 * k;
    const double bd = static_cast<double>(b);
    if (b >= static_cast<std::uint64_t>(k4)) return QString::number(bd / k4, 'f', 2) + " TiB";
    if (b >= static_cast<std::uint64_t>(k3)) return QString::number(bd / k3, 'f', 2) + " GiB";
    if (b >= static_cast<std::uint64_t>(k2)) return QString::number(bd / k2, 'f', 2) + " MiB";
    if (b >= static_cast<std::uint64_t>(k))  return QString::number(bd / k,  'f', 2) + " KiB";
    return QString::number(b) + " B";
}

}  // namespace ulli::core
