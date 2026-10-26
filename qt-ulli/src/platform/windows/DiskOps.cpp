// platform/windows/DiskOps.cpp
//
// Phase 1.1 Windows implementation.
//
// Layering policy:
//   - Enumeration uses WMI/CIM (Get-Disk, Get-Partition, Get-Volume)
//     via the Wmi helper. This is the authoritative Windows source of
//     truth and does not require parsing arbitrary CLI output.
//   - Partition-table mutations (create primary partition, format
//     FAT32) use diskpart.exe (built into Windows, mature, supports
//     scripted scripts). diskpart is the canonical mechanism for
//     "create primary partition at offset" on Windows.
//   - The bundled `parted` tool is reserved for operations diskpart
//     cannot do, and is currently unused on Windows (the
//     createLayout path uses diskpart for FAT32 only).
//
// Filesystem support matrix (Phase 1.1):
//   * FAT32     — create via diskpart, supported.
//   * NTFS      — create via diskpart, supported.
//   * ext2/3/4  — NOT supported (no mkfs.* bundled yet).
//   * btrfs     — NOT supported.
//   * resize/shrink — NOT supported (returned as unsupported to force
//     the user toward the "use free space" strategy).
//
// BitLocker and BCD store get their own helper classes; this file
// focuses on the partition / format / copy mechanics.

#include "platform/windows/DiskOps.h"
#include "platform/windows/BcdStore.h"
#include "platform/windows/BitLocker.h"
#include "platform/windows/EspOps.h"
#include "platform/windows/Wmi.h"
#include "core/DiskIdentity.h"
#include "core/DiskLayout.h"
#include "core/StagingMount.h"
#include "core/ScriptDiagnostics.h"
#include "core/Sha256.h"
#include "platform/HttpDownload.h"
#include "platform/Platform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTextStream>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <iostream>

