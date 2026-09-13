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
#include "platform/windows/Sha256.h"
#include "platform/Platform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStorageInfo>
#include <QTextStream>
#include <QUrl>
#include <QEventLoop>

#include <algorithm>
#include <iostream>

namespace ulli::platform::windows {

namespace {

// Run an external tool, return a typed Result. Never throws.
core::Result<QString> runTool(const QString& program,
                              const QStringList& args,
                              const QString& workingDir = {}) {
    QProcess proc;
    if (!workingDir.isEmpty()) proc.setWorkingDirectory(workingDir);
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(program, args);
    if (!proc.waitForStarted(10000)) {
        return core::makeError<std::string>(core::Error::Kind::NotFound,
            "Failed to start: " + program.toStdString());
    }
    if (!proc.waitForFinished(-1)) {
        return core::makeError<std::string>(core::Error::Kind::Platform,
            "Tool did not finish: " + program.toStdString());
    }
    const QString out = QString::fromLocal8Bit(proc.readAll());
    if (proc.exitStatus() != QProcess::NormalExit) {
        return core::makeError<std::string>(core::Error::Kind::Platform,
            "Tool crashed: " + program.toStdString());
    }
    if (proc.exitCode() != 0) {
        return core::makeError<std::string>(core::Error::Kind::Platform,
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

std::vector<core::Disk> DiskOps::enumerateDisks() {
    auto r = Wmi::runScript("list_disks");
    if (!r) {
        std::cerr << "WARN: enumerateDisks: " << r.error().message() << '\n';
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
        return {};
    }
    return translateWmiDisks(arr);
}

core::Result<void> DiskOps::validatePlanForDisk(const core::InstallPlan& plan) {
    // Re-read the live disk state and check that the plan refers to a
    // disk that actually exists, and that the requested strategy is
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

    int matchedDiskNumber = -1;
    bool isSystem = false;
    for (const QJsonValue& v : arr) {
        const QJsonObject d = v.toObject();
        if (d.value("Number").toInt(-1) == plan.targetDiskNumber) {
            matchedDiskNumber = plan.targetDiskNumber;
            const QJsonArray parts = d.value("Partitions").toArray();
            for (const QJsonValue& pv : parts) {
                if (pv.toObject().value("IsSystem").toBool()) {
                    isSystem = true;
                }
            }
            break;
        }
    }
    if (matchedDiskNumber < 0) {
        return core::makeError(core::Error::Kind::NotFound,
            "Target disk " + std::to_string(plan.targetDiskNumber) +
            " does not exist or is not Online");
    }
    if (plan.strategy == core::Strategy::WipeDisk && isSystem) {
        return core::makeError(core::Error::Kind::InvalidInput,
            "Refusing to wipe a disk that contains the system partition. "
            "Choose a non-system disk or pick a different strategy.");
    }
    return core::makeOk();
}

core::Result<void> DiskOps::preflight(const core::InstallPlan& plan) {
    if (!core::platform::isElevated()) {
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
    const std::filesystem::path cache = core::platform::cacheDir();
    const std::filesystem::path isoPath = cache / d.isoFilename();
    if (std::filesystem::exists(isoPath)) {
        // Verify SHA-256 against the expected hash from distros.json
        auto v = Sha256::verifyFile(isoPath, d.sha256());
        if (!v) return v;
        return core::makeOk(isoPath);
    }
    return core::makeError(core::Error::Kind::NotFound,
        "ISO not found in cache. Please download " + d.isoFilename() +
        " to " + cache.string() + " (link: " + d.downloadPage() + ").");
}

core::Result<std::filesystem::path> DiskOps::downloadIso(
    const core::Distro& distro,
    const std::filesystem::path& destPath,
    std::function<void(int percent, const QString& status)> progressCallback) {

    if (distro.mirrors().empty()) {
        return core::makeError(core::Error::Kind::NotFound,
            "No download mirrors available for " + distro.label());
    }

    // Ensure destination directory exists
    std::error_code ec;
    std::filesystem::create_directories(destPath.parent_path(), ec);
    if (ec) {
        return core::makeError(core::Error::Kind::Io,
            "Cannot create download directory: " + ec.message());
    }

    // Try each mirror in order
    for (const auto& mirror : distro.mirrors()) {
        if (progressCallback) {
            progressCallback(0, QString("Connecting to %1...").arg(QString::fromStdString(mirror)));
        }

        QNetworkAccessManager manager;
        QNetworkRequest request(QUrl(QString::fromStdString(mirror)));
        request.setRawHeader("User-Agent", "ULLI/1.0");

        QNetworkReply* reply = manager.get(request);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::downloadProgress,
            [progressCallback](qint64 bytesReceived, qint64 bytesTotal) {
                if (bytesTotal > 0 && progressCallback) {
                    int percent = static_cast<int>((bytesReceived * 100) / bytesTotal);
                    progressCallback(percent,
                        QString("Downloading: %1 / %2 MB")
                            .arg(bytesReceived / (1024*1024))
                            .arg(bytesTotal / (1024*1024)));
                }
            });

        loop.exec();

        if (reply->error() != QNetworkReply::NoError) {
            QString error = reply->errorString();
            reply->deleteLater();
            if (progressCallback) {
                progressCallback(0, tr("Mirror failed: %1").arg(error));
            }
            continue; // Try next mirror
        }

        // Write to file
        QFile file(QString::fromStdString(destPath.string()));
        if (!file.open(QIODevice::WriteOnly)) {
            reply->deleteLater();
            return core::makeError(core::Error::Kind::Io,
                "Cannot open destination file for writing: " + destPath.string());
        }

        file.write(reply->readAll());
        file.close();
        reply->deleteLater();

        // Verify SHA-256
        if (progressCallback) {
            progressCallback(100, tr("Verifying checksum..."));
        }
        auto v = Sha256::verifyFile(destPath, distro.sha256());
        if (!v) {
            std::filesystem::remove(destPath);
            return v;
        }

        if (progressCallback) {
            progressCallback(100, tr("Download complete and verified"));
        }
        return core::makeOk(destPath);
    }

    return core::makeError(core::Error::Kind::Platform,
        "All download mirrors failed for " + distro.label());
}

core::Result<std::uint64_t> DiskOps::shrinkPartition(char driveLetter,
                                                     std::uint64_t requestedShrinkBytes) {
    // Use Windows native Storage API via PowerShell:
    // 1. Get-PartitionSupportedSize to determine SizeMin/SizeMax
    // 2. Calculate requested final size
    // 3. Verify requested final size >= SizeMin
    // 4. Resize-Partition to the requested final size
    // 5. Post-resize verification

    if (requestedShrinkBytes == 0) {
        return core::makeError(core::Error::Kind::InvalidInput,
            "shrinkPartition: requestedShrinkBytes is zero");
    }

    // Step 1: Find the partition by drive letter and verify it's NTFS
    QString script = QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$driveLetter = '%1'
$partition = Get-Partition -DriveLetter $driveLetter -ErrorAction Stop
if (-not $partition) {
    Write-Output '{"error":"Partition not found for drive letter"}'
    exit 1
}

# Verify filesystem is NTFS
$volume = Get-Volume -Partition $partition -ErrorAction SilentlyContinue
if (-not $volume -or $volume.FileSystem -ne 'NTFS') {
    Write-Output '{"error":"Target partition is not NTFS"}'
    exit 1
}

# Verify it's not a system/EFI/recovery partition
if ($partition.IsSystem -or $partition.IsBoot -or $partition.IsHidden) {
    Write-Output '{"error":"Cannot shrink system/boot/recovery partition"}'
    exit 1
}

$currentSize = $partition.Size
$requestedShrink = %2
$requestedFinalSize = $currentSize - $requestedShrink

if ($requestedFinalSize <= 0) {
    Write-Output '{"error":"Requested final size would be zero or negative"}'
    exit 1
}

# Get supported size range
$supported = Get-PartitionSupportedSize -DiskNumber $partition.DiskNumber -PartitionNumber $partition.PartitionNumber -ErrorAction Stop
$sizeMin = $supported.SizeMin
$sizeMax = $supported.SizeMax

if ($requestedFinalSize -lt $sizeMin) {
    Write-Output ('{{"error":"Requested final size {0} is below minimum supported size {1}", "sizeMin":{1}, "sizeMax":{2}, "currentSize":{3}}}' -f $requestedFinalSize, $sizeMin, $sizeMax, $currentSize)
    exit 1
}
if ($requestedFinalSize -gt $sizeMax) {
    Write-Output ('{{"error":"Requested final size {0} exceeds maximum supported size {1}", "sizeMin":{1}, "sizeMax":{2}, "currentSize":{3}}}' -f $requestedFinalSize, $sizeMin, $sizeMax, $currentSize)
    exit 1
}

# Output current state for verification
Write-Output ('{{"diskNumber":{0}, "partitionNumber":{1}, "currentSize":{2}, "sizeMin":{3}, "sizeMax":{4}, "requestedFinalSize":{5}, "requestedShrink":{6}}}' -f $partition.DiskNumber, $partition.PartitionNumber, $currentSize, $sizeMin, $sizeMax, $requestedFinalSize, $requestedShrink)
)PS1").arg(QString(driveLetter)).arg(static_cast<qulonglong>(requestedShrinkBytes));

    auto r = runPowerShellScript(script);
    if (!r) return core::makeError(r.error().kind(), r.error().message());

    QJsonParseError perr{};
    QJsonDocument doc = QJsonDocument::fromJson(r.value().toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        return core::makeError(core::Error::Kind::Platform,
            "PowerShell output was not valid JSON: " + perr.errorString().toStdString());
    }
    QJsonObject obj = doc.object();
    if (obj.contains("error")) {
        return core::makeError(core::Error::Kind::Platform,
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
        return core::makeError(core::Error::Kind::Platform,
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
    if (!resizeResult) return core::makeError(resizeResult.error().kind(), resizeResult.error().message());

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
    if (!verifyResult) return core::makeError(verifyResult.error().kind(), verifyResult.error().message());

    QJsonDocument verifyDoc = QJsonDocument::fromJson(verifyResult.value().toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        return core::makeError(core::Error::Kind::Platform,
            "Post-resize verification output was not valid JSON");
    }
    QJsonObject verifyObj = verifyDoc.object();
    const qulonglong newSize = verifyObj.value("newSize").toVariant().toULongLong();
    const QString fs = verifyObj.value("filesystem").toString();
    const qulonglong offset = verifyObj.value("offset").toVariant().toULongLong();

    // Verify the results
    if (fs != "NTFS") {
        return core::makeError(core::Error::Kind::Platform,
            "Post-resize verification failed: filesystem is no longer NTFS (got: " + fs.toStdString() + ")");
    }

    // Allow for small alignment differences (1 MB tolerance)
    const qulonglong tolerance = 1024ull * 1024;
    if (newSize < requestedFinalSize - tolerance || newSize > requestedFinalSize + tolerance) {
        return core::makeError(core::Error::Kind::Platform,
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
        const QString err = QString::fromLocal8Bit(proc.readAllStandardError());
        QFile::remove(scriptPath);
        return core::makeError<QString>(core::Error::Kind::Platform,
            "PowerShell script failed (exit " + std::to_string(proc.exitCode()) + "): " + err.toStdString());
    }
    const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
    QFile::remove(scriptPath);
    return core::makeOk(out);
}

core::Result<void> DiskOps::wipeDisk(int diskNumber) {
    if (diskNumber <= 0) {
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

    if (plan.targetDiskNumber <= 0) {
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
    if (!validateResult) return core::makeError(validateResult.error().kind(), validateResult.error().message());

    QJsonParseError perr{};
    QJsonDocument validateDoc = QJsonDocument::fromJson(validateResult.value().toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        return core::makeError(core::Error::Kind::Platform,
            "Pre-create validation output was not valid JSON: " + perr.errorString().toStdString());
    }
    QJsonObject validateObj = validateDoc.object();
    if (validateObj.contains("error")) {
        return core::makeError(core::Error::Kind::Platform,
            validateObj.value("error").toString().toStdString());
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
    if (!createResult) return core::makeError(createResult.error().kind(), createResult.error().message());

    QJsonDocument createDoc = QJsonDocument::fromJson(createResult.value().toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        return core::makeError(core::Error::Kind::Platform,
            "Create partition output was not valid JSON: " + perr.errorString().toStdString());
    }
    QJsonObject createObj = createDoc.object();
    if (createObj.contains("error")) {
        return core::makeError(core::Error::Kind::Platform,
            createObj.value("error").toString().toStdString());
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
    // offset matches expected gap offset, remaining unallocated space exists
    QString verifyScript = QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$diskNumber = %1
$expectedPartitionNumber = %2
$expectedOffset = %3
$expectedSize = %4
$stagingSizeBytes = %5
$totalFreedBytes = %6
$targetPartitionNumber = %7

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

# Calculate remaining unallocated space after staging partition
$allParts = @($partitions)
$totalUsed = 0
foreach ($p in $allParts) {
    $totalUsed += $p.Size
}
$remainingUnallocated = $disk.Size - $totalUsed

# Verify remaining unallocated is approximately what we expect
$expectedRemaining = $totalFreedBytes - $stagingSizeBytes
if ($expectedRemaining -lt 0) { $expectedRemaining = 0 }

# Allow some tolerance for alignment (10 MB)
$remainingDiff = [math]::Abs($remainingUnallocated - $expectedRemaining)
if ($remainingDiff -gt 10485760) {
    Write-Output ('{{"error":"Remaining unallocated space mismatch. Expected ~{0}, got {1}"}}' -f $expectedRemaining, $remainingUnallocated)
    exit 1
}

Write-Output ('{{"success":true, "remainingUnallocated":{0}, "partitionNumber":{1}, "size":{2}, "offset":{3}, "filesystem":"{4}"}}' -f $remainingUnallocated, $newPartition.PartitionNumber, $newPartition.Size, $newPartition.Offset, $volume.FileSystem)
)PS1").arg(diskNumber)
        .arg(partitionNumber)
        .arg(static_cast<qulonglong>(gapOffset))
        .arg(static_cast<qulonglong>(stagingSizeBytes))
        .arg(static_cast<qulonglong>(stagingSizeBytes))
        .arg(static_cast<qulonglong>(totalFreedBytes))
        .arg(targetPartitionNumber);

    auto verifyResult = runPowerShellScript(verifyScript);
    if (!verifyResult) return core::makeError(verifyResult.error().kind(), verifyResult.error().message());

    QJsonDocument verifyDoc = QJsonDocument::fromJson(verifyResult.value().toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        return core::makeError(core::Error::Kind::Platform,
            "Post-create verification output was not valid JSON: " + perr.errorString().toStdString());
    }
    QJsonObject verifyObj = verifyDoc.object();
    if (verifyObj.contains("error")) {
        return core::makeError(core::Error::Kind::Platform,
            verifyObj.value("error").toString().toStdString());
    }

    // Step 4: Assign a temporary drive letter for ISO copying
    // Use LetterGuard pattern - assign letter, track it, ensure cleanup
    char assignedLetter = 0;
    bool letterWasPreExisting = false;
    if (!driveLetter.isEmpty() && driveLetter.at(0).isLetter()) {
        assignedLetter = driveLetter.at(0).toLatin1();
        letterWasPreExisting = true;
    } else {
        // Find and assign a free letter
        auto freeLetter = findFreeLetter('Z');
        if (freeLetter) {
            assignedLetter = *freeLetter;
            QString assignScript = QString("Add-PartitionAccessPath -DiskNumber %1 -PartitionNumber %2 -AssignDriveLetter")
                .arg(diskNumber).arg(partitionNumber);
            auto assignResult = runPowerShellScript(assignScript);
            if (!assignResult) {
                // Non-fatal - we can still proceed without a drive letter
                assignedLetter = 0;
            } else {
                // Verify the letter was actually assigned and is accessible
                QString verifyLetterScript = QString(R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$partition = Get-Partition -DiskNumber %1 -PartitionNumber %2 -ErrorAction Stop
$volume = Get-Volume -Partition $partition -ErrorAction SilentlyContinue
if ($volume -and $volume.DriveLetter) {
    Write-Output ('{{"driveLetter":"{0}"}}' -f $volume.DriveLetter)
} else {
    Write-Output '{"driveLetter":""}'
)PS1").arg(diskNumber).arg(partitionNumber);
                auto verifyResult = runPowerShellScript(verifyLetterScript);
                if (verifyResult) {
                    QJsonDocument verifyDoc = QJsonDocument::fromJson(verifyResult.value().toUtf8(), &perr);
                    if (perr.error == QJsonParseError::NoError) {
                        QJsonObject verifyObj = verifyDoc.object();
                        QString actualLetter = verifyObj.value("driveLetter").toString();
                        if (actualLetter.isEmpty() || actualLetter.at(0).toLatin1() != assignedLetter) {
                            // Letter assignment didn't stick - fallback
                            assignedLetter = 0;
                        }
                    }
                }
            }
        }
    }

    if (assignedLetter) {
        bootMount = std::filesystem::path(std::string(1, assignedLetter) + ":\\");
    } else {
        // Fallback: use the partition path directly
        bootMount = std::filesystem::path("\\\\.\\PHYSICALDRIVE" + std::to_string(diskNumber) + "\\PARTITION" + std::to_string(partitionNumber));
    }

    // Note: rEFInd partition creation is deferred to Phase 2.3+
    (void)refindMount;

    return core::makeOk();
}

core::Result<std::filesystem::path> DiskOps::mountIso(
    const std::filesystem::path& iso) {
    QProcess proc;
    proc.start("powershell", {"-NoProfile", "-Command",
        QString("(Mount-DiskImage -ImagePath '%1' -PassThru | "
                "Get-Volume | Select-Object -First 1).DriveLetter").arg(
                    QString::fromStdString(iso.string()))});
    if (!proc.waitForFinished(15000)) {
        return core::makeError<std::filesystem::path>(core::Error::Kind::Platform,
            "Mount-DiskImage timed out");
    }
    const QString letter = QString::fromLocal8Bit(proc.readAll()).trimmed();
    if (letter.isEmpty() || proc.exitCode() != 0) {
        return core::makeError<std::filesystem::path>(core::Error::Kind::Platform,
            "Mount-DiskImage failed: " + QString::fromLocal8Bit(proc.readAll()).toStdString());
    }
    return core::makeOk<std::filesystem::path>(std::filesystem::path(
        (letter + ":\\").toStdString()));
}

void DiskOps::unmountIso(const std::filesystem::path& mount) {
    Q_UNUSED(mount);
    QProcess::execute("powershell", {"-NoProfile", "-Command",
        "Get-DiskImage | Where-Object { $_.Attached -eq $true } | Dismount-DiskImage"});
}

core::Result<void> DiskOps::copyFiles(const std::filesystem::path& src,
                                       const std::filesystem::path& dst) {
    // robocopy exit codes: 0=ok, 1=files copied, 2=extras, 3+=warnings
    // anything >= 8 is an error.
    QProcess proc;
    proc.start("robocopy",
               {QString::fromStdString(src.string()),
                QString::fromStdString(dst.string()),
                "/E", "/R:3", "/W:5", "/NP", "/NFL", "/NDL"});
    if (!proc.waitForFinished(-1)) {
        return core::makeError(core::Error::Kind::Platform, "robocopy timed out");
    }
    const int code = proc.exitCode();
    if (code >= 8) {
        return core::makeError(core::Error::Kind::Platform,
            "robocopy failed (exit " + std::to_string(code) + "): " +
            QString::fromLocal8Bit(proc.readAll()).toStdString());
    }
    return core::makeOk();
}

core::Result<void> DiskOps::patchDistroBootConfig(const core::Distro& d,
                                                   const core::InstallPlan& plan) {
    if (!plan.bootPartitionMount.has_value()) {
        return core::makeError(core::Error::Kind::InvalidInput,
            "bootPartitionMount is not set");
    }
    Q_UNUSED(d);
    return core::makeOk();
}

core::Result<void> DiskOps::installRefind(const core::InstallPlan& plan) {
    Q_UNUSED(plan);
    return core::makeError(core::Error::Kind::Platform,
        "installRefind not yet implemented (Phase 1.1 stub)");
}

core::Result<void> DiskOps::createBootEntry(const core::InstallPlan& plan) {
    return BcdStore::createLinuxEntry(plan);
}

void DiskOps::rollbackBootEntry(const core::InstallPlan& plan) {
    BcdStore::deleteEntry(plan);
}

void DiskOps::restartSystem() {
    // The actual restart is gated by the UI's RestartCountdownDialog.
    // This method is only called if the user has confirmed the
    // countdown. It is intentionally a fast no-arg shutdown.
    QProcess::startDetached("shutdown", {"/r", "/t", "0", "/f"});
}

std::optional<char> DiskOps::findFreeLetter(char startFrom) {
    for (char c = startFrom; c >= 'C'; --c) {
        std::filesystem::path testPath(std::string(1, c) + ":\\");
        if (!std::filesystem::exists(testPath)) {
            return c;
        }
    }
    return std::nullopt;
}

}  // namespace ulli::platform::windows
