// platform/windows/BitLocker.cpp
#include "platform/windows/BitLocker.h"

#include "platform/windows/Wmi.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace ulli::platform::windows {

bool BitLocker::isLocked() {
    auto r = Wmi::runScript("bitlocker_status");
    if (!r) {
        // PowerShell/CIM unavailable — fail open but do not lie.
        return false;
    }
    QJsonDocument doc = r.value();
    if (!doc.isTextual()) return false;
    const QString text = doc.text().trimmed();
    return text.compare("true", Qt::CaseInsensitive) == 0;
}

}  // namespace ulli::platform::windows