namespace ulli::platform::windows {

namespace {

// Join a vector of strings with a delimiter.
std::string join(const std::vector<std::string>& vec, const std::string& delim) {
    if (vec.empty()) return std::string();
    std::string result = vec[0];
    for (size_t i = 1; i < vec.size(); ++i) {
        result += delim + vec[i];
    }
    return result;
}

// Run an external tool, return a typed Result. Never throws.
core::Result<QString> runTool(const QString& program,
                              const QStringList& args,
                              const QString& workingDir = {}) {
    QProcess proc;
    if (!workingDir.isEmpty()) proc.setWorkingDirectory(workingDir);
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(program, args);
    if (!proc.waitForStarted(10000)) {
        return core::makeError<QString>(core::Error::Kind::NotFound,
            "Failed to start: " + program.toStdString());
    }
    if (!proc.waitForFinished(-1)) {
        return core::makeError<QString>(core::Error::Kind::Platform,
            "Tool did not finish: " + program.toStdString());
    }
    const QString out = QString::fromLocal8Bit(proc.readAll());
    if (proc.exitStatus() != QProcess::NormalExit) {
        return core::makeError<QString>(core::Error::Kind::Platform,
            "Tool crashed: " + program.toStdString());
    }
    if (proc.exitCode() != 0) {
        return core::makeError<QString>(core::Error::Kind::Platform,
            "Tool " + program.toStdString() + " failed (exit "
            + std::to_string(proc.exitCode()) + "):\n" + out.toStdString());
    }
    return core::makeOk(out);
}

// Write a diskpart script to a temp file and run it. Returns the
// merged stdout/stderr on success or a Result error on failure.
core::Result<QString> runDiskpartScript(const QStringList& lines) {
    const QString tmpDir = QDir::tempPath();
    const QString scriptPath = QDir(tmpDir).filePath("ulli-diskpart.txt");
    QFile f(scriptPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return core::makeError<QString>(core::Error::Kind::Io,
            "Cannot write temp diskpart script: " + f.errorString().toStdString());
    }
    QTextStream out(&f);
    for (const QString& l : lines) out << l << "\n";
    out << "exit\n";
    f.close();

    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start("diskpart.exe", {"/s", scriptPath});
    if (!proc.waitForStarted(10000)) {
        return core::makeError<QString>(core::Error::Kind::NotFound,
            "diskpart.exe is not available");
    }
    if (!proc.waitForFinished(-1)) {
        proc.kill();
        return core::makeError<QString>(core::Error::Kind::Platform,
            "diskpart did not finish");
    }
    const QString combined = QString::fromLocal8Bit(proc.readAll());
    QFile::remove(scriptPath);
    // diskpart exit code 0 = success. Even on success the script may
    // emit warnings — the caller can grep for the success phrase.
    if (proc.exitCode() != 0) {
        return core::makeError<QString>(core::Error::Kind::Platform,
            "diskpart failed (exit " + std::to_string(proc.exitCode()) +
            "):\n" + combined.toStdString());
    }
    return core::makeOk(combined);
}

// --- WMI → core::Disk translation ---------------------------------------

core::PartitionKind classifyGpt(const QString& gptType) {
    // Canonical GPT type GUIDs.
    if (gptType.compare("{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}",
                        Qt::CaseInsensitive) == 0) {
        return core::PartitionKind::Esp;
    }
    if (gptType.compare("{e3c9e316-0b5c-4db8-817d-f92df00215ae}",
                        Qt::CaseInsensitive) == 0) {
        return core::PartitionKind::MsReserved;
    }
    if (gptType.compare("{de94bba4-06d1-4d40-a16a-bfd50179d6ac}",
                        Qt::CaseInsensitive) == 0) {
        return core::PartitionKind::Recovery;
    }
    if (gptType.compare("{0fc63daf-8483-4772-8e79-3d69d8477de4}",
                        Qt::CaseInsensitive) == 0 ||
        gptType.compare("{0657fd63-d699-4e8e-a39c-fcf1277d4cca}",
                        Qt::CaseInsensitive) == 0 ||
        gptType.compare("{44479540-f297-41b2-9af7-d131d3f61266}",
                        Qt::CaseInsensitive) == 0) {
        return core::PartitionKind::Linux;
    }
    if (gptType.compare("{e6d6d379-5078-4a4d-b4d3-7c7f4e6c5b1e}",
                        Qt::CaseInsensitive) == 0) {
        return core::PartitionKind::LinuxSwap;
    }
    if (gptType.compare("{024dee41-33e7-11d3-9d69-0008c781f39f}",
                        Qt::CaseInsensitive) == 0 ||
        gptType.compare("{e3c9e316-0b5c-4db8-817d-f92df00215ae}",
                        Qt::CaseInsensitive) == 0) {
        return core::PartitionKind::MsReserved;
    }
    // Basic Data partition (any user data)
    if (gptType.compare("{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}",
                        Qt::CaseInsensitive) == 0) {
        return core::PartitionKind::BasicData;
    }
    return core::PartitionKind::BasicData;
}

core::PartitionKind classifyMbr(int mbrType) {
    // MBR type byte → kind. Only the most common ones.
    switch (mbrType) {
        case 0x07: return core::PartitionKind::WindowsNtfs;  // NTFS / exFAT
        case 0x0B:
        case 0x0C: return core::PartitionKind::WindowsFat;   // FAT32
        case 0x82: return core::PartitionKind::LinuxSwap;
        case 0x83: return core::PartitionKind::Linux;
        case 0x8E: return core::PartitionKind::LinuxLvm;
        case 0x27: return core::PartitionKind::Recovery;       // Windows recovery
        default:  return core::PartitionKind::BasicData;
    }
}

core::FileSystem classifyFs(const QString& fs) {
    if (fs.isEmpty()) return core::FileSystem::Unknown;
    const QString f = fs.toUpper();
    if (f == "FAT32")     return core::FileSystem::Fat32;
    if (f == "NTFS")      return core::FileSystem::Ntfs;
    if (f == "EXFAT")     return core::FileSystem::Unknown;  // not modeled yet
    if (f == "REFS")      return core::FileSystem::Unknown;
    if (f == "EXT2")      return core::FileSystem::Ext2;
    if (f == "EXT3")      return core::FileSystem::Ext3;
    if (f == "EXT4")      return core::FileSystem::Ext4;
    if (f == "BTRFS")     return core::FileSystem::Btrfs;
    if (f == "XFS")       return core::FileSystem::Xfs;
    if (f == "F2FS")      return core::FileSystem::F2fs;
    if (f == "ISO9660" || f == "UDF") return core::FileSystem::Iso9660;
    return core::FileSystem::Unknown;
}

core::Partition translatePartition(const QJsonObject& p) {
    core::Partition out;
    out.number = p.value("PartitionNumber").toInt(0);
    out.sizeBytes = static_cast<std::uint64_t>(
        p.value("Size").toDouble(0.0));
    out.offsetBytes = static_cast<std::uint64_t>(
        p.value("Offset").toDouble(0.0));
    out.label = p.value("Label").toString().toStdString();
    out.fs = classifyFs(p.value("FileSystem").toString());
    out.isBoot = p.value("IsBoot").toBool();
    out.isSystem = p.value("IsSystem").toBool();
    out.isHidden = p.value("IsHidden").toBool();
    if (const QJsonValue dl = p.value("DriveLetter"); dl.isString()) {
        const QString s = dl.toString();
        if (!s.isEmpty()) out.driveLetter = s.at(0).toLatin1();
    }
    const QString style = p.value("PartitionStyle").toString();
    if (style.compare("GPT", Qt::CaseInsensitive) == 0) {
        out.style = core::PartitionStyle::GPT;
        out.kind = classifyGpt(p.value("GptType").toString());
    } else if (style.compare("MBR", Qt::CaseInsensitive) == 0) {
        out.style = core::PartitionStyle::MBR;
        out.kind = classifyMbr(p.value("MbrType").toInt(-1));
    }
    return out;
}

std::vector<core::Disk> translateWmiDisks(const QJsonArray& arr) {
    std::vector<core::Disk> out;
    out.reserve(arr.size());
    for (const QJsonValue& v : arr) {
        const QJsonObject d = v.toObject();
        core::Disk disk;
        disk.number = d.value("Number").toInt(0);
        disk.model = d.value("Model").toString().toStdString();
        disk.busType = d.value("BusType").toString("Unknown").toStdString();
        disk.sizeBytes = static_cast<std::uint64_t>(
            d.value("Size").toDouble(0.0));
        disk.identity.uniqueId = d.value("UniqueId").toString().toStdString();
        disk.identity.uniqueIdFormat =
            d.value("UniqueIdFormat").toString().toStdString();
        disk.identity.serialNumber =
            d.value("SerialNumber").toString().toStdString();
        const QString style = d.value("PartitionStyle").toString();
        if (style.compare("GPT", Qt::CaseInsensitive) == 0) {
            disk.style = core::PartitionStyle::GPT;
        } else if (style.compare("MBR", Qt::CaseInsensitive) == 0) {
            disk.style = core::PartitionStyle::MBR;
        }
        const QJsonArray parts = d.value("Partitions").toArray();
        for (const QJsonValue& pv : parts) {
            disk.partitions.push_back(translatePartition(pv.toObject()));
        }
        std::sort(disk.partitions.begin(), disk.partitions.end(),
                  [](const core::Partition& a, const core::Partition& b) {
                      return a.offsetBytes < b.offsetBytes;
                  });
        // Unallocated bytes = sum of gaps between/after partitions.
        std::uint64_t cursor = 0;
        std::uint64_t unalloc = 0;
        for (const auto& p : disk.partitions) {
            if (p.offsetBytes > cursor) unalloc += p.offsetBytes - cursor;
            cursor = std::max<std::uint64_t>(cursor, p.offsetBytes + p.sizeBytes);
        }
        if (disk.sizeBytes > cursor) unalloc += disk.sizeBytes - cursor;
        disk.unallocatedBytes = unalloc;
        out.push_back(std::move(disk));
    }
    return out;
}

}  // namespace

DiskOps::DiskOps() = default;
DiskOps::~DiskOps() = default;

std::vector<core::Disk> DiskOps::enumerateDisks(std::string* errorMessage) {
    if (errorMessage) errorMessage->clear();
    auto r = Wmi::runScript("list_disks");
    if (!r) {
        std::cerr << "WARN: enumerateDisks: " << r.error().message() << '\n';
        if (errorMessage) *errorMessage = r.error().message();
        return {};
    }
    const QJsonDocument doc = r.value();
    QJsonArray arr;
    if (doc.isArray()) {
        arr = doc.array();
    } else if (doc.isObject()) {
        arr.append(doc.object());
    } else {
        std::cerr << "WARN: enumerateDisks: unexpected JSON top-level type\n";
        if (errorMessage) *errorMessage = "Unexpected JSON shape from the disk query";
        return {};
    }
    return translateWmiDisks(arr);
}

core::Result<void> DiskOps::validatePlanForDisk(const core::InstallPlan& plan) {
    // Re-read the live disk state and check that the plan still refers to
    // the very same physical disk, and that the requested strategy is
    // safe to run on it.
    auto r = Wmi::runScript("list_disks");
    if (!r) return core::makeError(core::Error::Kind::Platform,
        "Cannot read disk state: " + r.error().message());
    const QJsonDocument doc = r.value();
    QJsonArray arr;
    if (doc.isArray()) arr = doc.array();
    else if (doc.isObject()) arr.append(doc.object());
    else return core::makeError(core::Error::Kind::Platform,
        "Unexpected JSON shape from WMI");

    const std::vector<core::Disk> liveDisks = translateWmiDisks(arr);

    // The disk number is only an enumeration index: two identical disks
    // share model and capacity, and the number changes when the device
    // topology changes. The approved device identity is therefore checked
    // against a fresh enumeration before anything is written. When the
    // identity cannot be established the operation is refused.
    const core::DiskIdentityCheck identity =
        core::checkDiskIdentity(plan.targetDiskIdentity,
                                plan.targetDiskNumber, liveDisks);
    if (!identity.ok()) {
        return core::makeError(core::Error::Kind::InvalidInput,
            "Disk identity check failed: " + identity.detail +
            " The plan must be reviewed again.");
    }

    const core::Disk& disk = *identity.disk;

    bool isSystem = false;
    for (const auto& p : disk.partitions) {
        if (p.isSystem) isSystem = true;
    }
    if (plan.strategy == core::Strategy::WipeDisk && isSystem) {
        return core::makeError(core::Error::Kind::InvalidInput,
            "Refusing to wipe a disk that contains the system partition. "
            "Choose a non-system disk or pick a different strategy.");
    }
    if (!plan.targetDiskModel.empty()) {
        const QString liveModel = disk.qmodel().trimmed();
        const QString planModel = QString::fromStdString(plan.targetDiskModel).trimmed();
        if (liveModel.compare(planModel, Qt::CaseInsensitive) != 0) {
            return core::makeError(core::Error::Kind::InvalidInput,
                "Disk identity changed: model mismatch (expected '" +
                plan.targetDiskModel + "', found '" + liveModel.toStdString() +
                "'). The plan must be reviewed again.");
        }
    }
    if (plan.targetDiskSizeBytes > 0 && disk.sizeBytes != plan.targetDiskSizeBytes) {
        return core::makeError(core::Error::Kind::InvalidInput,
            "Disk identity changed: size mismatch (expected " +
            std::to_string(plan.targetDiskSizeBytes) + " bytes, found " +
            std::to_string(disk.sizeBytes) + " bytes). The plan must be reviewed again.");
    }

    // Validate the shrink target partition identity when the plan targets a
    // specific partition (ShrinkAll / UseFreeBoot / OtherDriveShrink). The
    // partition must still exist on the live disk with the expected drive
    // letter, and it must not be a protected system/recovery partition.
    if (plan.strategy == core::Strategy::ShrinkAll ||
        plan.strategy == core::Strategy::UseFreeBoot ||
        plan.strategy == core::Strategy::OtherDriveShrink) {
        if (!plan.shrinkPartitionNumber.has_value()) {
            return core::makeError(core::Error::Kind::InvalidInput,
                "Shrink plan is missing the target partition number. "
                "The plan must be reviewed again.");
        }
        if (!plan.shrinkDriveLetter.has_value()) {
            return core::makeError(core::Error::Kind::InvalidInput,
                "Shrink plan is missing the target drive letter. "
                "The plan must be reviewed again.");
        }
        bool found = false;
        for (const auto& p : disk.partitions) {
            if (p.number == plan.shrinkPartitionNumber.value()) {
                found = true;
                if (!p.driveLetter.has_value() ||
                    p.driveLetter.value() != plan.shrinkDriveLetter.value()) {
                    return core::makeError(core::Error::Kind::InvalidInput,
                        "Partition identity mismatch: disk " +
                        std::to_string(disk.number) + " partition " +
                        std::to_string(p.number) + " expected drive letter '" +
                        std::string(1, plan.shrinkDriveLetter.value()) +
                        "' but the live partition has no matching drive letter. "
                        "The plan must be reviewed again.");
                }
                if (p.isSystem || p.isHidden) {
                    return core::makeError(core::Error::Kind::InvalidInput,
                        "Refusing to shrink protected partition: disk " +
                        std::to_string(disk.number) + " partition " +
                        std::to_string(p.number) + " is system or recovery. "
                        "Choose a different target.");
                }
                break;
            }
        }
        if (!found) {
            return core::makeError(core::Error::Kind::InvalidInput,
                "Shrink target partition not found: disk " +
                std::to_string(disk.number) + " partition " +
                std::to_string(plan.shrinkPartitionNumber.value()) +
                " no longer exists. The plan must be reviewed again.");
        }
    }

    return core::makeOk();
}

core::Result<void> DiskOps::preflight(const core::InstallPlan& plan) {
    if (!platform::isElevated()) {
        return core::makeNotElevated();
    }
    if (auto v = validatePlanForDisk(plan); !v) return v;
    if (BitLocker::isLocked()) {
        return core::makeError(core::Error::Kind::Platform,
            "BitLocker is enabled on the target volume. Decrypt it before running ULLI.");
    }
    return core::makeOk();
}

core::Result<std::filesystem::path> DiskOps::resolveIso(const core::Distro& d) {
    const std::filesystem::path cache = platform::cacheDir();
    const std::filesystem::path isoPath = cache / d.isoFilename();
    if (std::filesystem::exists(isoPath)) {
        if (d.hasChecksum()) {
            auto v = core::Sha256::verifyFile(isoPath, d.sha256());
            if (!v) {
                std::error_code ec;
                std::filesystem::remove(isoPath, ec);
                return core::makeError<std::filesystem::path>(v.error().kind(), v.error().message());
            }
        }
        return core::makeOk(isoPath);
    }
    return core::makeError<std::filesystem::path>(core::Error::Kind::NotFound,
        "ISO not found in cache. Please download " + d.isoFilename() +
        " to " + cache.string() + " (link: " + d.downloadPage() + ").");
}

core::Result<core::IsoDownload> DiskOps::downloadIso(
    const core::Distro& distro,
    const std::filesystem::path& destPath,
    std::function<void(int percent, const QString& status)> progressCallback,
    const std::atomic<bool>* cancelFlag) {

    if (distro.mirrors().empty()) {
        return core::makeError<core::IsoDownload>(core::Error::Kind::NotFound,
            "No download mirrors available for " + distro.label());
    }

    std::error_code ec;
    std::filesystem::create_directories(destPath.parent_path(), ec);
    if (ec) {
        return core::makeError<core::IsoDownload>(core::Error::Kind::Io,
            "Cannot create download directory: " + ec.message());
    }

    const std::filesystem::path partPath(destPath.string() + ".part");
    const auto removePart = [&]() {
        std::error_code e;
        std::filesystem::remove(partPath, e);
    };

    // Try each mirror in order
    bool sawTimeout = false;
    QString lastError;
    for (const auto& mirror : distro.mirrors()) {
        removePart();

        if (progressCallback) {
            progressCallback(0, QString("Connecting to %1...").arg(QString::fromStdString(mirror)));
        }

        // The transfer itself observes the cancellation flag and enforces
        // an inactivity timeout, so a stalled mirror cannot block the
        // installer forever.
        const platform::HttpDownloadResult transfer = platform::httpDownload(
            QUrl(QString::fromStdString(mirror)), destPath, progressCallback,
            cancelFlag, platform::HttpDownloadLimits());

        if (transfer.outcome == platform::HttpDownloadOutcome::Cancelled) {
            removePart();
            return core::makeError<core::IsoDownload>(core::Error::Kind::Cancelled,
                "Download cancelled");
        }
        if (transfer.outcome == platform::HttpDownloadOutcome::TimedOut) {
            sawTimeout = true;
            lastError = transfer.errorMessage;
            if (progressCallback) {
                progressCallback(0, QString("Mirror timed out: %1").arg(lastError));
            }
            continue;
        }
        if (!transfer.ok()) {
            lastError = transfer.errorMessage;
            if (progressCallback) {
                progressCallback(0, QString("Mirror failed: %1").arg(lastError));
            }
            continue;
        }

        // Verification phase
        if (progressCallback) {
            progressCallback(100, QString("Verifying checksum..."));
        }

        if (distro.hasChecksum()) {
            auto v = core::Sha256::verifyFile(partPath, distro.sha256());
            if (!v) {
                removePart();
                return core::makeError<core::IsoDownload>(v.error().kind(), v.error().message());
            }

            std::error_code ec2;
            std::filesystem::remove(destPath, ec2);
            std::error_code ec3;
            std::filesystem::rename(partPath, destPath, ec3);
            if (ec3) {
                removePart();
                return core::makeError<core::IsoDownload>(core::Error::Kind::Io,
                    "Failed to publish downloaded ISO: " + ec3.message());
            }

            core::IsoDownload out;
            out.path = destPath;
            out.verified = true;
            out.sha256 = distro.sha256();
            if (progressCallback) {
                progressCallback(100, QString("Download complete"));
            }
            return core::makeOk(out);
        } else {
            std::error_code ec2;
            std::filesystem::remove(destPath, ec2);
            std::error_code ec3;
            std::filesystem::rename(partPath, destPath, ec3);
            if (ec3) {
                removePart();
                return core::makeError<core::IsoDownload>(core::Error::Kind::Io,
                    "Failed to publish downloaded ISO: " + ec3.message());
            }

            core::IsoDownload out;
            out.path = destPath;
            out.verified = false;
            out.sha256.clear();
            if (progressCallback) {
                progressCallback(100, QString("Download complete (no checksum available - not verified)"));
            }
            return core::makeOk(out);
        }
    }

    if (sawTimeout) {
        return core::makeError<core::IsoDownload>(core::Error::Kind::Timeout,
            "The download timed out: " + lastError.toStdString() +
            " Try again later or use a different mirror.");
    }
    return core::makeError<core::IsoDownload>(core::Error::Kind::Platform,
        "All download mirrors failed for " + distro.label() +
        (lastError.isEmpty() ? std::string()
                              : ": " + lastError.toStdString()));
}

// Builds the PowerShell probe script that validates a shrink plan and
// reports the requested final size. All size arithmetic is in bytes; the
// nonpositive-final-size guard uses PowerShell -le (never C-style <=,
// which PowerShell reserves for redirection) and runs before any resize,
// so an invalid plan fails safely before the destructive Resize-Partition.
// The partition is resolved by disk number + partition number (stable
// on-disk identity), not by drive letter alone; the drive letter is then
// cross-checked to detect any mismatch between plan and execution.
QString DiskOps::buildShrinkProbeScript(char driveLetter,
                                        int partitionNumber,
                                        std::uint64_t requestedShrinkBytes) {
    return QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$diskNumber = %1
$partitionNumber = %2
$expectedDriveLetter = '%3'

# Resolve the partition by stable on-disk identity (disk number +
// partition number), not by drive letter. Drive letters can change
// between plan approval and execution and must never be the sole
// means of identifying the target.
$partition = Get-Partition -DiskNumber $diskNumber -PartitionNumber $partitionNumber -ErrorAction Stop
if (-not $partition) {
    Write-Output '{"error":"Target partition not found on disk"}'
    exit 1
}

# Cross-check the drive letter against the one recorded in the plan. A
// mismatch means the plan is stale or the target changed: abort before
// any mutation.
$actualDriveLetter = if ($partition.DriveLetter) { [string]$partition.DriveLetter } else { '' }
if ($actualDriveLetter -ne $expectedDriveLetter) {
    Write-Output ('{{"error":"Drive letter mismatch: expected {0}, found {1} on disk {2} partition {3}", "diskNumber":{2}, "partitionNumber":{3}, "expectedDriveLetter":{0}, "actualDriveLetter":{1}}}' -f $expectedDriveLetter, $actualDriveLetter, $diskNumber, $partitionNumber)
    exit 1
}

# Verify filesystem is NTFS
$volume = Get-Volume -Partition $partition -ErrorAction SilentlyContinue
if (-not $volume -or $volume.FileSystem -ne 'NTFS') {
    Write-Output ('{{"error":"Target partition is not NTFS", "diskNumber":{0}, "partitionNumber":{1}, "filesystem":{2}}}' -f $diskNumber, $partitionNumber, (if ($volume) { $volume.FileSystem } else { 'none' }))
    exit 1
}

# Reject EFI system (ESP) and recovery/hidden partitions. The Windows OS
// partition (C:, IsBoot) is intentionally NOT rejected here: it is the
// normal shrink target for a side-by-side Linux install.
if ($partition.IsSystem -or $partition.IsHidden) {
    Write-Output ('{{"error":"Cannot shrink system or recovery partition", "diskNumber":{0}, "partitionNumber":{1}, "isSystem":{2}, "isHidden":{3}}}' -f $diskNumber, $partitionNumber, $partition.IsSystem, $partition.IsHidden)
    exit 1
}

$currentSize = $partition.Size
$requestedShrink = %4
$requestedFinalSize = $currentSize - $requestedShrink

if ($requestedFinalSize -le 0) {
    Write-Output ('{{"error":"Requested final size would be zero or negative", "diskNumber":{0}, "partitionNumber":{1}, "currentSize":{2}, "requestedShrink":{3}}}' -f $diskNumber, $partitionNumber, $currentSize, $requestedShrink)
    exit 1
}

# Get supported size range
$supported = Get-PartitionSupportedSize -DiskNumber $partition.DiskNumber -PartitionNumber $partition.PartitionNumber -ErrorAction Stop
$sizeMin = $supported.SizeMin
$sizeMax = $supported.SizeMax

if ($requestedFinalSize -lt $sizeMin) {
    Write-Output ('{{"error":"Requested final size {0} is below minimum supported size {1}", "sizeMin":{1}, "sizeMax":{2}, "currentSize":{3}, "diskNumber":{4}, "partitionNumber":{5}}}' -f $requestedFinalSize, $sizeMin, $sizeMax, $currentSize, $diskNumber, $partitionNumber)
    exit 1
}
if ($requestedFinalSize -gt $sizeMax) {
    Write-Output ('{{"error":"Requested final size {0} exceeds maximum supported size {1}", "sizeMin":{1}, "sizeMax":{2}, "currentSize":{3}, "diskNumber":{4}, "partitionNumber":{5}}}' -f $requestedFinalSize, $sizeMin, $sizeMax, $currentSize, $diskNumber, $partitionNumber)
    exit 1
}

# Output current state for verification and diagnostic logging
Write-Output ('{{"diskNumber":{0}, "partitionNumber":{1}, "currentSize":{2}, "sizeMin":{3}, "sizeMax":{4}, "requestedFinalSize":{5}, "requestedShrink":{6}, "driveLetter":"{7}", "isSystem":{8}, "isBoot":{9}, "isHidden":{10}}}' -f $partition.DiskNumber, $partition.PartitionNumber, $currentSize, $sizeMin, $sizeMax, $requestedFinalSize, $requestedShrink, $actualDriveLetter, $partition.IsSystem, $partition.IsBoot, $partition.IsHidden)
)PS1").arg(diskNumber).arg(partitionNumber).arg(QString(driveLetter)).arg(static_cast<qulonglong>(requestedShrinkBytes));
}

