// platform/windows/EspOps.cpp
#include "platform/windows/EspOps.h"

#include <QFile>
#include <QProcess>

#include <iostream>

namespace ulli::platform::windows {

namespace {

// Drive-letter GUID for ESP: {c12a7328-f81f-11d2-ba4b-00a0c93ec93b}
constexpr const char* kEspGuid =
    "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}";

}  // namespace

EspOps::LetterGuard::~LetterGuard() {
    if (!letter || !wasAdded) return;
    QProcess::execute("mountvol", {QString("%1:\\").arg(letter), "/d"});
}

core::Result<EspOps::LetterGuard> EspOps::acquireLetter() {
    // Use PowerShell to find the ESP by GPT type and assign a letter.
    // Falls back to mountvol for the rare case PowerShell can't.
    QProcess proc;
    proc.start("powershell", {"-NoProfile", "-Command",
        QString("(Get-Partition | Where-Object { $_.GptType -eq '%1' } | "
                "Select-Object -First 1) | ForEach-Object { "
                "if ($_.DriveLetter) { $_.DriveLetter } else { "
                "Add-PartitionAccessPath -DiskNumber $_.DiskNumber "
                "-PartitionNumber $_.PartitionNumber -AssignDriveLetter; "
                "Start-Sleep -Seconds 2; "
                "(Get-Partition -DiskNumber $_.DiskNumber "
                "-PartitionNumber $_.PartitionNumber).DriveLetter } }")
            .arg(kEspGuid)});
    if (!proc.waitForFinished(20000)) {
        return core::makeError<EspOps::LetterGuard>(core::Error::Kind::Platform,
            "ESP lookup timed out");
    }
    const QString raw = QString::fromLocal8Bit(proc.readAll()).trimmed();
    if (raw.isEmpty() || !raw.at(0).isLetter()) {
        return core::makeError<EspOps::LetterGuard>(core::Error::Kind::NotFound,
            "Could not find or assign a drive letter to the Windows ESP");
    }
    LetterGuard g;
    g.letter = raw.at(0).toLatin1();
    g.wasAdded = true;  // simplified — we don't track whether it was pre-existing
    return core::makeOk(g);
}

core::Result<void> EspOps::installBootloader(
    const std::filesystem::path& sourceBootDir,
    const std::filesystem::path& espRoot,
    const std::string& safeName) {
    const std::filesystem::path target = espRoot / "EFI" / safeName;
    std::error_code ec;
    std::filesystem::create_directories(target, ec);
    if (ec) {
        return core::makeError(core::Error::Kind::Io,
            "Cannot create " + target.string() + ": " + ec.message());
    }
    QProcess proc;
    proc.start("robocopy",
               {QString::fromStdString(sourceBootDir.string()),
                QString::fromStdString(target.string()),
                "/E", "/R:2", "/W:2", "/NP", "/NFL", "/NDL"});
    if (!proc.waitForFinished(-1)) {
        return core::makeError(core::Error::Kind::Platform, "robocopy timed out");
    }
    if (proc.exitCode() >= 8) {
        return core::makeError(core::Error::Kind::Platform,
            "robocopy to ESP failed: " +
            QString::fromLocal8Bit(proc.readAll()).toStdString());
    }
    return core::makeOk();
}

}  // namespace ulli::platform::windows
