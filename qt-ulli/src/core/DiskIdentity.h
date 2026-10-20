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
//   * The unique id is only equivalent to another unique id of the same
//     format. The format describes which kind of identifier the value is
//     (see UniqueIdFormat below), so the same text under two different
//     formats describes two different things.

#pragma once

#include "core/DiskInfo.h"

#include <string>
#include <vector>

namespace ulli::core {

// Prepares a raw device identifier for comparison.
//
// Only the surrounding whitespace is removed. The identifiers are opaque
// values, and every further transformation can merge two different
// devices: dropping a hyphen would make "AB-CD" and "A-BCD" equal, and
// folding the case would make "AB" and "ab" equal. Such a collision is
// exactly the false positive that must never happen here, because it
// can send a destructive operation to the wrong disk.
//
// The surrounding whitespace is trimmed because the serial strings of
// SCSI and ATA devices are space padded (SCSI VPD page 0x80, ATA
// IDENTIFY DEVICE); the padding is not part of the serial.
std::string trimDiskIdentifier(const std::string& raw);

// True when the identifier carries no information: it is empty or one of
// the placeholder values that many devices report ("0000000000",
// "NONE", ...). The placeholder vocabulary is a fixed list, so it is
// compared case insensitively: rejecting more is the safe direction.
bool isPlaceholderDiskIdentifier(const std::string& identifier);

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
