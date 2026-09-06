// platform/windows/Wmi.cpp
//
// Embeds a small set of PowerShell scripts as raw string literals. The
// scripts are written to a temp .ps1 file, run with `powershell.exe
// -NoProfile -ExecutionPolicy Bypass -File <path>`, and the JSON
// output is parsed. This avoids command-line escaping pain and keeps
// the scripts reviewable in source control.
//
// Script names (used as the lookup key):
//   - "list_disks"       : Get-Disk + Get-Partition + Get-Volume
//                          tree, used by enumerateDisks().
//   - "bitlocker_status" : manage-bde-free, used by BitLocker.cpp.

#include "platform/windows/Wmi.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <iostream>

namespace ulli::platform::windows {

namespace {

// All scripts share the same prelude: stop on error, no progress, no
// interactive prompts, emit JSON only.
constexpr const char* kPrelude = R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'
$WarningPreference     = 'SilentlyContinue'
)PS1";

// One script per top-level operation. Keep them small and JSON-shaped.
constexpr const char* kListDisks = R"PS1(
$disks = @( Get-Disk | Sort-Object Number )
$out = foreach ($d in $disks) {
    if ($d.OperationalStatus -ne 'Online') { continue }
    if ($d.Size -le 0) { continue }

    $parts = @( Get-Partition -DiskNumber $d.Number -ErrorAction SilentlyContinue |
                Sort-Object PartitionNumber )

    $partOut = foreach ($p in $parts) {
        # Volume info (may be null for unformatted/RAW partitions)
        $v = Get-Volume -Partition $p -ErrorAction SilentlyContinue
        $letter = $null
        $fs     = $null
        $label  = $null
        $sizeRemaining = $null
        if ($v) {
            if ($v.DriveLetter)      { $letter = [string]$v.DriveLetter }
            if ($v.FileSystem)       { $fs     = [string]$v.FileSystem }
            if ($v.FileSystemLabel)  { $label  = [string]$v.FileSystemLabel }
            if ($v.SizeRemaining -ne $null) { $sizeRemaining = [int64]$v.SizeRemaining }
        }
        [pscustomobject]@{
            PartitionNumber   = [int]$p.PartitionNumber
            Offset            = [int64]$p.Offset
            Size              = [int64]$p.Size
            DriveLetter       = $letter
            FileSystem        = $fs
            Label             = $label
            SizeRemaining     = $sizeRemaining
            IsBoot            = [bool]$p.IsBoot
            IsSystem          = [bool]$p.IsSystem
            IsHidden          = [bool]$p.IsHidden
            GptType           = if ($p.GptType) { [string]$p.GptType } else { $null }
            PartitionStyle    = [string]$p.DiskPartitionStyle
            MbrType           = if ($p.MbrType -ne $null) { [int]$p.MbrType } else { $null }
        }
    }

    [pscustomobject]@{
        Number        = [int]$d.Number
        Model         = if ($d.Model) { [string]$d.Model } else { '' }
        BusType       = if ($d.BusType) { [string]$d.BusType } else { 'Unknown' }
        PartitionStyle= [string]$d.PartitionStyle
        Size          = [int64]$d.Size
        Partitions    = $partOut
    }
}
$out | ConvertTo-Json -Depth 6 -Compress
)PS1";

constexpr const char* kBitlockerStatus = R"PS1(
# 'true' / 'false' on stdout, consumed as text.
$vol = Get-BitLockerVolume -MountPoint 'C:' -ErrorAction SilentlyContinue
if ($null -eq $vol) { 'false'; return }
$prot = [bool]$vol.ProtectionStatus
$locked = ($vol.VolumeStatus -eq 'FullyEncrypted' -or
           $vol.VolumeStatus -eq 'EncryptionInProgress' -or
           $vol.LockStatus -eq 'Locked')
if ($prot -and $locked) { 'true' } else { 'false' }
)PS1";

const std::string& scriptFor(const char* name) {
    if (std::string(name) == "list_disks") {
        static const std::string s = std::string(kPrelude) + "\n" + kListDisks;
        return s;
    }
    if (std::string(name) == "bitlocker_status") {
        static const std::string s = std::string(kPrelude) + "\n" + kBitlockerStatus;
        return s;
    }
    static const std::string empty;
    return empty;
}

}  // namespace

std::string Wmi::scriptBody(const char* scriptName) {
    return scriptFor(scriptName);
}

std::filesystem::path Wmi::writeTempScript(const std::string& body) {
    const QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir().mkpath(tmpDir);
    const QString path = QDir(tmpDir).filePath("ulli-wmi.ps1");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return {};
    }
    f.write(body.c_str(), static_cast<qint64>(body.size()));
    f.close();
    return std::filesystem::path(path.toStdString());
}

core::Result<QJsonDocument> Wmi::runScript(const char* scriptName) {
    const std::string body = scriptFor(scriptName);
    if (body.empty()) {
        return core::makeError<QJsonDocument>(core::Error::Kind::InvalidInput,
            std::string("Unknown WMI script: ") + scriptName);
    }
    const std::filesystem::path scriptPath = writeTempScript(body);
    if (scriptPath.empty()) {
        return core::makeError<QJsonDocument>(core::Error::Kind::Io,
            "Failed to write temp PowerShell script");
    }

    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start("powershell.exe",
               {"-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", QString::fromStdString(scriptPath.string())});
    if (!proc.waitForStarted(10000)) {
        return core::makeError<QJsonDocument>(core::Error::Kind::NotFound,
            "powershell.exe is not available on this system");
    }
    if (!proc.waitForFinished(60000)) {
        proc.kill();
        return core::makeError<QJsonDocument>(core::Error::Kind::Platform,
            "PowerShell script timed out: " + scriptName);
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        const QString err = QString::fromLocal8Bit(proc.readAllStandardError());
        return core::makeError<QJsonDocument>(core::Error::Kind::Platform,
            "PowerShell script '" + scriptName + "' failed: " + err.toStdString());
    }
    const QByteArray raw = proc.readAllStandardOutput();
    if (raw.isEmpty()) {
        return core::makeError<QJsonDocument>(core::Error::Kind::Platform,
            "PowerShell script produced no output: " + scriptName);
    }
    QJsonParseError perr{};
    QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
    if (perr.error != QJsonParseError::NoError) {
        return core::makeError<QJsonDocument>(core::Error::Kind::Platform,
            "PowerShell output was not valid JSON: " + perr.errorString().toStdString());
    }
    return core::makeOk(doc);
}

}  // namespace ulli::platform::windows
