// core/Sha256.h
//
// Bounded-memory SHA-256 helpers. The file is read in fixed size chunks
// so a multi-gigabyte ISO never has to fit in memory.

#pragma once

#include "core/Result.h"

#include <filesystem>
#include <string>

namespace ulli::core {

class Sha256 {
public:
    // Computes the SHA-256 of a file, reading it in bounded chunks.
    // Returns the lower case hex digest.
    static Result<std::string> hashFile(const std::filesystem::path& file);

    // Compares the SHA-256 of a file against an expected hex digest.
    // An empty expected digest is an error: without a trusted digest
    // nothing can be claimed to be cryptographically verified.
    static Result<void> verifyFile(const std::filesystem::path& file,
                                   const std::string& expectedHex);
};

}  // namespace ulli::core
