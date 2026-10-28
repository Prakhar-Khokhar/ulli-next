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

#include "core/ScriptDiagnostics.h"

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
            PartitionGuid     = if ($p.Guid) { [string]$p.Guid } else { $null }
            GptType           = if ($p.GptType) { [string]$p.GptType } else { $null }
            PartitionStyle    = [string]$p.DiskPartitionStyle
            MbrType           = if ($p.MbrType -ne $null) { [int]$p.MbrType } else { $null }
        }
    }

    # Device identity: UniqueId is the storage stack's own id for the
    # device and survives a repartition, SerialNumber is the (weaker)
    # hardware serial. Both are needed to tell two identical disks
    # apart - the disk number alone is only an enumeration index.
    # UniqueIdFormat names the descriptor type the unique id was built
    # from (Vendor Specific, Vendor Id, EUI64, FCPH Name, SCSI Name
    # String); it is part of the identity and is compared with it.
    # 0 is a valid descriptor type ("Vendor Specific"), but it is falsy in
    # PowerShell - so the null check below has to be explicit, otherwise a
    # valid format would be serialized as an empty string.
    [pscustomobject]@{
        Number        = [int]$d.Number
        Model         = if ($d.Model) { [string]$d.Model } else { '' }
        BusType       = if ($d.BusType) { [string]$d.BusType } else { 'Unknown' }
        PartitionStyle= [string]$d.PartitionStyle
        Size          = [int64]$d.Size
        UniqueId      = if ($d.UniqueId) { [string]$d.UniqueId } else { '' }
        UniqueIdFormat= if ($null -ne $d.UniqueIdFormat) { [string]$d.UniqueIdFormat } else { '' }
        SerialNumber  = if ($d.SerialNumber) { [string]$d.SerialNumber } else { '' }
        Partitions    = $partOut
    }
}
$out | ConvertTo-Json -Depth 6 -Compress
)PS1";

constexpr const char* kBitlockerStatus = R"PS1(
# Returns JSON: {"status":"locked"} or {"status":"unlocked"} or
# {"status":"indeterminate","reason":"..."}.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$partition = Get-Partition -DriveLetter 'C' -ErrorAction Stop
$volume = Get-Volume -Partition $partition -ErrorAction SilentlyContinue
if (-not $volume) {
    $r = [pscustomobject]@{ status = "indeterminate"; reason = "no volume for C:" }
    Write-Output ($r | ConvertTo-Json -Compress)
    exit 0
}

$mountPoint = 'C:'

# Method 1: Get-BitLockerVolume cmdlet
$blCmd = Get-Command Get-BitLockerVolume -ErrorAction SilentlyContinue
if ($blCmd) {
    $bl = Get-BitLockerVolume -MountPoint $mountPoint -ErrorAction SilentlyContinue
    if (-not $bl) {
        $r = [pscustomobject]@{ status = "indeterminate"; reason = "Get-BitLockerVolume returned no result" }
        Write-Output ($r | ConvertTo-Json -Compress)
        exit 0
    }
    $volStatus = [string]$bl.VolumeStatus
    $lockSt = [string]$bl.LockStatus
    $protSt = $bl.ProtectionStatus

    if ($volStatus -eq 'EncryptionInProgress' -or $volStatus -eq 'DecryptionInProgress' -or
        $volStatus -eq 'EncryptionPaused' -or $volStatus -eq 'DecryptionPaused') {
        $r = [pscustomobject]@{ status = "locked"; reason = "bitlocker encryption/decryption in progress or paused" }
        Write-Output ($r | ConvertTo-Json -Compress)
        exit 0
    }
    if ($volStatus -eq 'FullyDecrypted') {
        $r = [pscustomobject]@{ status = "unlocked"; reason = "bitlocker fully decrypted" }
        Write-Output ($r | ConvertTo-Json -Compress)
        exit 0
    }
    if ($volStatus -eq 'FullyEncrypted') {
        if ($lockSt -eq 'Locked') {
            $r = [pscustomobject]@{ status = "locked"; reason = "bitlocker protected and locked" }
            Write-Output ($r | ConvertTo-Json -Compress)
            exit 0
        }
        # Validate ProtectionStatus is exactly On/Off (not coerced)
        $protValid = ($protSt -eq $true -or $protSt -eq $false -or $protSt -eq 'On' -or $protSt -eq 'Off')
        if (-not $protValid) {
            $r = [pscustomobject]@{ status = "indeterminate"; reason = "invalid ProtectionStatus value" }
            Write-Output ($r | ConvertTo-Json -Compress)
            exit 0
        }
        $protBool = ($protSt -eq $true -or $protSt -eq 'On')
        if ($protBool -and $lockSt -eq 'Unlocked') {
            $r = [pscustomobject]@{ status = "unlocked"; reason = "bitlocker not locked" }
            Write-Output ($r | ConvertTo-Json -Compress)
            exit 0
        }
        $r = [pscustomobject]@{ status = "indeterminate"; reason = "bitlocker encrypted but state ambiguous" }
        Write-Output ($r | ConvertTo-Json -Compress)
        exit 0
    }
    $r = [pscustomobject]@{ status = "indeterminate"; reason = ("unrecognized VolumeStatus: " + $volStatus) }
    Write-Output ($r | ConvertTo-Json -Compress)
    exit 0
}

