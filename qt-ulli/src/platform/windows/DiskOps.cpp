// platform/windows/DiskOps.cpp
//
// Phase 1 Windows implementation. Uses QProcess to call:
//   - parted.exe   (bundled) for partition ops
//   - bcdedit.exe  (system)  for UEFI boot entries
//   - robocopy.exe (system)  for file copy
//   - PowerShell   (system)  for BitLocker status
//
// BitLocker and BCD store get their own helper classes; this file
// focuses on the partition / format / copy mechanics.

#include "platform/windows/DiskOps.h"
#include "platform/windows/BcdStore.h"
#include "platform/windows/BitLocker.h"
#include "platform/windows/EspOps.h"
#include "platform/Platform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStorageInfo>
#include <QTextStream>

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

bool isPowerShellJson(const QByteArray& raw, QJsonDocument& doc) {
    QJsonParseError perr{};
    doc = QJsonDocument::fromJson(raw, &perr);
    return perr.error == QJsonParseError::NoError;
}

QStringList partedArgs(const QStringList& a) { return a; }

}  // namespace

DiskOps::DiskOps() = default;
DiskOps::~DiskOps() = default;

std::vector<core::Disk> DiskOps::enumerateDisks() {
    // Phase 1 stub: ask PowerShell for disk info in JSON.
    // The real implementation mirrors the existing PowerShell
    // Update-DiskInfo + Show-DiskPlan logic but uses parted -l JSON output
    // and WMI to translate to core::Disk / core::Partition.
    return {};
}

core::Result<void> DiskOps::preflight(const core::InstallPlan& plan) {
    if (!core::platform::isElevated()) {
        return core::makeNotElevated();
    }
    auto bde = BitLocker::isLocked();
    if (bde) {
        return core::makeError(core::Error::Kind::Platform,
            "BitLocker is enabled on the target volume. Decrypt it before running ULLI.");
    }
    return core::makeOk();
}

core::Result<std::filesystem::path> DiskOps::resolveIso(const core::Distro& d) {
    const std::filesystem::path cache = core::platform::cacheDir();
    const std::filesystem::path isoPath = cache / d.isoFilename();
    if (std::filesystem::exists(isoPath)) {
        // SHA-256 check (Phase 1: implement with QCryptographicHash).
        // For now, trust the file and return it.
        return core::makeOk(isoPath);
    }
    // Phase 1: ask the user to download manually. (Real download is
    // implemented in Phase 1.1 via QNetworkAccessManager.)
    return core::makeError(core::Error::Kind::NotFound,
        "ISO not found in cache. Please download " + d.isoFilename() +
        " to " + cache.string() + " (link: " + d.downloadPage() + ").");
}

core::Result<std::uint64_t> DiskOps::shrinkPartition(char driveLetter,
                                                    std::uint64_t /*newSizeBytes*/) {
    // Phase 1 stub: call into parted for shrink.
    return core::makeError(core::Error::Kind::Platform,
        "shrinkPartition not yet implemented (Phase 1 stub)");
}

core::Result<void> DiskOps::wipeDisk(int diskNumber) {
    const auto parted = core::platform::toolPath("parted");
    auto r = runTool(QString::fromStdString(parted.string()),
                     {"-s", QString("\\\\.\\PhysicalDrive%1").arg(diskNumber),
                      "mklabel", "gpt"});
    if (!r) return core::makeError(r.error().kind(), r.error().message());
    return core::makeOk();
}

core::Result<void> DiskOps::createLayout(const core::InstallPlan& plan,
                                          std::filesystem::path& bootMount,
                                          std::filesystem::path& refindMount) {
    // Phase 1 stub. The real implementation:
    //   1. Use parted to create the 7 GiB FAT32 boot partition at the
    //      chosen offset (or 100 MiB rEFInd partition too if requested).
    //   2. Use mke2fs / mkfs.fat / mkfs.btrfs (bundled) to format.
    //   3. Assign a drive letter via Windows DiskPart.
    //   4. Return the drive letters as mount points.
    (void)plan;
    (void)bootMount;
    (void)refindMount;
    return core::makeError(core::Error::Kind::Platform,
        "createLayout not yet implemented (Phase 1 stub)");
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
    // Phase 1 stub: Fedora and CachyOS get the LABEL= patch (mirrors
    // the existing PowerShell logic). The full impl is in Phase 1.1.
    Q_UNUSED(d);
    return core::makeOk();
}

core::Result<void> DiskOps::installRefind(const core::InstallPlan& plan) {
    Q_UNUSED(plan);
    return core::makeError(core::Error::Kind::Platform,
        "installRefind not yet implemented (Phase 1 stub)");
}

core::Result<void> DiskOps::createBootEntry(const core::InstallPlan& plan) {
    return BcdStore::createLinuxEntry(plan);
}

void DiskOps::rollbackBootEntry(const core::InstallPlan& plan) {
    BcdStore::deleteEntry(plan);
}

void DiskOps::restartSystem() {
    QProcess::startDetached("shutdown", {"/r", "/t", "0", "/f"});
}

}  // namespace ulli::platform::windows