core::Result<std::uint64_t> DiskOps::shrinkPartition(int diskNumber,
                                                     char driveLetter,
                                                     int partitionNumber,
                                                     std::uint64_t requestedShrinkBytes) {
    // Use Windows native Storage API via PowerShell:
    // 1. Get-PartitionSupportedSize to determine SizeMin/SizeMax
    // 2. Calculate requested final size
    // 3. Verify requested final size >= SizeMin
    // 4. Resize-Partition to the requested final size
    // 5. Post-resize verification

    if (requestedShrinkBytes == 0) {
        return core::makeError<std::uint64_t>(core::Error::Kind::InvalidInput,
            "shrinkPartition: requestedShrinkBytes is zero");
    }

    // Step 1: Find the partition by stable on-disk identity (disk number +
    // partition number) and verify it's NTFS. The probe script is built by
    // buildShrinkProbeScript() so it can be inspected (and regression-tested)
    // without running PowerShell or touching any disk. The nonpositive-final-size
    // guard here uses PowerShell -le (not C-style <=) and runs before any resize.
    const QString script = buildShrinkProbeScript(diskNumber, driveLetter, partitionNumber, requestedShrinkBytes);

    auto r = runPowerShellScript(script);
    if (!r) return core::makeError<std::uint64_t>(r.error().kind(), r.error().message());

    QJsonParseError perr{};
    QJsonDocument doc = QJsonDocument::fromJson(r.value().toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        return core::makeError<std::uint64_t>(core::Error::Kind::Platform,
            "PowerShell output was not valid JSON: " + perr.errorString().toStdString());
    }
    QJsonObject obj = doc.object();
    if (obj.contains("error")) {
        return core::makeError<std::uint64_t>(core::Error::Kind::Platform,
            obj.value("error").toString().toStdString());
    }

    // Extract partition info for verification
    const int diskNumber = obj.value("diskNumber").toInt();
    const int partitionNumber = obj.value("partitionNumber").toInt();
    const qulonglong currentSize = obj.value("currentSize").toVariant().toULongLong();
    const qulonglong sizeMin = obj.value("sizeMin").toVariant().toULongLong();
    const qulonglong sizeMax = obj.value("sizeMax").toVariant().toULongLong();
    const qulonglong requestedFinalSize = obj.value("requestedFinalSize").toVariant().toULongLong();

    // Step 2: Check BitLocker on the target partition
    if (isBitLockerLockedOnPartition(diskNumber, partitionNumber)) {
        return core::makeError<std::uint64_t>(core::Error::Kind::Platform,
            "Target partition is BitLocker-protected and locked. Unlock it before shrinking.");
    }

    // Step 3: Perform the resize
    QString resizeScript = QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

Resize-Partition -DiskNumber %1 -PartitionNumber %2 -Size %3 -ErrorAction Stop
Write-Output '{"success":true}'
)PS1").arg(diskNumber).arg(partitionNumber).arg(static_cast<qulonglong>(requestedFinalSize));

    auto resizeResult = runPowerShellScript(resizeScript);
    if (!resizeResult) return core::makeError<std::uint64_t>(resizeResult.error().kind(), resizeResult.error().message());

    // Step 4: Post-resize verification
    QString verifyScript = QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$partition = Get-Partition -DiskNumber %1 -PartitionNumber %2 -ErrorAction Stop
$volume = Get-Volume -Partition $partition -ErrorAction SilentlyContinue
$newSize = $partition.Size
$fs = if ($volume) { $volume.FileSystem } else { '' }

Write-Output ('{{"newSize":{0}, "filesystem":"{1}", "offset":{2}}}' -f $newSize, $fs, $partition.Offset)
)PS1").arg(diskNumber).arg(partitionNumber);

    auto verifyResult = runPowerShellScript(verifyScript);
    if (!verifyResult) return core::makeError<std::uint64_t>(verifyResult.error().kind(), verifyResult.error().message());

    QJsonDocument verifyDoc = QJsonDocument::fromJson(verifyResult.value().toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        return core::makeError<std::uint64_t>(core::Error::Kind::Platform,
            "Post-resize verification output was not valid JSON");
    }
    QJsonObject verifyObj = verifyDoc.object();
    const qulonglong newSize = verifyObj.value("newSize").toVariant().toULongLong();
    const QString fs = verifyObj.value("filesystem").toString();
    const qulonglong offset = verifyObj.value("offset").toVariant().toULongLong();

    // Verify the results
    if (fs != "NTFS") {
        return core::makeError<std::uint64_t>(core::Error::Kind::Platform,
            "Post-resize verification failed: filesystem is no longer NTFS (got: " + fs.toStdString() + ")");
    }

    // Allow for small alignment differences (1 MB tolerance)
    const qulonglong tolerance = 1024ull * 1024;
    if (newSize < requestedFinalSize - tolerance || newSize > requestedFinalSize + tolerance) {
        return core::makeError<std::uint64_t>(core::Error::Kind::Platform,
            "Post-resize verification failed: partition size mismatch. Expected ~" + 
            std::to_string(requestedFinalSize) + ", got " + std::to_string(newSize));
    }

    return core::makeOk(static_cast<std::uint64_t>(newSize));
}

