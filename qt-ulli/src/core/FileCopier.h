// core/FileCopier.h
//
// The copy that fills the boot partition with the live ISO.
//
// The work is split into two steps so that the source tree is walked
// exactly once:
//
//   1. enumerateCopyPlan() walks the source tree and collects its files
//      and directories. It also computes the total size, the file count
//      and the largest file - the values the safety checks need.
//   2. copyCopyPlan() creates the directories and copies the files from
//      the plan without touching the source tree again.
//
// Both steps report their progress and check the cancellation flag, so a
// long running copy never looks like a hang: the caller always knows
// which phase runs and how far it got.

#pragma once

#include "core/Result.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ulli::core {

// Progress of a copy step.
//   percent - 0..100 within the step
//   phase   - a short name of the current phase, e.g. "Copying ISO contents"
//   detail  - a human readable line for the log (may be empty)
using CopyProgressCallback = std::function<void(int percent,
                                                const std::string& phase,
                                                const std::string& detail)>;

// One entry of the source tree.
struct CopyEntry {
    std::filesystem::path source;    // the absolute source path
    std::filesystem::path relative;  // relative to the source root
    bool isDirectory = false;
    std::uint64_t size = 0;
};

// The source tree, collected for the copy.
struct CopyPlan {
    // Directories first, parents before their children.
    std::vector<CopyEntry> directories;
    std::vector<CopyEntry> files;
    std::uint64_t totalBytes = 0;
    std::uint64_t fileCount = 0;
    std::uint64_t largestFileBytes = 0;
    // Entries that were not copied because the destination file system
    // cannot represent them (symbolic links) or because they are not
    // regular files. They are reported to the caller, never skipped
    // silently.
    std::vector<std::string> skipped;
};

// What the copy did.
struct CopyStats {
    std::uint64_t filesCopied = 0;
    std::uint64_t bytesCopied = 0;
    std::uint64_t directoriesCreated = 0;
};

// The safety limits and the retry behaviour of the copy.
struct CopyLimits {
    // The largest file the destination file system can hold. FAT32
    // cannot store a file of 4 GiB or more. 0 disables the check.
    std::uint64_t maxFileSizeBytes = 0;
    // The free space the destination needs in addition to the size of
    // the payload (file system metadata).
    std::uint64_t capacityMarginPercent = 10;
    // The number of attempts per file and the pause between them.
    int attempts = 3;
    int retryDelayMs = 1000;
};

// Walks the source tree once and collects the copy plan.
//
// Symbolic links are not followed: FAT32 cannot store a link, and
// traversing links is the classic way into an endless directory loop.
// A symbolic link to a *file* is copied with the content of its target
// (like robocopy does), a symbolic link to a directory is reported as
// skipped.
Result<CopyPlan> enumerateCopyPlan(const std::filesystem::path& source,
                                    const std::function<bool()>& cancelRequested = nullptr,
                                    const CopyProgressCallback& progress = nullptr);

// Copies the collected plan to the destination.
//
// The safety checks run before the first byte is written: a file that is
// larger than the destination file system can hold is rejected, and the
// destination needs enough free space for the payload plus the margin.
// destinationFreeBytes == 0 skips the capacity check (the free space is
// unknown).
Result<CopyStats> copyCopyPlan(const CopyPlan& plan,
                                const std::filesystem::path& destination,
                                std::uint64_t destinationFreeBytes = 0,
                                const CopyLimits& limits = CopyLimits(),
                                const std::function<bool()>& cancelRequested = nullptr,
                                const CopyProgressCallback& progress = nullptr);

}  // namespace ulli::core
