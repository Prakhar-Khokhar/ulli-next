// core/DiskIdentity.cpp
#include "core/DiskIdentity.h"

#include <algorithm>
#include <cctype>

namespace ulli::core {

namespace {

// Placeholder values that devices report when they have no real serial.
// Many USB enclosures report the same dummy for every device, so such a
// value must never be used to tell two disks apart.
const char* const kPlaceholderIdentifiers[] = {
    "NONE",
    "NULL",
    "NA",
    "N/A",
    "UNKNOWN",
    "DEFAULT",
    "NOTAPPLICABLE",
    "NOTAVAILABLE",
    "TOBEFILLEDBYOEM",
    "TOBEFILLEDBYOEM.",
    "TO BE FILLED BY O.E.M.",
    "TO BE FILLED BY OEM",
    "0123456789",
    "1234567890",
    "0123456789ABCDEF",
    "1234567890ABCDEF",
    "DEADBEEF",
    "CAFEBABE",
    "NOTSET",
};

bool consistsOf(char c, const std::string& s) {
    return !s.empty() &&
           std::all_of(s.begin(), s.end(),
                       [c](char x) {
                           return std::toupper(static_cast<unsigned char>(x)) == c;
                       });
}

std::string toUpper(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

enum class IdKind { UniqueId, Serial };

const char* idKindName(IdKind kind) {
    return kind == IdKind::UniqueId ? "unique id" : "serial number";
}

struct IdentityKey {
    IdKind kind = IdKind::UniqueId;
    std::string value;   // trimmed, compared verbatim
    std::string format;  // UniqueIdFormat, only meaningful for the unique id
};

// The unique id of a disk is built from the device identification
// descriptor (SCSI VPD page 0x83) and the format says which kind of
// descriptor it was: "Vendor Specific" (0), "Vendor Id" (1), "EUI64"
// (2), "FCPH Name" (3) or "SCSI Name String" (8).
//
// Two identifiers are only equivalent when their text is equal *and*
// they come from the same kind of descriptor: the same text under two
// different formats describes two different things (for example a
// "SCSI Name String" is an iSCSI name, while an "EUI64" is a hex
// address), so they must not be treated as the same device.
//
// If one side does not report a format, the text comparison decides.
// That is the documented fallback: the format only describes how the
// value is to be read, and the ambiguity check below still refuses the
// operation when more than one disk carries the identifier.
//
// A format that was known when the plan was approved must still be
// reported by the live disk. If it is gone, the live enumeration cannot
// confirm that the identifier still has the same meaning, and the match
// is refused. (The Windows storage stack always reports the format, so
// a missing one means the value could not be read.)
bool formatsCompatible(const std::string& approvedFormat,
                       const std::string& liveFormat) {
    if (approvedFormat.empty()) {
        // The plan does not know the format: the identifier text decides.
        return true;
    }
    if (liveFormat.empty()) {
        // The plan knew the format, the live disk does not report one:
        // the meaning of the identifier cannot be confirmed.
        return false;
    }
    // The format names come from a fixed vocabulary, so comparing them
    // case insensitively cannot merge two different formats.
    return toUpper(approvedFormat) == toUpper(liveFormat);
}

// The identifiers of an identity, strongest first.
std::vector<IdentityKey> identityKeys(const DiskIdentity& id) {
    std::vector<IdentityKey> keys;

    const std::string unique = trimDiskIdentifier(id.uniqueId);
    if (!isPlaceholderDiskIdentifier(unique)) {
        keys.push_back({IdKind::UniqueId, unique,
                        trimDiskIdentifier(id.uniqueIdFormat)});
    }

    const std::string serial = trimDiskIdentifier(id.serialNumber);
    if (!isPlaceholderDiskIdentifier(serial)) {
        keys.push_back({IdKind::Serial, serial, std::string()});
    }

    return keys;
}

// The identifier of a live disk for the given kind. An empty string
// means "this disk does not report that identifier".
std::string diskKeyValue(const Disk& disk, IdKind kind) {
    const std::string raw = kind == IdKind::UniqueId ? disk.identity.uniqueId
                                                      : disk.identity.serialNumber;
    const std::string trimmed = trimDiskIdentifier(raw);
    return isPlaceholderDiskIdentifier(trimmed) ? std::string() : trimmed;
}

std::string diskKeyFormat(const Disk& disk, IdKind kind) {
    if (kind != IdKind::UniqueId) return std::string();
    return trimDiskIdentifier(disk.identity.uniqueIdFormat);
}

// True when the live disk carries the approved identifier.
bool diskMatchesKey(const Disk& disk, const IdentityKey& key) {
    if (diskKeyValue(disk, key.kind) != key.value) return false;
    if (key.kind != IdKind::UniqueId) return true;
    return formatsCompatible(key.format, diskKeyFormat(disk, key.kind));
}

// A readable description of the identifier that was looked for.
std::string describeKey(const IdentityKey& key) {
    std::string text = std::string(idKindName(key.kind)) + " (" + key.value + ")";
    if (key.kind == IdKind::UniqueId && !key.format.empty()) {
        text += " [" + key.format + "]";
    }
    return text;
}

}  // namespace

std::string trimDiskIdentifier(const std::string& raw) {
    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1])) != 0) {
        --end;
    }
    return raw.substr(begin, end - begin);
}