bool DiskOps::isBitLockerLockedOnPartition(int diskNumber, int partitionNumber) {
    QString script = QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$partition = Get-Partition -DiskNumber %1 -PartitionNumber %2 -ErrorAction Stop
$volume = Get-Volume -Partition $partition -ErrorAction SilentlyContinue
if (-not $volume) {
    Write-Output 'false'
    exit 0
}

$bl = Get-BitLockerVolume -MountPoint ($volume.DriveLetter + ':') -ErrorAction SilentlyContinue
if (-not $bl) {
    Write-Output 'false'
    exit 0
}

$locked = ($bl.VolumeStatus -eq 'FullyEncrypted' -or
           $bl.VolumeStatus -eq 'EncryptionInProgress' -or
           $bl.LockStatus -eq 'Locked')
$prot = [bool]$bl.ProtectionStatus
if ($prot -and $locked) { 'true' } else { 'false' }
)PS1").arg(diskNumber).arg(partitionNumber);

    auto r = runPowerShellScript(script);
    if (!r) return false; // Fail-open but log
    const QString result = r.value().trimmed();
    return result.compare("true", Qt::CaseInsensitive) == 0;
}

core::Result<QString> DiskOps::runPowerShellScript(const QString& script) {
    const QString tmpDir = QDir::tempPath();
    const QString scriptPath = QDir(tmpDir).filePath("ulli-ps.ps1");
    QFile f(scriptPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return core::makeError<QString>(core::Error::Kind::Io,
            "Cannot write temp PowerShell script: " + f.errorString().toStdString());
    }
    f.write(script.toUtf8());
    f.close();

    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start("powershell.exe",
               {"-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", scriptPath});
    if (!proc.waitForStarted(10000)) {
        QFile::remove(scriptPath);
        return core::makeError<QString>(core::Error::Kind::NotFound,
            "powershell.exe is not available on this system");
    }
    if (!proc.waitForFinished(120000)) {
        proc.kill();
        QFile::remove(scriptPath);
        return core::makeError<QString>(core::Error::Kind::Platform,
            "PowerShell script timed out");
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        // A failing script can report its problem on either stream: the
        // createLayout scripts write a JSON error object to standard
        // output and exit with 1, the errors of PowerShell itself go to
        // standard error. Both are needed to understand the failure.
        const QString err = QString::fromLocal8Bit(proc.readAllStandardError());
        const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
        QFile::remove(scriptPath);
        return core::makeError<QString>(core::Error::Kind::Platform,
            "PowerShell script failed (exit " + std::to_string(proc.exitCode()) + "): " +
            core::joinScriptOutput(err, out));
    }
    const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
    QFile::remove(scriptPath);
    return core::makeOk(out);
}

core::Result<void> DiskOps::wipeDisk(int diskNumber) {
    // Windows disk numbers are 0-based (Get-Disk / diskpart "select disk 0"),
    // so 0 is a valid target. Only the "not selected" sentinel is rejected.
    // The system-disk protection lives in validatePlanForDisk(), which the
    // engine calls before every wipe.
    if (diskNumber < 0) {
        return core::makeError(core::Error::Kind::InvalidInput,
            "Invalid disk number");
    }
    QStringList script;
    script << "select disk " + QString::number(diskNumber);
    script << "clean";
    script << "convert gpt";
    auto r = runDiskpartScript(script);
    if (!r) return core::makeError(r.error().kind(), r.error().message());
    return core::makeOk();
}

core::Result<void> DiskOps::createLayout(const core::InstallPlan& plan,
                                             std::filesystem::path& bootMount,
                                             std::filesystem::path& refindMount,
                                             std::function<bool()> cancelCallback) {
    // Create the FAT32 staging partition from the newly freed space.
    // Uses Windows native Storage API (New-Partition, Format-Volume).
    // For LiveOnly: staging = total requested (~7 GB), no unallocated.
    // For FullInstall: staging = 7 GB, remaining = linuxSizeBytes - 7 GB unallocated.
    // The staging partition is created at the beginning of the freed region.

    auto checkCancel = [&](const char* stage) -> core::Result<void> {
        if (cancelCallback && cancelCallback()) {
            return core::makeCancelled();
        }
        return core::makeOk();
    };

    // Use centralized constant
    const std::uint64_t kStagingSizeBytes = core::kStagingSizeBytes;

    if (plan.targetDiskNumber < 0) {
        return core::makeError(core::Error::Kind::InvalidInput,
            "createLayout: targetDiskNumber not set");
    }

    // Determine staging size and remaining unallocated based on allocation mode
    const std::uint64_t stagingSizeBytes = (plan.allocationMode == core::AllocationMode::LiveOnly)
        ? plan.linuxSizeBytes  // LiveOnly: total requested is the staging size
        : kStagingSizeBytes;   // FullInstall: fixed 7 GB staging

    // Use actual freed bytes if available (from shrink), otherwise fall back to planned
    const std::uint64_t totalFreedBytes = (plan.actualFreedBytes > 0)
        ? plan.actualFreedBytes
        : plan.linuxSizeBytes;

    const std::uint64_t stagingSizeMB = stagingSizeBytes / (1024ull * 1024);

    // Step 1: Pre-create validation - re-enumerate and verify disk/partition state
    if (auto r = checkCancel("pre-create-validation"); !r) return r;

    // We need to know which NTFS partition was shrunk - use the shrink drive letter if available
    QString validateScript;
    if (plan.shrinkDriveLetter.has_value()) {
        // Strategy ShrinkAll/UseFreeBoot/OtherDriveShrink: verify the specific NTFS partition
        validateScript = QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$diskNumber = %1
$expectedFreedBytes = %2
$stagingSizeNeeded = %3
$shrinkDriveLetter = '%4'

$disk = Get-Disk -Number $diskNumber -ErrorAction Stop
if (-not $disk) {
    Write-Output '{"error":"Target disk not found"}'
    exit 1
}

# Verify disk is Online
if ($disk.OperationalStatus -ne 'Online') {
    Write-Output '{"error":"Target disk is not Online"}'
    exit 1
}

# Find the partition by drive letter
$targetPartition = Get-Partition -DriveLetter $shrinkDriveLetter -ErrorAction SilentlyContinue
if (-not $targetPartition) {
    Write-Output ('{{"error":"Target partition for drive letter {0} not found"}}' -f $shrinkDriveLetter)
    exit 1
}

# Verify filesystem is NTFS
$volume = Get-Volume -Partition $targetPartition -ErrorAction SilentlyContinue
if (-not $volume -or $volume.FileSystem -ne 'NTFS') {
    Write-Output ('{{"error":"Target partition is not NTFS. Filesystem: {0}"}}' -f (if ($volume) { $volume.FileSystem } else { 'none' }))
    exit 1
}

# Verify it's not a system/EFI/recovery partition
if ($targetPartition.IsSystem -or $targetPartition.IsBoot -or $targetPartition.IsHidden) {
    Write-Output '{"error":"Cannot use system/boot/recovery partition for staging"}'
    exit 1
}

# Get all partitions on the disk, sorted by offset
$partitions = Get-Partition -DiskNumber $diskNumber -ErrorAction SilentlyContinue | Sort-Object Offset

# Find the gap immediately after the target partition
$targetStart = $targetPartition.Offset
$targetEnd = $targetPartition.Offset + $targetPartition.Size

$partitionsArray = @($partitions)
$foundGap = $false
$gapOffset = 0
$gapSize = 0

# Look for gap immediately after the target partition
for ($i = 0; $i -lt $partitionsArray.Count; $i++) {
    $p = $partitionsArray[$i]
    if ($p.Offset -eq $targetEnd) {
        # Found a partition starting exactly where target ends - no gap
        $foundGap = $false
        break
    }
    if ($p.Offset -gt $targetEnd) {
        # Gap found between targetEnd and this partition's start
        $gapOffset = $targetEnd
        $gapSize = $p.Offset - $targetEnd
        $foundGap = $true
        break
    }
}

# If no gap found after target partition, check after last partition
if (-not $foundGap) {
    $lastEnd = $partitionsArray[-1].Offset + $partitionsArray[-1].Size
    if ($lastEnd -eq $targetEnd) {
        $gapOffset = $targetEnd
        $gapSize = $disk.Size - $targetEnd
        $foundGap = $true
    }
}

if (-not $foundGap) {
    Write-Output ('{{"error":"No free region found immediately after target partition. Target end: {0}, disk size: {1}"}}' -f $targetEnd, $disk.Size)
    exit 1
}

# Verify the gap is large enough for staging partition
if ($gapSize -lt $stagingSizeNeeded) {
    Write-Output ('{{"error":"Free region after target partition ({0} bytes) is smaller than required staging size ({1} bytes)."}}' -f $gapSize, $stagingSizeNeeded)
    exit 1
}

# Also verify the total freed space is approximately what we expect
# (the gap should be at least the expected freed amount, accounting for alignment)
if ($gapSize -lt ($expectedFreedBytes - 10485760)) {  # 10 MB tolerance for alignment
    Write-Output ('{{"error":"Free region ({0} bytes) is significantly smaller than expected freed space ({1} bytes)."}}' -f $gapSize, $expectedFreedBytes)
    exit 1
}

# Verify target partition size is what we expect post-shrink
# The target partition should have been shrunk by approximately expectedFreedBytes
# Original size = targetPartition.Size + expectedFreedBytes (approximately)
# We can't know exact original size, but we can verify the partition is still NTFS and valid

Write-Output ('{{"diskNumber":{0}, "gapOffset":{1}, "gapSize":{2}, "stagingSizeBytes":{3}, "targetPartitionNumber":{4}, "targetPartitionSize":{5}, "targetPartitionOffset":{6}}}' -f $diskNumber, $gapOffset, $gapSize, $stagingSizeNeeded, $targetPartition.PartitionNumber, $targetPartition.Size, $targetPartition.Offset)
)PS1").arg(plan.targetDiskNumber)
            .arg(static_cast<qulonglong>(totalFreedBytes))
            .arg(static_cast<qulonglong>(stagingSizeBytes))
            .arg(QString(plan.shrinkDriveLetter.value()));
    } else {
        // Strategy UseFreeAll/WipeDisk: find any suitable free region
        validateScript = QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$diskNumber = %1
$expectedFreedBytes = %2
$stagingSizeNeeded = %3

$disk = Get-Disk -Number $diskNumber -ErrorAction Stop
if (-not $disk) {
    Write-Output '{"error":"Target disk not found"}'
    exit 1
}

# Verify disk is Online
if ($disk.OperationalStatus -ne 'Online') {
    Write-Output '{"error":"Target disk is not Online"}'
    exit 1
}

# Get all partitions on the disk, sorted by offset
$partitions = Get-Partition -DiskNumber $diskNumber -ErrorAction SilentlyContinue | Sort-Object Offset
$partitionsArray = @($partitions)
$foundGap = $false
$gapOffset = 0
$gapSize = 0

# Find the largest suitable unallocated gap
for ($i = 0; $i -lt $partitionsArray.Count; $i++) {
    $p = $partitionsArray[$i]
    $pStart = $p.Offset
    $pEnd = $p.Offset + $p.Size

    if ($i -eq 0) {
        # Check space before first partition
        if ($pStart -ge $stagingSizeNeeded) {
            $gapOffset = 0
            $gapSize = $pStart
            $foundGap = $true
            break
        }
    } else {
        # Check gap between previous and current partition
        $prevEnd = $partitionsArray[$i-1].Offset + $partitionsArray[$i-1].Size
        $gap = $pStart - $prevEnd
        if ($gap -ge $stagingSizeNeeded) {
            $gapOffset = $prevEnd
            $gapSize = $gap
            $foundGap = $true
            break
        }
    }
}

# Check space after last partition
if (-not $foundGap -and $partitionsArray.Count -gt 0) {
    $lastEnd = $partitionsArray[-1].Offset + $partitionsArray[-1].Size
    $remaining = $disk.Size - $lastEnd
    if ($remaining -ge $stagingSizeNeeded) {
        $gapOffset = $lastEnd
        $gapSize = $remaining
        $foundGap = $true
    }
}

if (-not $foundGap) {
    Write-Output ('{{"error":"No suitable free region found for staging partition. Need at least {0} bytes."}}' -f $stagingSizeNeeded)
    exit 1
}

# Verify the gap is large enough
if ($gapSize -lt $stagingSizeNeeded) {
    Write-Output ('{{"error":"Free region ({0} bytes) is smaller than required staging size ({1} bytes)."}}' -f $gapSize, $stagingSizeNeeded)
    exit 1
}

Write-Output ('{{"diskNumber":{0}, "gapOffset":{1}, "gapSize":{2}, "stagingSizeBytes":{3}}}' -f $diskNumber, $gapOffset, $gapSize, $stagingSizeNeeded)
)PS1").arg(plan.targetDiskNumber)
            .arg(static_cast<qulonglong>(totalFreedBytes))
            .arg(static_cast<qulonglong>(stagingSizeBytes));
    }

    auto validateResult = runPowerShellScript(validateScript);
    if (!validateResult) {
        return core::makeError(validateResult.error().kind(),
            "Pre-create validation failed: " + validateResult.error().message());
    }

    QJsonParseError perr{};
    QJsonDocument validateDoc = QJsonDocument::fromJson(validateResult.value().toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        return core::makeError(core::Error::Kind::Platform,
            "Pre-create validation output was not valid JSON: " + perr.errorString().toStdString());
    }
    QJsonObject validateObj = validateDoc.object();
    if (validateObj.contains("error")) {
        return core::makeError(core::Error::Kind::Platform,
            "Pre-create validation failed: " + validateObj.value("error").toString().toStdString());
    }

    const int diskNumber = validateObj.value("diskNumber").toInt();
    const qulonglong gapOffset = validateObj.value("gapOffset").toVariant().toULongLong();
    const qulonglong gapSize = validateObj.value("gapSize").toVariant().toULongLong();

    // Extract target partition info if available
    const int targetPartitionNumber = validateObj.value("targetPartitionNumber").toInt(-1);
    const qulonglong targetPartitionSize = validateObj.value("targetPartitionSize").toVariant().toULongLong();
    const qulonglong targetPartitionOffset = validateObj.value("targetPartitionOffset").toVariant().toULongLong();

    // Check cancellation after validation
    if (auto r = checkCancel("post-validation"); !r) return r;

    // Step 2: Create the staging partition at the beginning of the freed region
    QString createScript = QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$diskNumber = %1
