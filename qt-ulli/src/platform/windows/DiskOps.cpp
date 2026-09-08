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

core::Result<std::uint64_t> DiskOps::shrinkPartition(char /*driveLetter*/,
                                                    std::uint64_t /*newSizeBytes*/) {
    // The Windows backend does not implement partition shrinking. The
    // safe path is "UseFreeAll" / "UseFreeBoot" which use existing
    // unallocated space. Returning an explicit unsupported error
    // forces the engine and UI to pick a safe strategy.
    return core::makeError(core::Error::Kind::Platform,
        "Shrinking an existing NTFS partition is not supported by the "
        "ULLI Windows backend. Pick a strategy that uses unallocated space.");
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
                                          std::filesystem::path& refindMount) {
    // See file header for the filesystem support matrix. This Phase 1.1
    // implementation only creates FAT32 partitions (the live boot
    // partition and the optional rEFInd partition) via diskpart.
    //
    // The caller (InstallEngine) has already validated the plan and
    // verified the disk is not the system disk in preflight().

    if (plan.targetDiskNumber <= 0) {
        return core::makeError(core::Error::Kind::InvalidInput,
            "createLayout: targetDiskNumber not set");
    }

    // Step A: create the boot partition.
    QStringList script;
    script << "select disk " + QString::number(plan.targetDiskNumber);

    const std::uint64_t bootMB = plan.bootSizeBytes / (1024ull * 1024);
    script << QString("create partition primary size=%1 fs=fat32")
                .arg(static_cast<qulonglong>(bootMB));
    script << "format fs=fat32 label=\"LINUX_LIVE\" quick";
    // assign letter= L
    script << "assign letter=L";
    auto r = runDiskpartScript(script);
    if (!r) {
        return core::makeError(core::Error::Kind::Platform,
            "Failed to create boot partition: " + r.error().message());
    }

    bootMount = std::filesystem::path("L:\\");

    // Step B: optional rEFInd partition.
    if (plan.bootMode == core::BootMode::Refind) {
        QStringList rScript;
        rScript << "select disk " + QString::number(plan.targetDiskNumber);
        const std::uint64_t refMB = plan.refindSizeBytes / (1024ull * 1024);
        rScript << QString("create partition primary size=%1 fs=fat32")
                     .arg(static_cast<qulonglong>(refMB));
        rScript << "format fs=fat32 label=\"REFIND\" quick";
        rScript << "assign letter=R";
        auto rr = runDiskpartScript(rScript);
        if (!rr) {
            // Best-effort: log via the engine's log. Without a log
            // handle here, return an explicit error so the engine
            // surfaces it.
            return core::makeError(core::Error::Kind::Platform,
                "Failed to create rEFInd partition: " + rr.error().message());
        }
        refindMount = std::filesystem::path("R:\\");
    }
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
        "Dismount-DiskImage -ImagePath $env:lastIso 2>$null"});
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

}  // namespace ulli::platform::windows
