// platform/windows/BitLocker.cpp
//
// Pre-flight check for BitLocker-encrypted system volumes.
//
// Uses a PowerShell script that tries three detection methods:
//   1. Get-BitLockerVolume cmdlet (BitLocker PowerShell module)
//   2. WMI Win32_EncryptableVolume (available on all BitLocker-capable editions)
//   3. manage-bde.exe -status (command-line fallback)
//
// Returns a typed result so callers can distinguish:
//   Ok(true)  = volume is BitLocker-protected and locked/unsafe
//   Ok(false) = volume is confirmed safe (unencrypted or unlocked)
//   Error     = detection failed or state is indeterminate (fail-closed)

#include "platform/windows/BitLocker.h"

#include "platform/windows/Wmi.h"
#include "platform/windows/DiskOps.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace ulli::platform::windows {

core::Result<bool> BitLocker::isLocked() {
    auto r = Wmi::runScriptText("bitlocker_status");
    if (!r) {
        // PowerShell/CIM unavailable — fail closed.
        return core::makeError<bool>(r.error().kind(),
            "BitLocker detection failed: " + r.error().message());
    }
    const QString text = r.value().trimmed();
    // The script now emits JSON. Parse it.
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        // Malformed output — fail closed.
        return core::makeError<bool>(core::Error::Kind::Platform,
            "BitLocker detection returned invalid output: " + text.toStdString());
    }
    const QJsonObject obj = doc.object();
    const QString status = obj.value("status").toString();
    const QString reason = obj.value("reason").toString();

    if (status == "locked") return core::makeOk<bool>(true);
    if (status == "unlocked") return core::makeOk<bool>(false);
    // indeterminate or unknown — fail closed
    return core::makeError<bool>(core::Error::Kind::Platform,
        "BitLocker status indeterminate: " + reason.toStdString());
}

}  // namespace ulli::platform::windows
