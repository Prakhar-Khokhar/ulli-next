// platform/windows/BitLocker.h
//
// Pre-flight check for BitLocker-encrypted system volumes. The audit
// flagged that the original PowerShell script proceeded with
// Resize-Partition / Shrink-Partition on C: without ever checking
// BitLocker state — a silent hazard.

#pragma once

namespace ulli::platform::windows {

class BitLocker {
public:
    // Returns true if the system volume (C:) is BitLocker-protected
    // and currently locked or encrypting. Returns false if the volume
    // is unencrypted, fully decrypted, or unlocked.
    //
    // If PowerShell is unavailable, returns false (fail-open) but the
    // preflight check logs a warning. This matches the conservative
    // side: we block on "yes", not on "don't know".
    static bool isLocked();
};

}  // namespace ulli::platform::windows
