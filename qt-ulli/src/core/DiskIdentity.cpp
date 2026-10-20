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
                       [c](char x) { return x == c; });
}

enum class IdKind { UniqueId, Serial };

const char* idKindName(IdKind kind) {
    return kind == IdKind::UniqueId ? "unique id" : "serial number";
}

struct IdentityKey {
    IdKind kind = IdKind::UniqueId;
    std::string value;  // normalized
};

// The identifiers of an identity, strongest first.
std::vector<IdentityKey> identityKeys(const DiskIdentity& id) {
    std::vector<IdentityKey> keys;

    const std::string unique = normalizeDiskIdentifier(id.uniqueId);
    if (!isPlaceholderDiskIdentifier(unique)) {
        keys.push_back({IdKind::UniqueId, unique});
    }

    const std::string serial = normalizeDiskIdentifier(id.serialNumber);
    if (!isPlaceholderDiskIdentifier(serial)) {
        keys.push_back({IdKind::Serial, serial});
    }

    return keys;
}

// The normalized identifier of a live disk for the given kind. An empty
// string means "this disk does not report that identifier".
std::string diskKey(const Disk& disk, IdKind kind) {
    const std::string raw = kind == IdKind::UniqueId ? disk.identity.uniqueId
                                                      : disk.identity.serialNumber;
    const std::string normalized = normalizeDiskIdentifier(raw);
    return isPlaceholderDiskIdentifier(normalized) ? std::string() : normalized;
}

}  // namespace

std::string normalizeDiskIdentifier(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isspace(u) != 0) continue;
        if (c == '-' || c == '_') continue;
        out.push_back(static_cast<char>(std::toupper(u)));
    }
    return out;
}

bool isPlaceholderDiskIdentifier(const std::string& normalized) {
    if (normalized.empty()) return true;
    if (consistsOf('0', normalized)) return true;
    if (consistsOf('F', normalized)) return true;
    for (const char* placeholder : kPlaceholderIdentifiers) {
        if (normalized == placeholder) return true;
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
        if (diskKey(disk, key.kind) != key.value) continue;
        ++matches;
        if (found == nullptr) found = &disk;
    }

    if (matches == 0) {
        result.match = DiskIdentityMatch::NotFound;
        result.detail =
            std::string("The approved disk is no longer present: no disk "
                        "reports the same ") +
            idKindName(key.kind) + " (" + key.value + ").";
        return result;
    }

    if (matches > 1) {
        result.match = DiskIdentityMatch::Ambiguous;
        result.detail =
            std::string("More than one disk reports the same ") +
            idKindName(key.kind) + " (" + key.value +
            "). The target disk is not unique - refusing to modify any disk.";
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