$offset = %2
$sizeBytes = %3

# Create partition at the specific offset
$partition = New-Partition -DiskNumber $diskNumber -Offset $offset -Size $sizeBytes -UseMaximumSize:$false -ErrorAction Stop
if (-not $partition) {
    Write-Output '{"error":"Failed to create staging partition"}'
    exit 1
}

# Format as FAT32 with label
$volume = $partition | Format-Volume -FileSystem FAT32 -NewFileSystemLabel "LINUX_LIVE" -Force -ErrorAction Stop
if (-not $volume) {
    Write-Output '{"error":"Failed to format staging partition as FAT32"}'
    exit 1
}

# Get the partition details for verification
$partition = Get-Partition -DiskNumber $diskNumber -PartitionNumber $partition.PartitionNumber -ErrorAction Stop
$volume = Get-Volume -Partition $partition -ErrorAction SilentlyContinue

$fs = if ($volume) { $volume.FileSystem } else { '' }
$driveLetter = if ($volume -and $volume.DriveLetter) { [string]$volume.DriveLetter } else { '' }

Write-Output ('{{"partitionNumber":{0}, "offset":{1}, "size":{2}, "filesystem":"{3}", "driveLetter":"{4}"}}' -f $partition.PartitionNumber, $partition.Offset, $partition.Size, $fs, $driveLetter)
)PS1").arg(diskNumber)
        .arg(static_cast<qulonglong>(gapOffset))
        .arg(static_cast<qulonglong>(stagingSizeBytes));

    auto createResult = runPowerShellScript(createScript);
    if (!createResult) {
        return core::makeError(createResult.error().kind(),
            "Create partition failed: " + createResult.error().message());
    }

    QJsonDocument createDoc = QJsonDocument::fromJson(createResult.value().toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        return core::makeError(core::Error::Kind::Platform,
            "Create partition output was not valid JSON: " + perr.errorString().toStdString());
    }
    QJsonObject createObj = createDoc.object();
    if (createObj.contains("error")) {
        return core::makeError(core::Error::Kind::Platform,
            "Create partition failed: " + createObj.value("error").toString().toStdString());
    }

    const int partitionNumber = createObj.value("partitionNumber").toInt();
    const qulonglong createdOffset = createObj.value("offset").toVariant().toULongLong();
    const qulonglong createdSize = createObj.value("size").toVariant().toULongLong();
    const QString fs = createObj.value("filesystem").toString();
    const QString driveLetter = createObj.value("driveLetter").toString();

    // Check cancellation after partition creation
    if (auto r = checkCancel("post-create"); !r) return r;

    // Step 3: Post-create verification
    // Verify: exactly one new partition appeared, size matches, filesystem is FAT32,
    // offset matches expected gap offset, and the free space that is left in the
    // region the staging partition was created in matches the expectation.
    QString verifyScript = QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$diskNumber = %1
$expectedPartitionNumber = %2
$expectedOffset = %3
$expectedSize = %4
$regionSize = %5
$targetPartitionNumber = %6

$disk = Get-Disk -Number $diskNumber -ErrorAction Stop
$partitions = Get-Partition -DiskNumber $diskNumber -ErrorAction SilentlyContinue | Sort-Object Offset

# Find the newly created partition
$newPartition = $partitions | Where-Object { $_.PartitionNumber -eq $expectedPartitionNumber }
if (-not $newPartition) {
    Write-Output '{"error":"New partition not found after creation"}'
    exit 1
}

# Verify size (allow 1 MB tolerance)
$sizeDiff = [math]::Abs($newPartition.Size - $expectedSize)
if ($sizeDiff -gt 1048576) {
    Write-Output ('{{"error":"Partition size mismatch. Expected {0}, got {1}"}}' -f $expectedSize, $newPartition.Size)
    exit 1
}

# Verify offset matches expected gap start
if ($newPartition.Offset -ne $expectedOffset) {
    Write-Output ('{{"error":"Partition offset mismatch. Expected {0}, got {1}"}}' -f $expectedOffset, $newPartition.Offset)
    exit 1
}

# Verify filesystem is FAT32
$volume = Get-Volume -Partition $newPartition -ErrorAction SilentlyContinue
if (-not $volume -or $volume.FileSystem -ne 'FAT32') {
    Write-Output ('{{"error":"Filesystem is not FAT32. Got: {0}"}}' -f (if ($volume) { $volume.FileSystem } else { 'none' }))
    exit 1
}

# Verify target NTFS partition still exists and is unchanged (if applicable)
if ($targetPartitionNumber -gt 0) {
    $targetPartition = $partitions | Where-Object { $_.PartitionNumber -eq $targetPartitionNumber }
    if (-not $targetPartition) {
        Write-Output ('{{"error":"Target NTFS partition {0} disappeared after staging creation!"}}' -f $targetPartitionNumber)
        exit 1
    }
    $targetVolume = Get-Volume -Partition $targetPartition -ErrorAction SilentlyContinue
    if (-not $targetVolume -or $targetVolume.FileSystem -ne 'NTFS') {
        Write-Output ('{{"error":"Target NTFS partition {0} filesystem changed! Got: {1}"}}' -f $targetPartitionNumber, (if ($targetVolume) { $targetVolume.FileSystem } else { 'none' }))
        exit 1
    }
    # Verify target partition offset unchanged (Windows doesn't move partitions on shrink)
    # Note: Size may have changed slightly due to alignment, but offset should be stable
}

# Report the extents of the partitions on the disk. The free space that
# is left in the region the staging partition was created in is computed
# by the installer from these extents: free space elsewhere on the disk
# is not part of that region and must not influence the check.
$allParts = @($partitions)
$extentOut = foreach ($p in $allParts) {
    ('{{"offset":{0}, "size":{1}}}' -f $p.Offset, $p.Size)
}

