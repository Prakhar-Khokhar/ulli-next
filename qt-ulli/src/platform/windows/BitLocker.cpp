// platform/windows/BitLocker.cpp
#include "platform/windows/BitLocker.h"

#include "platform/windows/Wmi.h"

namespace ulli::platform::windows {

bool BitLocker::isLocked() {
    auto r = Wmi::runScriptText("bitlocker_status");
    if (!r) {
        // PowerShell/CIM unavailable — fail open but do not lie.
        return false;
    }
    const QString text = r.value().trimmed();
    return text.compare("true", Qt::CaseInsensitive) == 0;
}

}  // namespace ulli::platform::windows
