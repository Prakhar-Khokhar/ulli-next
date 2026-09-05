// platform/windows/BitLocker.cpp
#include "platform/windows/BitLocker.h"

#include <QProcess>

namespace ulli::platform::windows {

bool BitLocker::isLocked() {
    // Use manage-bde (built into Windows) — no PowerShell dependency.
    QProcess proc;
    proc.start("manage-bde.exe", {"/status", "C:"});
    if (!proc.waitForFinished(10000)) return false;
    if (proc.exitCode() != 0) return false;
    const QString out = QString::fromLocal8Bit(proc.readAll());
    // Look for "Protection On" and "Lock Status:" lines.
    bool protectionOn = out.contains("Protection On", Qt::CaseInsensitive);
    bool locked = out.contains("Lock Status:           Locked", Qt::CaseInsensitive);
    return protectionOn && locked;
}

}  // namespace ulli::platform::windows