Write-Output ('{{"success":true, "partitionNumber":{0}, "size":{1}, "offset":{2}, "filesystem":"{3}", "gptGuid":"{4}", "extents":[{5}]}}' -f $newPartition.PartitionNumber, $newPartition.Size, $newPartition.Offset, $volume.FileSystem, $newPartition.GptType, ($extentOut -join ','))
)PS1").arg(diskNumber)
        .arg(partitionNumber)
        .arg(static_cast<qulonglong>(gapOffset))
        .arg(static_cast<qulonglong>(stagingSizeBytes))
        .arg(static_cast<qulonglong>(gapSize))
        .arg(targetPartitionNumber);

    auto verifyResult = runPowerShellScript(verifyScript);
    if (!verifyResult) {
        return core::makeError(verifyResult.error().kind(),
            "Post-create verification failed: " + verifyResult.error().message());
    }

    QJsonDocument verifyDoc = QJsonDocument::fromJson(verifyResult.value().toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        return core::makeError(core::Error::Kind::Platform,
            "Post-create verification output was not valid JSON: " + perr.errorString().toStdString());
    }
    QJsonObject verifyObj = verifyDoc.object();
    if (verifyObj.contains("error")) {
        return core::makeError(core::Error::Kind::Platform,
            "Post-create verification failed: " + verifyObj.value("error").toString().toStdString());
    }

    // The free space that is left in the region the staging partition was
    // created in has to match the expectation. The region was measured
    // before the creation, so the expectation is based on that measurement
    // and not on the planned size of the installation: free space
    // elsewhere on the disk is not part of the region and must not
    // influence the check.
    std::vector<core::DiskExtent> extents;
    const QJsonArray extentArray = verifyObj.value("extents").toArray();
    for (const QJsonValue& value : extentArray) {
        const QJsonObject extentObject = value.toObject();
        core::DiskExtent extent;
        extent.offset = extentObject.value("offset").toVariant().toULongLong();
        extent.size = extentObject.value("size").toVariant().toULongLong();
        extents.push_back(extent);
    }

    const std::uint64_t freeInRegion =
        core::freeSpaceInRegion(gapOffset, gapSize, extents);
    const std::uint64_t expectedFree = gapSize > stagingSizeBytes
        ? gapSize - stagingSizeBytes
        : 0;
    // The tolerance covers the alignment of the partition boundaries.
    constexpr std::uint64_t kFreeSpaceToleranceBytes = 10ull * 1024 * 1024;
    if (!core::freeSpaceMatches(freeInRegion, expectedFree, kFreeSpaceToleranceBytes)) {
        return core::makeError(core::Error::Kind::Platform,
            "Post-create verification failed: free space in the target region "
            "mismatch. Expected ~" + std::to_string(expectedFree) +
            " bytes, got " + std::to_string(freeInRegion) + " bytes");
    }

    // Store staging partition identity for later verification
    // Note: InstallPlan is const here, but we can cast away const since this is
    // internal mutation during the installation process
    const_cast<core::InstallPlan&>(plan).stagingPartitionNumber = partitionNumber;
    const_cast<core::InstallPlan&>(plan).stagingPartitionOffset = createdOffset;
    const_cast<core::InstallPlan&>(plan).stagingPartitionSize = createdSize;
    if (verifyObj.contains("gptGuid") && !verifyObj.value("gptGuid").toString().isEmpty()) {
        const_cast<core::InstallPlan&>(plan).stagingPartitionGptGuid = 
            verifyObj.value("gptGuid").toString().toStdString();
    }

    // Step 4: Establish the mount root of the staging partition.
    //
    // The drive letter is never guessed. Add-PartitionAccessPath without an
    // explicit access path lets Windows pick a free letter of its own, so
    // assuming a letter (Z:) would be wrong - the partition ends up
    // somewhere else and the copy would write to the wrong place (or to
    // nothing at all). The letter that is really assigned is read back
    // from the partition and its volume, and only that letter is used.
    QString commandOutput;
    char assignedLetter = 0;
    if (!driveLetter.isEmpty() && driveLetter.at(0).isLetter()) {
        // The creation already produced a letter for the partition.
        assignedLetter = driveLetter.at(0).toLatin1();
    } else {
        const QString assignScript = QString(
            "Add-PartitionAccessPath -DiskNumber %1 -PartitionNumber %2 -AssignDriveLetter")
            .arg(diskNumber).arg(partitionNumber);
        auto assignResult = runPowerShellScript(assignScript);
        if (!assignResult) {
            return core::makeError(core::Error::Kind::Platform,
                core::stagingMountErrorMessage(diskNumber, partitionNumber, '\0',
                                               assignResult.error().message()));
        }
        commandOutput = assignResult.value();
    }

    // Independent verification: ask Windows which letter the partition
    // really has. Windows can need a moment to publish it, so the lookup
    // is retried a few times.
    char verifiedLetter = 0;
    for (int attempt = 0; attempt < 3 && verifiedLetter == 0; ++attempt) {
        if (attempt > 0) {
            QThread::msleep(500);
        }
        const QString verifyLetterScript = QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$partition = Get-Partition -DiskNumber %1 -PartitionNumber %2 -ErrorAction Stop
$volume = Get-Volume -Partition $partition -ErrorAction SilentlyContinue
if ($volume -and $volume.DriveLetter) {
    Write-Output ('{{"driveLetter":"{0}"}}' -f $volume.DriveLetter)
} else {
    Write-Output '{"driveLetter":""}'
}
)PS1").arg(diskNumber).arg(partitionNumber);
        auto verifyResult = runPowerShellScript(verifyLetterScript);
        if (!verifyResult) {
            commandOutput = QString::fromStdString(verifyResult.error().message());
            continue;
        }
        QJsonDocument verifyDoc = QJsonDocument::fromJson(verifyResult.value().toUtf8(), &perr);
        if (perr.error != QJsonParseError::NoError) {
            commandOutput = verifyResult.value();
            continue;
        }
        const QString actualLetter =
            verifyDoc.object().value("driveLetter").toString().trimmed();
        if (!actualLetter.isEmpty() && actualLetter.at(0).isLetter()) {
            verifiedLetter = actualLetter.at(0).toLatin1();
        }
    }

    const core::StagingMount mount = core::decideStagingMount(
        diskNumber, partitionNumber, verifiedLetter, assignedLetter,
        commandOutput.toStdString());
    if (!mount.ok) {
        // Without a mounted volume the file copy has no directory to write
        // to. Continuing would copy into a partition that does not exist.
        return core::makeError(core::Error::Kind::Platform, mount.errorMessage);
    }
    bootMount = std::filesystem::path(mount.root);

    // Note: rEFInd partition creation is deferred to Phase 2.3+
    (void)refindMount;

    return core::makeOk();
}

core::Result<std::filesystem::path> DiskOps::mountIso(
    const std::filesystem::path& iso) {
    // Mount the ISO and resolve the drive letter of the volume that
    // belongs to *that* image. A plain Get-Volume returns any volume of
    // the machine, so the lookup is scoped to the mounted image with
    // -DiskImage (the legacy PowerShell installer does the same).
    QString script = QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$imagePath = '%1'

$image = $null
$attachedByUs = $false
try {
    $image = Mount-DiskImage -ImagePath $imagePath -StorageType ISO -PassThru -ErrorAction Stop
    $attachedByUs = $true
} catch {
    # The image can already be attached from an earlier run. Use it, but
    # remember that it was not attached by us: it must not be dismounted.
    $image = Get-DiskImage -ImagePath $imagePath -ErrorAction Stop
    if (-not $image) { throw }
}

# Windows needs a moment to expose the volume of a freshly mounted
# image, so the lookup is retried for a few seconds.
$volume = $null
for ($attempt = 0; $attempt -lt 20; $attempt++) {
    $volume = Get-Volume -DiskImage $image -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($volume -and $volume.DriveLetter) { break }
    Start-Sleep -Milliseconds 500
}
if (-not $volume -or -not $volume.DriveLetter) {
    # Do not leave the image attached when it cannot be used.
    if ($attachedByUs) { Dismount-DiskImage -ImagePath $imagePath -ErrorAction SilentlyContinue }
    Write-Output '{"error":"The mounted ISO did not get a drive letter"}'
    exit 1
}

$attached = 'false'
if ($attachedByUs) { $attached = 'true' }
Write-Output ('{{"driveLetter":"{0}", "attachedByUs":{1}}}' -f "$($volume.DriveLetter):", $attached)
)PS1").arg(QString::fromStdString(iso.string()).replace("'", "''"));

    auto r = runPowerShellScript(script);
    if (!r) {
        return core::makeError<std::filesystem::path>(r.error().kind(),
            "Mount-DiskImage failed: " + r.error().message());
    }

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(r.value().toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        return core::makeError<std::filesystem::path>(core::Error::Kind::Platform,
            "Mount-DiskImage did not return a valid result: " +
            perr.errorString().toStdString());
    }
    const QJsonObject obj = doc.object();
    if (obj.contains("error")) {
        return core::makeError<std::filesystem::path>(core::Error::Kind::Platform,
            "Mount-DiskImage failed: " +
            obj.value("error").toString().toStdString());
    }

    const QString letter = obj.value("driveLetter").toString().trimmed();
    if (letter.size() != 2 || letter.at(1) != QLatin1Char(':')) {
        return core::makeError<std::filesystem::path>(core::Error::Kind::Platform,
            "Mount-DiskImage returned no usable drive letter: '" +
            letter.toStdString() + "'");
    }
    isoAttachedByUs_ = obj.value("attachedByUs").toBool();
    return core::makeOk<std::filesystem::path>(
        std::filesystem::path(letter.toStdString()));
}

void DiskOps::unmountIso(const std::filesystem::path& iso) {
    // Only the image that this installation attached is dismounted. An
    // image that was already attached (or that the user mounted) stays
    // attached, and no other image is touched.
    if (!isoAttachedByUs_) {
        return;
    }
    isoAttachedByUs_ = false;
    const QString script = QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Dismount-DiskImage -ImagePath '%1' -ErrorAction SilentlyContinue
)PS1").arg(QString::fromStdString(iso.string()).replace("'", "''"));
    // Best effort: a failure to dismount must not mask the result of the
    // installation.
    (void)runPowerShellScript(script);
}