# Method 2: WMI Win32_EncryptableVolume
$wmiAvailable = $false
$wmiVol = $null
try {
    $wmiVol = Get-CimInstance -Namespace "root/CIMV2/Security/MicrosoftVolumeEncryption" -ClassName Win32_EncryptableVolume -ErrorAction Stop | Where-Object { $_.DriveLetter -eq $mountPoint }
    $wmiAvailable = $true
} catch {
    $wmiAvailable = $false
}

if ($wmiAvailable -and $wmiVol) {
    if ($wmiVol -is [array]) {
        $r = [pscustomobject]@{ status = "indeterminate"; reason = "WMI returned multiple matching volumes" }
        Write-Output ($r | ConvertTo-Json -Compress)
        exit 0
    }
    if ($null -eq $wmiVol.ProtectionStatus -or $null -eq $wmiVol.ConversionStatus -or $null -eq $wmiVol.LockStatus) {
        $r = [pscustomobject]@{ status = "indeterminate"; reason = "WMI missing required properties" }
        Write-Output ($r | ConvertTo-Json -Compress)
        exit 0
    }
    $protStatus = [int]$wmiVol.ProtectionStatus
    $convStatus = [int]$wmiVol.ConversionStatus
    $lockStatus = [int]$wmiVol.LockStatus

    if ($protStatus -lt 0 -or $protStatus -gt 2 -or
        $convStatus -lt 0 -or $convStatus -gt 5 -or
        $lockStatus -lt 0 -or $lockStatus -gt 1) {
        $r = [pscustomobject]@{ status = "indeterminate"; reason = "WMI property values out of range" }
        Write-Output ($r | ConvertTo-Json -Compress)
        exit 0
    }
    if ($lockStatus -eq 1) {
        $r = [pscustomobject]@{ status = "locked"; reason = "WMI reports volume locked" }
        Write-Output ($r | ConvertTo-Json -Compress)
        exit 0
    }
    if ($convStatus -ge 2 -and $convStatus -le 5) {
        $r = [pscustomobject]@{ status = "locked"; reason = "WMI reports encryption/decryption in progress or paused" }
        Write-Output ($r | ConvertTo-Json -Compress)
        exit 0
    }
    if ($convStatus -eq 0) {
        $r = [pscustomobject]@{ status = "unlocked"; reason = "WMI reports fully decrypted" }
        Write-Output ($r | ConvertTo-Json -Compress)
        exit 0
    }
    if ($convStatus -eq 1 -and $protStatus -eq 0) {
        $r = [pscustomobject]@{ status = "indeterminate"; reason = "WMI encrypted but not protected" }
        Write-Output ($r | ConvertTo-Json -Compress)
        exit 0
    }
    if ($convStatus -eq 1 -and $protStatus -eq 1 -and $lockStatus -eq 0) {
        $r = [pscustomobject]@{ status = "unlocked"; reason = "WMI reports encrypted and unlocked" }
        Write-Output ($r | ConvertTo-Json -Compress)
        exit 0
    }
    if ($convStatus -eq 1 -and $protStatus -eq 1) {
        $r = [pscustomobject]@{ status = "indeterminate"; reason = "WMI encrypted and protected but lock status unknown" }
        Write-Output ($r | ConvertTo-Json -Compress)
        exit 0
    }
    if ($protStatus -eq 2) {
        $r = [pscustomobject]@{ status = "indeterminate"; reason = "WMI protection status unknown" }
        Write-Output ($r | ConvertTo-Json -Compress)
        exit 0
    }
    $r = [pscustomobject]@{ status = "indeterminate"; reason = ("WMI unrecognized state: conv=" + $convStatus + " prot=" + $protStatus + " lock=" + $lockStatus) }
    Write-Output ($r | ConvertTo-Json -Compress)
    exit 0
}

# Method 3: manage-bde
$manageBdeCmd = Get-Command manage-bde.exe -ErrorAction SilentlyContinue
if (-not $manageBdeCmd) {
    $r = [pscustomobject]@{ status = "indeterminate"; reason = "BitLocker PowerShell module, WMI Win32_EncryptableVolume, and manage-bde are all unavailable" }
    Write-Output ($r | ConvertTo-Json -Compress)
    exit 0
}

$mbOutput = & manage-bde.exe -status $mountPoint 2>&1
$mbExitCode = $LASTEXITCODE

if ($mbExitCode -ne 0) {
    $r = [pscustomobject]@{ status = "indeterminate"; reason = ("manage-bde exited with code " + $mbExitCode) }
    Write-Output ($r | ConvertTo-Json -Compress)
    exit 0
}

$lockStatus = ''
$conversionStatus = ''
$protectionStatus = ''

