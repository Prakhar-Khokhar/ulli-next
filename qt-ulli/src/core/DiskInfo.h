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
    std::optional<std::string> partitionGuid; // GPT partition GUID when available
};

// Stable identity of one physical disk.
//
// A disk number is not an identity: it is an enumeration index and it
// changes when a device is attached, removed or re-enumerated. Model and
// capacity are not an identity either: two identical drives share both.
// The storage layer therefore exposes identifiers of the device itself:
//
//   * uniqueId      - the storage device id. On Windows it is built from
//                     the device identification descriptor (SCSI VPD page
//                     0x83) and is an opaque value: every character of it
//                     is significant.
//   * uniqueIdFormat- which kind of descriptor the id was built from:
//                     "Vendor Specific" (0), "Vendor Id" (1), "EUI64"
//                     (2), "FCPH Name" (3) or "SCSI Name String" (8). Two
//                     ids are only equivalent when their text matches
//                     *and* their format matches.
//   * serialNumber  - the device serial from SCSI VPD page 0x80 or ATA
//                     IDENTIFY. Weaker than the unique id: some vendors
//                     report placeholders and some enclosures clone the
//                     value.
//
// The comparison rules (and the normalization of the raw values) live in
// core/DiskIdentity.h.
struct DiskIdentity {
    std::string uniqueId;
    std::string uniqueIdFormat;
    std::string serialNumber;

    // True when the identity carries no identifier at all.
    bool empty() const { return uniqueId.empty() && serialNumber.empty(); }
};

struct Disk {
    int number = 0;
    std::string model;
    std::string busType;            // SCSI, NVMe, SATA, USB, ...
    PartitionStyle style = PartitionStyle::Unknown;
    std::uint64_t sizeBytes = 0;
    DiskIdentity identity;          // stable device identity
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