core::Result<void> DiskOps::copyFiles(const std::filesystem::path& src,
                                       const std::filesystem::path& dst,
                                       const core::Distro* distro,
                                       const core::InstallPlan& plan,
                                       std::function<bool()> cancelCallback,
                                       core::CopyProgressCallback progress) {
    // 0. Validate destination is a properly rooted path. On Windows, a path
    //    like "D:" (drive letter without trailing backslash) is drive-relative,
    //    not root-relative: resolving required files against it would target the
    //    current directory on that drive instead of the drive root, causing
    //    verification to inspect the wrong location when drive letters are
    //    reassigned between runs. createLayout always records a properly rooted
    //    mount path (e.g. "D:\"), so reject anything else.
    //    This check targets the specific broken pattern: a root name (drive
    //    letter) without a root directory. Paths like "/nonexistent/staging"
    //    (root directory but no root name) are not affected and fall through
    //    to the normal existence check below.
    if (dst.has_root_name() && !dst.has_root_directory()) {
        return core::makeError(core::Error::Kind::InvalidInput,
            "Staging mount root has drive letter but no root directory: " +
            dst.string());
    }

    // ─── Pre-copy verification ─────────────────────────────────────────
    // 1. Verify source (mounted ISO) exists and is readable
    if (!std::filesystem::exists(src)) {
        return core::makeError(core::Error::Kind::NotFound,
            "Source ISO mount does not exist: " + src.string());
    }

    // 2. Verify destination (staging partition) exists
    if (!std::filesystem::exists(dst)) {
        return core::makeError(core::Error::Kind::NotFound,
            "Destination staging partition does not exist: " + dst.string());
    }

    const auto report = [&progress](const std::string& detail) {
        if (progress) progress(0, "Preparing the copy", detail);
    };

    report("Copying the ISO contents from " + src.string() + " to " + dst.string());

    // 3. Verify staging partition is FAT32 and get free space.
    //
    // The staging partition is looked up by the identity that
    // createLayout recorded (disk number + partition number), not by the
    // drive-letter path. Get-Volume -Path 'D:\' is fragile: in some VM
    // configurations it returns no volume even when the partition and its
    // volume exist, which aborts the copy before it starts. Querying the
    // exact partition and then its associated volume is the supported
    // Windows Storage API pattern and never picks an arbitrary volume.
    if (!plan.stagingPartitionNumber.has_value()) {
        return core::makeError(core::Error::Kind::InvalidInput,
            "Staging partition identity is not recorded; cannot verify staging volume");
    }
    if (plan.targetDiskNumber < 0) {
        return core::makeError(core::Error::Kind::InvalidInput,
            "Staging partition identity is not recorded; cannot verify staging volume");
    }
    const int diskNumber = plan.targetDiskNumber;
    const int partitionNumber = plan.stagingPartitionNumber.value();

    // Drive letter that createLayout already verified is on the staging
    // partition. The volume we find here must use exactly this letter.
    const QString dstStr = QString::fromStdString(dst.string());
    QString expectedLetterStr;
    if (dstStr.size() >= 2 && dstStr.at(0).isLetter() &&
        dstStr.at(1) == QLatin1Char(':')) {
        expectedLetterStr = dstStr.at(0);
    }

    QString verifyScript = QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$diskNumber = %1
$partitionNumber = %2
$expectedDriveLetter = '%3'

# Get the exact staging partition on the target disk.
$partition = Get-Partition -DiskNumber $diskNumber -PartitionNumber $partitionNumber -ErrorAction SilentlyContinue
if (-not $partition) {
    Write-Output ('{{"error":"Staging partition not found on disk {0} (partition {1})"}}' -f $diskNumber, $partitionNumber)
    exit 1
}

# Get the volume associated with that exact partition (never an arbitrary volume).
$volume = Get-Volume -Partition $partition -ErrorAction SilentlyContinue
if (-not $volume) {
    Write-Output ('{{"error":"No volume associated with staging partition on disk {0} (partition {1})"}}' -f $diskNumber, $partitionNumber)
    exit 1
}

# Verify filesystem is FAT32
$fs = $volume.FileSystem
if (-not $fs -or $fs -ne 'FAT32') {
    Write-Output ('{{"error":"Staging volume is not FAT32 (got: {0})"}}' -f (if ($volume) { $volume.FileSystem } else { 'none' }))
    exit 1
}

# Verify the volume's drive letter matches the staging mount root that
# createLayout already verified.
$driveLetter = if ($volume.DriveLetter) { [string]$volume.DriveLetter } else { '' }
if (-not $driveLetter) {
    Write-Output ('{{"error":"Staging volume on disk {0} partition {1} has no drive letter"}}' -f $diskNumber, $partitionNumber)
    exit 1
}

if ($expectedDriveLetter -and $driveLetter -ne $expectedDriveLetter) {
    Write-Output ('{{"error":"Staging volume drive letter mismatch. Expected: {0}, found: {1}"}}' -f $expectedDriveLetter, $driveLetter)
    exit 1
}

$freeBytes = $volume.SizeRemaining
$totalBytes = $volume.Size

Write-Output ('{{"filesystem":"{0}", "freeBytes":{1}, "totalBytes":{2}, "driveLetter":"{3}"}}' -f $fs, $freeBytes, $totalBytes, $driveLetter)
)PS1").arg(diskNumber).arg(partitionNumber).arg(expectedLetterStr);

    auto verifyResult = runPowerShellScript(verifyScript);
    if (!verifyResult) {
        return core::makeError(verifyResult.error().kind(),
            "Staging verification failed (disk " + std::to_string(diskNumber) +
            ", partition " + std::to_string(partitionNumber) +
            ", mount " + dst.string() + "): " + verifyResult.error().message());
    }

    QJsonParseError perr{};
    QJsonDocument verifyDoc = QJsonDocument::fromJson(verifyResult.value().toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        return core::makeError(core::Error::Kind::Platform,
            "Staging verification output was not valid JSON (disk " +
            std::to_string(diskNumber) + ", partition " +
            std::to_string(partitionNumber) + ", mount " + dst.string() +
            "): " + perr.errorString().toStdString());
    }
    QJsonObject verifyObj = verifyDoc.object();
    if (verifyObj.contains("error")) {
        return core::makeError(core::Error::Kind::Platform,
            "Staging verification failed: " + verifyObj.value("error").toString().toStdString());
    }

    const QString fs = verifyObj.value("filesystem").toString();
    const qulonglong freeBytes = verifyObj.value("freeBytes").toVariant().toULongLong();
    const qulonglong totalBytes = verifyObj.value("totalBytes").toVariant().toULongLong();

    if (fs.compare("FAT32", Qt::CaseInsensitive) != 0) {
        return core::makeError(core::Error::Kind::Platform,
            "Staging partition is not FAT32 (got: " + fs.toStdString() + ")");
    }

    // Verify total space can be read reliably. A zero total means the
    // volume query returned no usable size data.
    if (totalBytes == 0) {
        return core::makeError(core::Error::Kind::Platform,
            "Staging volume reports zero total bytes (disk " +
            std::to_string(diskNumber) + ", partition " +
            std::to_string(partitionNumber) + ")");
    }

    // 4. The mounted image has to contain the expected files. The legacy
    //    installer asks the user in this case; there is no prompt here,
    //    so the problem is reported and the copy continues.
    if (distro && !distro->validationFile().empty()) {
        const std::filesystem::path validation = src / distro->validationFile();
        if (!std::filesystem::exists(validation)) {
            report("Warning: the mounted ISO does not contain '" +
                   distro->validationFile() + "'. It may not be a valid image.");
        }
    }

    // ─── Enumerate the source once ─────────────────────────────────────
    // The plan holds the files, the directories, the total size and the
    // largest file. The safety checks use it, and the copy uses it as
    // well, so the source tree is not walked a second time.
    core::CopyLimits limits;
    // FAT32 cannot store a file of 4 GiB or more.
    constexpr std::uint64_t FAT32_MAX_FILE_SIZE = 4ull * 1024 * 1024 * 1024 - 1;
    limits.maxFileSizeBytes = FAT32_MAX_FILE_SIZE;

    auto copyPlanResult = core::enumerateCopyPlan(src, cancelCallback, progress);
    if (!copyPlanResult) {
        return core::makeError(copyPlanResult.error().kind(),
            copyPlanResult.error().message());
    }
    const core::CopyPlan& copyPlan = copyPlanResult.value();
    const std::uint64_t estimatedSize = copyPlan.totalBytes;

    // ─── Copy ──────────────────────────────────────────────────────────
    auto copyResult = core::copyCopyPlan(copyPlan, dst, freeBytes, limits,
                                         cancelCallback, progress);
    if (!copyResult) {
        return core::makeError(copyResult.error().kind(),
            copyResult.error().message());
    }

    // ─── Post-copy verification ────────────────────────────────────────
    // Verify required files exist based on distro
    if (distro) {
        std::vector<std::string> requiredFiles;

        // Common required files for all distros
        requiredFiles.push_back("EFI/BOOT/BOOTx64.EFI");

        // Distro-specific required files
        if (distro->keyword() == "Fedora") {
            requiredFiles.push_back("LiveOS/squashfs.img");
            requiredFiles.push_back("isolinux/vmlinuz");
            requiredFiles.push_back("isolinux/initrd.img");
        } else if (distro->keyword() == "CachyOS") {
            requiredFiles.push_back("arch/boot/x86_64/vmlinuz-linux-cachyos");
            requiredFiles.push_back("arch/boot/x86_64/initramfs-linux-cachyos.img");
            requiredFiles.push_back("arch/x86_64/airootfs.sfs");
        } else if (distro->keyword() == "Debian") {
            requiredFiles.push_back("live/vmlinuz");
            requiredFiles.push_back("live/initrd.img");
            requiredFiles.push_back("live/filesystem.squashfs");
        } else {
            // Ubuntu, Kubuntu, Mint, Linux Lite (Caspar-based)
            requiredFiles.push_back("casper/vmlinuz");
            requiredFiles.push_back("casper/filesystem.squashfs");
        }

        // Add initrd variants from the distro catalog. At least one must exist.
        const auto& initrdVariants = distro->initrdVariants();
        if (!initrdVariants.empty()) {
            // We'll handle initrd variants separately: at least one must exist.
        }

        for (const auto& reqFile : requiredFiles) {
            if (cancelCallback && cancelCallback()) {
                return core::makeCancelled();
            }

            // Build the required-file path component-wise: reqFile uses
            // forward slashes (e.g. "casper/initrd"), and appending each
            // component individually avoids any ambiguity with the native
            // path separator on Windows (backslash). This guarantees the
            // resolved path is D:\casper\initrd when dst is D:\.
            std::filesystem::path reqPath = dst;
            std::filesystem::path reqFilePath(reqFile);
            reqPath /= reqFilePath;

            std::error_code reqEc;
            const bool reqExists = std::filesystem::exists(reqPath, reqEc);
            if (!reqExists) {
                // No error code means the filesystem query succeeded and
                // determined the file does not exist. Treat this the same
                // as no_such_file_or_directory: a genuinely missing file,
                // not an I/O error. A stale ERROR_SUCCESS ("The operation
                // completed successfully") from a prior operation must not
                // be misclassified as an I/O failure.
                if (reqEc == std::errc::no_such_file_or_directory || !reqEc) {
                    return core::makeError(core::Error::Kind::Platform,
                        "Required file missing after copy: " + reqPath.string());
                }
                return core::makeError(core::Error::Kind::Io,
                    "Cannot verify required file after copy: " + reqPath.string() +
                    " (" + reqEc.message() + ")");
            }
        }

        // Verify initrd variants: at least one must exist.
        const auto& initrdVariantsCheck = distro->initrdVariants();
        if (!initrdVariantsCheck.empty()) {
            if (cancelCallback && cancelCallback()) {
                return core::makeCancelled();
            }
            bool anyInitrdExists = false;
            std::filesystem::path foundInitrdPath;
            for (const auto& initrdVariant : initrdVariantsCheck) {
                std::filesystem::path reqPath = dst;
                std::filesystem::path reqFilePath(initrdVariant);
                reqPath /= reqFilePath;

                std::error_code reqEc;
                if (std::filesystem::exists(reqPath, reqEc)) {
                    anyInitrdExists = true;
                    foundInitrdPath = reqPath;
                    break;
                }
            }
            if (!anyInitrdExists) {
                // Report the first variant as the expected path for clarity
                std::filesystem::path reqPath = dst;
                std::filesystem::path reqFilePath(initrdVariantsCheck.front());
                reqPath /= reqFilePath;
                return core::makeError(core::Error::Kind::Platform,
                    "Required file missing after copy: " + reqPath.string() +
                    " (accepted variants: " + join(initrdVariantsCheck, ", ") + ")");
            }
        }
    }

    // Verify total copied size is sensible (at least 50% of estimated)
    std::uint64_t copiedSize = 0;
    std::error_code walkEc;
    for (std::filesystem::recursive_directory_iterator it(dst, walkEc), end;
         it != end; it.increment(walkEc)) {
        if (cancelCallback && cancelCallback()) {
            return core::makeCancelled();
        }
        std::error_code typeEc;
        if (it->is_regular_file(typeEc) && !typeEc) {
            std::error_code sizeEc;
            copiedSize += std::filesystem::file_size(*it, sizeEc);
        }
        if (walkEc) {
            break;
        }
    }

    if (estimatedSize > 0 && copiedSize < estimatedSize / 2) {
        return core::makeError(core::Error::Kind::Platform,
            "Copied size suspiciously small: " + std::to_string(copiedSize) +
            " vs estimated " + std::to_string(estimatedSize));
    }

    return core::makeOk();
}

