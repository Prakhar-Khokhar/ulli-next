// core/DiskIdentity.h
//
// Policy for "is the live disk still the disk the user approved?".
//
// The installer addresses disks by number, but a number is only an
// enumeration index: it changes when a device is attached, removed or
// re-enumerated, and two identical drives share model and capacity. A
// plan that only carries number + model + size can therefore end up
// pointing at a different physical disk.
//
// The approved identity is carried through the plan and compared against
// a fresh enumeration before anything is written:
//
//   * The strongest identifier that the plan carries decides. A weaker
//     identifier is never used to accept a disk that the stronger one
//     contradicts (a replaced disk with a cloned serial is not accepted).
//   * Identifiers that carry no information (empty, or a well known
//     placeholder like "0000000000") are treated as absent.
//   * If no usable identifier is available the operation is refused: a
//     disk that cannot be identified is never modified.
//   * If more than one live disk carries the approved identifier the
//     operation is refused as well: the target is not unique.
//   * If the device is still there but under a different disk number the
//     operation is refused and the plan has to be reviewed again, so a
//     number change can never silently retarget another disk.

#pragma once

#include "core/DiskInfo.h"

#include <string>
#include <vector>

namespace ulli::core {

// Normalizes a raw device identifier for comparison:
//   * whitespace, hyphens and underscores are removed (vendors pad and
//     format serials inconsistently)
//   * the value is upper-cased
std::string normalizeDiskIdentifier(const std::string& raw);

// True when the (already normalized) identifier carries no information:
// it is empty or one of the placeholder values that many devices report.
bool isPlaceholderDiskIdentifier(const std::string& normalized);

enum class DiskIdentityMatch {
    Match,             // the live disk is the approved disk
    NoIdentity,        // the plan carries no usable identifier
    NotFound,          // no live disk carries the approved identifier
    Ambiguous,         // more than one live disk carries the identifier
    DiskNumberChanged, // the device is there, but under a different number
};

// Outcome of comparing the approved identity with a fresh enumeration.
struct DiskIdentityCheck {
    DiskIdentityMatch match = DiskIdentityMatch::NoIdentity;
    std::string detail;          // human readable reason
    // The matching live disk. It points into the vector that was passed
    // to checkDiskIdentity(), so that vector has to stay alive as long as
    // the check is used.
    const Disk* disk = nullptr;

    bool ok() const { return match == DiskIdentityMatch::Match; }
};

// Compares the identity captured in the plan with a fresh enumeration.
// `approvedNumber` is the disk number the plan was approved for.
DiskIdentityCheck checkDiskIdentity(const DiskIdentity& approved,
                                    int approvedNumber,
                                    const std::vector<Disk>& liveDisks);

}  // namespace ulli::core
