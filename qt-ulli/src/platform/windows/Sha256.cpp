// platform/windows/Sha256.cpp
#include "platform/windows/Sha256.h"

#include "platform/windows/Wmi.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>

namespace ulli::platform::windows {

namespace {

// Embedded PowerShell script to compute SHA-256 and return JSON.
constexpr const char* kSha256Script = R"PS1(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

param(
    [Parameter(Mandatory=$true)]
    [string]$Path
)

if (-not (Test-Path -LiteralPath $Path)) {
    Write-Output '{"error":"File not found"}'
    exit 1
}

try {
    $hash = Get-FileHash -Algorithm SHA256 -Path $Path -ErrorAction Stop
    $result = @{
        hash = $hash.Hash.ToLower()
    }
    $result | ConvertTo-Json -Compress
} catch {
    Write-Output ('{"error":"' + $_.Exception.Message.Replace('"','\"') + '"}')
    exit 1
}
)PS1";

std::filesystem::path writeSha256Script() {
    const QString tmpDir = QDir::tempPath();
    const QString path = QDir(tmpDir).filePath("ulli-sha256.ps1");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return {};
    }
    f.write(kSha256Script);
    f.close();
    return std::filesystem::path(path.toStdString());
}

}  // namespace

core::Result<void> Sha256::verifyFile(
    const std::filesystem::path& file,
    const std::string& expectedHex) {
    
    if (!std::filesystem::exists(file)) {
        return core::makeError(core::Error::Kind::NotFound,
            "ISO file does not exist: " + file.string());
    }

    // Write the script to a temp file
    const std::filesystem::path scriptPath = writeSha256Script();
    if (scriptPath.empty()) {
        return core::makeError(core::Error::Kind::Io,
            "Failed to write temp PowerShell script");
    }

    // Run the PowerShell script with the file path as parameter
    QProcess proc;
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start("powershell.exe",
               {"-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", QString::fromStdString(scriptPath.string()),
                "-Path", QString::fromStdString(file.string())});
    
    if (!proc.waitForStarted(10000)) {
        return core::makeError(core::Error::Kind::NotFound,
            "powershell.exe is not available on this system");
    }
    if (!proc.waitForFinished(120000)) {
        proc.kill();
        return core::makeError(core::Error::Kind::Platform,
            "Get-FileHash timed out");
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        const QString err = QString::fromLocal8Bit(proc.readAllStandardError());
        const QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
        return core::makeError(core::Error::Kind::Platform,
            "Get-FileHash failed (exit " + std::to_string(proc.exitCode()) +
            "): " + err.toStdString() + " | " + out.toStdString());
    }

    const QByteArray raw = proc.readAllStandardOutput();
    if (raw.isEmpty()) {
        return core::makeError(core::Error::Kind::Platform,
            "Get-FileHash produced no output");
    }

    QJsonParseError perr{};
    QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
    if (perr.error != QJsonParseError::NoError) {
        return core::makeError(core::Error::Kind::Platform,
            "Get-FileHash output was not valid JSON: " + perr.errorString().toStdString());
    }

    QJsonObject obj = doc.object();
    if (obj.contains("error")) {
        return core::makeError(core::Error::Kind::Platform,
            "Get-FileHash error: " + obj.value("error").toString().toStdString());
    }

    QString actualHex = obj.value("hash").toString().toLower();
    QString expected = QString::fromStdString(expectedHex).toLower().trimmed();

    if (actualHex != expected) {
        return core::makeError(core::Error::Kind::Platform,
            "SHA-256 mismatch:\n  expected: " + expected.toStdString() +
            "\n  actual:   " + actualHex.toStdString());
    }

    return core::makeOk();
}

}  // namespace ulli::platform::windows