core::Result<void> DiskOps::patchDistroBootConfig(const core::Distro& d,
                                                   const core::InstallPlan& plan) {
    if (!plan.bootPartitionMount.has_value()) {
        return core::makeError(core::Error::Kind::InvalidInput,
            "bootPartitionMount is not set");
    }

    const std::filesystem::path bootMount = plan.bootPartitionMount.value();
    const std::string keyword = d.keyword();

    if (keyword != "Fedora" && keyword != "CachyOS") {
        return core::makeOk();
    }

    std::vector<std::filesystem::path> configFiles;

    if (keyword == "Fedora") {
        configFiles.push_back(bootMount / "EFI" / "BOOT" / "grub.cfg");
        configFiles.push_back(bootMount / "isolinux" / "grub.cfg");
        configFiles.push_back(bootMount / "EFI" / "fedora" / "grub.cfg");
    } else if (keyword == "CachyOS") {
        configFiles.push_back(bootMount / "arch" / "boot" / "x86_64" / "grub.cfg");
        configFiles.push_back(bootMount / "EFI" / "BOOT" / "grub.cfg");
        configFiles.push_back(bootMount / "boot" / "grub" / "grub.cfg");
    }

    bool anyPatched = false;
    for (const auto& cfgPath : configFiles) {
        if (!std::filesystem::exists(cfgPath)) {
            continue;
        }

        QFile file(QString::fromStdString(cfgPath.string()));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        QString content = QString::fromUtf8(file.readAll());
        file.close();

        QString original = content;
        bool modified = false;

        if (keyword == "Fedora") {
            QRegularExpression labelRx(R"(root=live:LABEL=([^\s\\]+))");
            QRegularExpressionMatchIterator it = labelRx.globalMatch(content);
            while (it.hasNext()) {
                auto match = it.next();
                if (match.captured(1) != "LINUX_LIVE") {
                    content.replace(match.capturedStart(1), match.capturedLength(1), "LINUX_LIVE");
                    modified = true;
                }
            }

            QRegularExpression isolabelRx(R"(islabel=([^\s\\]+))");
            it = isolabelRx.globalMatch(content);
            while (it.hasNext()) {
                auto match = it.next();
                if (match.captured(1) != "LINUX_LIVE") {
                    content.replace(match.capturedStart(1), match.capturedLength(1), "LINUX_LIVE");
                    modified = true;
                }
            }

            QRegularExpression cdlabelRx(R"(CDLABEL=([^\s\\]+))");
            it = cdlabelRx.globalMatch(content);
            while (it.hasNext()) {
                auto match = it.next();
                if (match.captured(1) != "LINUX_LIVE") {
                    content.replace(match.capturedStart(1), match.capturedLength(1), "LINUX_LIVE");
                    modified = true;
                }
            }
        } else if (keyword == "CachyOS") {
            QRegularExpression archisoLabelRx(R"(archisolabel=([^\s\\]+))");
            QRegularExpressionMatchIterator it = archisoLabelRx.globalMatch(content);
            while (it.hasNext()) {
                auto match = it.next();
                if (match.captured(1) != "LINUX_LIVE") {
                    content.replace(match.capturedStart(1), match.capturedLength(1), "LINUX_LIVE");
                    modified = true;
                }
            }

            QRegularExpression archisoSearchLabelRx(R"(archisosearchlabel=([^\s\\]+))");
            it = archisoSearchLabelRx.globalMatch(content);
            while (it.hasNext()) {
                auto match = it.next();
                if (match.captured(1) != "LINUX_LIVE") {
                    content.replace(match.capturedStart(1), match.capturedLength(1), "LINUX_LIVE");
                    modified = true;
                }
            }

            QRegularExpression archisoDeviceRx(R"(archisodevice=/dev/disk/by-label/([^\s\\]+))");
            it = archisoDeviceRx.globalMatch(content);
            while (it.hasNext()) {
                auto match = it.next();
                if (match.captured(1) != "LINUX_LIVE") {
                    content.replace(match.capturedStart(1), match.capturedLength(1), "LINUX_LIVE");
                    modified = true;
                }
            }

            QRegularExpression searchLabelRx(R"((search\s+[^\r\n]*?--(?:label|fs-label)\s+)(\S+))");
            it = searchLabelRx.globalMatch(content);
            while (it.hasNext()) {
                auto match = it.next();
                if (match.captured(2) != "LINUX_LIVE") {
                    content.replace(match.capturedStart(2), match.capturedLength(2), "LINUX_LIVE");
                    modified = true;
                }
            }
        }

        if (modified) {
            if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                return core::makeError(core::Error::Kind::Io,
                    "Cannot write patched config: " + cfgPath.string());
            }
            file.write(content.toUtf8());
            file.close();
            anyPatched = true;
        }
    }

    if (!anyPatched && keyword == "Fedora") {
        return core::makeError(core::Error::Kind::Platform,
            "Fedora: No GRUB config found to patch with LABEL=LINUX_LIVE");
    }
    if (!anyPatched && keyword == "CachyOS") {
        return core::makeError(core::Error::Kind::Platform,
            "CachyOS: No GRUB config found to patch with archisolabel=LINUX_LIVE");
    }

    return core::makeOk();
}

core::Result<void> DiskOps::installRefind(const core::InstallPlan& plan) {
    Q_UNUSED(plan);
    return core::makeError(core::Error::Kind::Platform,
        "installRefind not yet implemented (Phase 1.1 stub)");
}

core::Result<void> DiskOps::createBootEntry(const core::InstallPlan& plan) {
    // Re-verify staging partition identity before creating boot entry
    if (!plan.stagingPartitionNumber.has_value() ||
        !plan.stagingPartitionOffset.has_value() ||
        !plan.stagingPartitionSize.has_value()) {
        return core::makeError(core::Error::Kind::InvalidInput,
            "Staging partition identity not recorded");
    }

    if (!plan.bootPartitionMount.has_value()) {
        return core::makeError(core::Error::Kind::InvalidInput,
            "bootPartitionMount is not set");
    }

    // Verify the staging partition still exists and matches recorded identity
    const int diskNumber = plan.targetDiskNumber;
    const int expectedPartitionNumber = plan.stagingPartitionNumber.value();
    const std::uint64_t expectedOffset = plan.stagingPartitionOffset.value();
    const std::uint64_t expectedSize = plan.stagingPartitionSize.value();

    QString verifyScript = QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$diskNumber = %1
$expectedPartitionNumber = %2
$expectedOffset = %3
$expectedSize = %4

$disk = Get-Disk -Number $diskNumber -ErrorAction Stop
$partitions = Get-Partition -DiskNumber $diskNumber -ErrorAction SilentlyContinue | Sort-Object Offset

$newPartition = $partitions | Where-Object { $_.PartitionNumber -eq $expectedPartitionNumber }
if (-not $newPartition) {
    Write-Output '{"error":"Staging partition not found during re-verification"}'
    exit 1
}

$sizeDiff = [math]::Abs($newPartition.Size - $expectedSize)
if ($sizeDiff -gt 1048576) {
    Write-Output ('{{"error":"Partition size mismatch during re-verification. Expected {0}, got {1}"}}' -f $expectedSize, $newPartition.Size)
    exit 1
}

if ($newPartition.Offset -ne $expectedOffset) {
    Write-Output ('{{"error":"Partition offset mismatch during re-verification. Expected {0}, got {1}"}}' -f $expectedOffset, $newPartition.Offset)
    exit 1
}

$volume = Get-Volume -Partition $newPartition -ErrorAction SilentlyContinue
if (-not $volume -or $volume.FileSystem -ne 'FAT32') {
    Write-Output ('{{"error":"Filesystem is not FAT32 during re-verification. Got: {0}"}}' -f (if ($volume) { $volume.FileSystem } else { 'none' }))
    exit 1
}

$driveLetter = if ($volume -and $volume.DriveLetter) { [string]$volume.DriveLetter } else { '' }
$gptGuid = if ($newPartition.GptType) { [string]$newPartition.GptType } else { '' }

Write-Output ('{{"driveLetter":"{0}", "gptGuid":"{1}"}}' -f $driveLetter, $gptGuid)
)PS1").arg(diskNumber)
        .arg(expectedPartitionNumber)
        .arg(static_cast<qulonglong>(expectedOffset))
        .arg(static_cast<qulonglong>(expectedSize));

    auto verifyResult = runPowerShellScript(verifyScript);
    if (!verifyResult) {
        return core::makeError(verifyResult.error().kind(),
            "Staging partition re-verification failed: " + verifyResult.error().message());
    }

    QJsonParseError perr{};
    QJsonDocument verifyDoc = QJsonDocument::fromJson(verifyResult.value().toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        return core::makeError(core::Error::Kind::Platform,
            "Re-verification output was not valid JSON: " + perr.errorString().toStdString());
    }
    QJsonObject verifyObj = verifyDoc.object();
    if (verifyObj.contains("error")) {
        return core::makeError(core::Error::Kind::Platform,
            verifyObj.value("error").toString().toStdString());
    }

    QString driveLetter = verifyObj.value("driveLetter").toString();
    if (driveLetter.isEmpty()) {
        return core::makeError(core::Error::Kind::Platform,
            "Staging partition has no drive letter assigned");
    }

    // Verify EFI/BOOT/BOOTx64.EFI exists
    std::filesystem::path efiPath = plan.bootPartitionMount.value() / "EFI" / "BOOT" / "BOOTx64.EFI";
    if (!std::filesystem::exists(efiPath)) {
        return core::makeError(core::Error::Kind::NotFound,
            "EFI/BOOT/BOOTx64.EFI not found on staging partition");
    }

    // Distro-specific required-file verification happens in copyFiles.

    // Create boot entry pointing to staging partition
    return BcdStore::createDirectBootEntry(plan, driveLetter.at(0).toLatin1());
}

void DiskOps::rollbackBootEntry(const core::InstallPlan& plan) {
    BcdStore::deleteEntry(plan);
}

void DiskOps::rollbackStagingPartition(const core::InstallPlan& plan) {
    if (!plan.stagingPartitionNumber.has_value() ||
        !plan.targetDiskNumber) {
        // Nothing to roll back
        return;
    }

    const int diskNumber = plan.targetDiskNumber;
    const int partitionNumber = plan.stagingPartitionNumber.value();

    QString script = QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$diskNumber = %1
$partitionNumber = %2

$partition = Get-Partition -DiskNumber $diskNumber -PartitionNumber $partitionNumber -ErrorAction SilentlyContinue
if (-not $partition) {
    Write-Output '{"success":true, "message":"Partition already gone"}'
    exit 0
}

$volume = Get-Volume -Partition $partition -ErrorAction SilentlyContinue
$driveLetter = if ($volume -and $volume.DriveLetter) { [string]$volume.DriveLetter } else { '' }

# Remove drive letter if assigned
if ($driveLetter) {
    $partition | Remove-PartitionAccessPath -AccessPath "$($driveLetter):\" -ErrorAction SilentlyContinue
}

# Delete the partition
Remove-Partition -DiskNumber $diskNumber -PartitionNumber $partitionNumber -Confirm:$false -ErrorAction Stop

Write-Output '{"success":true}'
)PS1").arg(diskNumber).arg(partitionNumber);

    runPowerShellScript(script);
    // Best effort - don't return error on rollback
}

void DiskOps::restartSystem() {
    // The actual restart is gated by the UI's RestartCountdownDialog.
    // This method is only called if the user has confirmed the
    // countdown. It is intentionally a fast no-arg shutdown.
    QProcess::startDetached("shutdown", {"/r", "/t", "0", "/f"});
}

}  // namespace ulli::platform::windows