foreach ($line in $mbOutput) {
    $lineStr = $line.ToString().Trim()
    if ($lineStr -match 'Lock Status:\s*(.+)') {
        $lockStatus = $matches[1].Trim()
    }
    if ($lineStr -match 'Conversion Status:\s*(.+)') {
        $conversionStatus = $matches[1].Trim()
    }
    if ($lineStr -match 'Protection Status:\s*(.+)') {
        $protectionStatus = $matches[1].Trim()
    }
}

if ($lockStatus -eq 'Locked') {
    $r = [pscustomobject]@{ status = "locked"; reason = "manage-bde reports locked" }
    Write-Output ($r | ConvertTo-Json -Compress)
    exit 0
}
if ([string]::IsNullOrWhiteSpace($lockStatus) -or [string]::IsNullOrWhiteSpace($conversionStatus)) {
    $r = [pscustomobject]@{ status = "indeterminate"; reason = "manage-bde output could not be parsed (possibly localized)" }
    Write-Output ($r | ConvertTo-Json -Compress)
    exit 0
}
if ($conversionStatus -eq 'Fully Decrypted' -and $protectionStatus -eq 'Protection Off') {
    $r = [pscustomobject]@{ status = "unlocked"; reason = "manage-bde reports fully decrypted" }
    Write-Output ($r | ConvertTo-Json -Compress)
    exit 0
}
if ($conversionStatus -eq 'Fully Encrypted' -and $protectionStatus -eq 'Protection On' -and $lockStatus -eq 'Unlocked') {
    $r = [pscustomobject]@{ status = "unlocked"; reason = "manage-bde reports encrypted but unlocked" }
    Write-Output ($r | ConvertTo-Json -Compress)
    exit 0
}
if ($conversionStatus -eq 'Fully Encrypted' -and $protectionStatus -eq 'Protection On') {
    $r = [pscustomobject]@{ status = "indeterminate"; reason = "manage-bde lock status not determined" }
    Write-Output ($r | ConvertTo-Json -Compress)
    exit 0
}
if ($conversionStatus.Contains("InProgress") -or $conversionStatus.Contains("Paused")) {
    $r = [pscustomobject]@{ status = "indeterminate"; reason = "manage-bde in-progress state" }
    Write-Output ($r | ConvertTo-Json -Compress)
    exit 0
}
$r = [pscustomobject]@{ status = "indeterminate"; reason = "manage-bde unrecognized state combination" }
Write-Output ($r | ConvertTo-Json -Compress)
exit 0
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
            std::string("PowerShell script timed out: ") + scriptName);
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        // Both streams are reported: the scripts can leave a partial
        // result on standard output, and PowerShell reports its own
        // errors on standard error.
        const QString err = QString::fromLocal8Bit(proc.readAllStandardError());
        const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
        return core::makeError<QJsonDocument>(core::Error::Kind::Platform,
            std::string("PowerShell script '") + scriptName + "' failed: " +
            core::joinScriptOutput(err, out));
    }
    const QByteArray raw = proc.readAllStandardOutput();
    if (raw.isEmpty()) {
        return core::makeError<QJsonDocument>(core::Error::Kind::Platform,
            std::string("PowerShell script produced no output: ") + scriptName);
    }
    QJsonParseError perr{};
    QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
    if (perr.error != QJsonParseError::NoError) {
        return core::makeError<QJsonDocument>(core::Error::Kind::Platform,
            "PowerShell output was not valid JSON: " + perr.errorString().toStdString());
    }
    return core::makeOk(doc);
}

core::Result<QString> Wmi::runScriptText(const char* scriptName) {
    const std::string body = scriptFor(scriptName);
    if (body.empty()) {
        return core::makeError<QString>(core::Error::Kind::InvalidInput,
            std::string("Unknown WMI script: ") + scriptName);
    }
    const std::filesystem::path scriptPath = writeTempScript(body);
    if (scriptPath.empty()) {
        return core::makeError<QString>(core::Error::Kind::Io,
            "Failed to write temp PowerShell script");
    }

    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start("powershell.exe",
               {"-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", QString::fromStdString(scriptPath.string())});
    if (!proc.waitForStarted(10000)) {
        return core::makeError<QString>(core::Error::Kind::NotFound,
            "powershell.exe is not available on this system");
    }
    if (!proc.waitForFinished(60000)) {
        proc.kill();
        return core::makeError<QString>(core::Error::Kind::Platform,
            std::string("PowerShell script timed out: ") + scriptName);
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        // Both streams are reported: the scripts can leave a partial
        // result on standard output, and PowerShell reports its own
        // errors on standard error.
        const QString err = QString::fromLocal8Bit(proc.readAllStandardError());
        const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
        return core::makeError<QString>(core::Error::Kind::Platform,
            std::string("PowerShell script '") + scriptName + "' failed: " +
            core::joinScriptOutput(err, out));
    }
    const QByteArray raw = proc.readAllStandardOutput();
    return core::makeOk(QString::fromLocal8Bit(raw).trimmed());
}

}  // namespace ulli::platform::windows
