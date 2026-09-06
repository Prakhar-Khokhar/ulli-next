// platform/windows/Sha256.h
//
// SHA-256 verification for ISO files on Windows. Uses the built-in
// `Get-FileHash` cmdlet via PowerShell. No new dependencies required.

#pragma once

#include "core/Result.h"

#include <filesystem>
#include <string>

namespace ulli::platform::windows {

class Sha256 {
public:
    // Compute the SHA-256 hash of a file and compare it against the
    // expected lowercase hex string. Returns ok() on match, or an
    // error on mismatch, missing file, or hashing failure.
    static core::Result<void> verifyFile(
        const std::filesystem::path& file,
        const std::string& expectedHex);
};

}  // namespace ulli::platform::windows