bool isPlaceholderDiskIdentifier(const std::string& identifier) {
    const std::string trimmed = trimDiskIdentifier(identifier);
    if (trimmed.empty()) return true;
    if (consistsOf('0', trimmed)) return true;
    if (consistsOf('F', trimmed)) return true;
    const std::string upper = toUpper(trimmed);
    for (const char* placeholder : kPlaceholderIdentifiers) {
        if (upper == placeholder) return true;
    }
    return false;
}

DiskIdentityCheck checkDiskIdentity(const DiskIdentity& approved,
                                    int approvedNumber,
                                    const std::vector<Disk>& liveDisks) {
    DiskIdentityCheck result;

    const std::vector<IdentityKey> keys = identityKeys(approved);
    if (keys.empty()) {
        result.match = DiskIdentityMatch::NoIdentity;
        result.detail =
            "The target disk has no unique id and no serial number, so it "
            "cannot be identified reliably. Refusing to modify a disk that "
            "cannot be identified - review the plan again.";
        return result;
    }

    // The strongest identifier decides. A weaker one is never used to
    // accept a disk that the stronger identifier contradicts.
    const IdentityKey& key = keys.front();

    const Disk* found = nullptr;
    int matches = 0;
    for (const Disk& disk : liveDisks) {
        if (!diskMatchesKey(disk, key)) continue;
        ++matches;
        if (found == nullptr) found = &disk;
    }

    if (matches == 0) {
        result.match = DiskIdentityMatch::NotFound;
        result.detail =
            std::string("The approved disk is no longer present: no disk "
                        "reports the same ") +
            describeKey(key) + ".";
        return result;
    }

    if (matches > 1) {
        result.match = DiskIdentityMatch::Ambiguous;
        result.detail =
            std::string("More than one disk reports the same ") +
            describeKey(key) +
            ". The target disk is not unique - refusing to modify any disk.";
        return result;
    }

    // The device is still there, but the plan was approved for a specific
    // disk number and the destructive commands address the disk by that
    // number. A changed number means the plan has to be reviewed again.
    if (found->number != approvedNumber) {
        result.match = DiskIdentityMatch::DiskNumberChanged;
        result.detail =
            "The approved disk is now disk " + std::to_string(found->number) +
            " instead of disk " + std::to_string(approvedNumber) +
            ". The plan must be reviewed again.";
        return result;
    }

    result.match = DiskIdentityMatch::Match;
    result.disk = found;
    return result;
}

}  // namespace ulli::core
