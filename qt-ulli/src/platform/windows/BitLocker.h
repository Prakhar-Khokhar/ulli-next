// platform/windows/BitLocker.h
//
// Pre-flight check for BitLocker-encrypted system volumes.
//
// Returns a typed result so that callers can distinguish:
//   Ok(true)  = volume is BitLocker-protected and locked/unsafe
//   Ok(false) = volume is confirmed safe (unencrypted or unlocked)
//   Error     = detection failed or state is indeterminate (fail-closed)

#pragma once

#include "core/Result.h"

namespace ulli::platform::windows {

class BitLocker {
public:
    // Checks whether the system volume (C:) is BitLocker-protected and
    // locked or unsafe. Returns Ok(true) when locked, Ok(false) when
    // confirmed safe, and an error when detection fails or the state is
    // indeterminate. Callers must treat an error as "block".
    static core::Result<bool> isLocked();
};

}  // namespace ulli::platform::windows
