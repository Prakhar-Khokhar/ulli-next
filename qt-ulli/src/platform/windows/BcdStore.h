// platform/windows/BcdStore.h
//
// Wraps bcdedit.exe with explicit GUID lifecycle. Every successful
// create returns a GUID stored in InstallPlan::bcdGuid; on any later
// failure InstallEngine calls deleteEntry() in a finally. The audit
// flagged that the original PowerShell script only deleted the GUID
// on a narrow failure path — leaving the user with a default entry
// pointing to nothing. This class fixes that.

#pragma once

#include "core/InstallPlan.h"
#include "core/Result.h"

namespace ulli::platform::windows {

class BcdStore {
public:
    // Creates a new bootmgr copy with description "DistroName" and
    // configures device / path / displayorder / default. On success
    // sets plan.bcdGuid.
    static core::Result<void> createLinuxEntry(core::InstallPlan& plan);

    // Best-effort delete. Idempotent — silently succeeds if the GUID
    // is empty or already gone.
    static void deleteEntry(const core::InstallPlan& plan);
};

}  // namespace ulli::platform::windows
