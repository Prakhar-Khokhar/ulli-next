// platform/windows/EspOps.h
//
// Helpers for the Windows EFI System Partition. The original PowerShell
// script temporarily assigned a drive letter to copy the bootloader in.
// This class wraps that with proper RAII — if a letter is added, it
// gets removed on scope exit even when an exception is thrown.

#pragma once

#include "core/Result.h"

#include <filesystem>
#include <optional>

namespace ulli::platform::windows {

class EspOps {
public:
    // Returns the drive letter of the Windows ESP, optionally assigning
    // one if not already mounted. RAII: caller can rely on the letter
    // remaining valid until the returned LetterGuard is destroyed.
    struct LetterGuard {
        char letter = 0;
        bool wasAdded = false;
        ~LetterGuard();
    };

    static core::Result<LetterGuard> acquireLetter();

    // Copy the bootloader (EFI/BOOT/BOOTx64.EFI) into the Windows ESP
    // under \EFI\<safename>\.
    static core::Result<void> installBootloader(
        const std::filesystem::path& sourceBootDir,
        const std::filesystem::path& espRoot,
        const std::string& safeName);
};

}  // namespace ulli::platform::windows
