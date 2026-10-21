// core/FileCopier.cpp
#include "core/FileCopier.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace ulli::core {

namespace {

constexpr const char* kPhaseEnumerating = "Enumerating ISO contents";
constexpr const char* kPhaseCopying = "Copying ISO contents";
constexpr const char* kPhaseFinalizing = "Finalizing the copy";

// How often the enumeration reports its progress. Every entry would
// flood the log on an ISO with thousands of files.
constexpr std::uint64_t kReportEveryEntries = 100;

void report(const CopyProgressCallback& progress, int percent,
            const char* phase, const std::string& detail) {
    if (progress) progress(percent, phase, detail);
}

}  // namespace

Result<CopyPlan> enumerateCopyPlan(const std::filesystem::path& source,
                                    const std::function<bool()>& cancelRequested,
                                    const CopyProgressCallback& progress) {
    CopyPlan plan;

    if (cancelRequested && cancelRequested()) {
        return makeCancelled<CopyPlan>();
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(source, ec) || ec) {
        return makeError<CopyPlan>(Error::Kind::NotFound,
            "Source directory is not accessible: " + source.string());
    }

    // directory_options::none: symbolic links to directories are listed
    // but not descended into.
    std::filesystem::recursive_directory_iterator it(
        source, std::filesystem::directory_options::none, ec);
    if (ec) {
        return makeError<CopyPlan>(Error::Kind::Io,
            "Cannot read source directory: " + source.string() +
            " (" + ec.message() + ")");
    }

    const std::filesystem::recursive_directory_iterator end;
    std::uint64_t seen = 0;
    while (it != end) {
        if (cancelRequested && cancelRequested()) {
            return makeCancelled<CopyPlan>();
        }

        const std::filesystem::path entryPath = it->path();
        std::error_code statusEc;
        const std::filesystem::file_status status = it->symlink_status(statusEc);
        const bool isLink = !statusEc && std::filesystem::is_symlink(status);

        CopyEntry entry;
        entry.source = entryPath;
        std::error_code relEc;
        entry.relative = std::filesystem::relative(entryPath, source, relEc);
        if (relEc) {
            entry.relative = entryPath.filename();
        }

        if (isLink) {
            // A link to a directory is not traversed: the destination file
            // system cannot store a link, and following links is the
            // classic way into an endless directory loop. It is reported
            // instead of being skipped silently.
            std::error_code targetEc;
            const bool linksToDirectory =
                std::filesystem::is_directory(it->status(targetEc)) && !targetEc;
            if (linksToDirectory) {
                plan.skipped.push_back(entry.relative.string());
                report(progress, 0, kPhaseEnumerating,
                       "Symbolic link to a directory is not copied: " +
                       entry.relative.string());
            } else {
                // A link to a file is copied with the content of its
                // target, like robocopy does: FAT32 cannot store the
                // link itself and the boot loader needs a real file.
                std::error_code sizeEc;
                const std::uintmax_t size = it->file_size(sizeEc);
                if (sizeEc) {
                    plan.skipped.push_back(entry.relative.string());
                } else {
                    entry.size = static_cast<std::uint64_t>(size);
                    plan.files.push_back(entry);
                    plan.totalBytes += entry.size;
                    plan.largestFileBytes = std::max(plan.largestFileBytes, entry.size);
                    ++plan.fileCount;
                }
            }
        } else if (!statusEc && std::filesystem::is_directory(status)) {
            entry.isDirectory = true;
            plan.directories.push_back(entry);
        } else if (!statusEc && std::filesystem::is_regular_file(status)) {
            std::error_code sizeEc;
            const std::uintmax_t size = it->file_size(sizeEc);
            if (sizeEc) {
                plan.skipped.push_back(entry.relative.string());
                report(progress, 0, kPhaseEnumerating,
                       "Could not read the size of '" + entry.relative.string() +
                       "', it is not copied");
            } else {
                entry.size = static_cast<std::uint64_t>(size);
                plan.files.push_back(entry);
                plan.totalBytes += entry.size;
                plan.largestFileBytes = std::max(plan.largestFileBytes, entry.size);
                ++plan.fileCount;
            }
        } else if (!statusEc) {
            // Sockets, fifos and device nodes have no meaning on the
            // destination.
            plan.skipped.push_back(entry.relative.string());
        }

        ++seen;
        if (seen % kReportEveryEntries == 0) {
            report(progress, 0, kPhaseEnumerating,
                   "Enumerated " + std::to_string(plan.fileCount) +
                   " files (" + std::to_string(plan.totalBytes / (1024 * 1024)) +
                   " MiB), current: " + entry.relative.string());
        }

        std::error_code stepEc;
        it.increment(stepEc);
        if (stepEc) {
            // A directory that cannot be read must not abort the whole
            // copy, but it is reported.
            plan.skipped.push_back(entryPath.string());
            report(progress, 0, kPhaseEnumerating,
                   "Could not read '" + entryPath.string() + "': " +
                   stepEc.message());
        }
    }

    report(progress, 100, kPhaseEnumerating,
           "Enumerated " + std::to_string(plan.fileCount) + " files (" +
           std::to_string(plan.totalBytes / (1024 * 1024)) + " MiB) in " +
           std::to_string(plan.directories.size()) + " directories");
    if (!plan.skipped.empty()) {
        report(progress, 100, kPhaseEnumerating,
               std::to_string(plan.skipped.size()) +
               " entries are not copied (symbolic links or unreadable)");
    }
    return makeOk(plan);
}

Result<CopyStats> copyCopyPlan(const CopyPlan& plan,
                                const std::filesystem::path& destination,
                                std::uint64_t destinationFreeBytes,
                                const CopyLimits& limits,
                                const std::function<bool()>& cancelRequested,
                                const CopyProgressCallback& progress) {
    CopyStats stats;

    if (cancelRequested && cancelRequested()) {
        return makeCancelled<CopyStats>();
    }

    // ─── Safety checks, before the first byte is written ──────────────
    if (limits.maxFileSizeBytes > 0 && plan.largestFileBytes > limits.maxFileSizeBytes) {
        return makeError<CopyStats>(Error::Kind::Platform,
            "ISO contains a file larger than the 4 GiB limit of the destination "
            "file system: " + std::to_string(plan.largestFileBytes) +
            " bytes. Cannot copy to FAT32 staging.");
    }

    if (destinationFreeBytes > 0) {
        const std::uint64_t margin =
            plan.totalBytes * limits.capacityMarginPercent / 100;
        const std::uint64_t required = plan.totalBytes + margin;
        if (destinationFreeBytes < required) {
            return makeError<CopyStats>(Error::Kind::Platform,
                "Insufficient space on the staging partition. Need ~" +
                std::to_string(required) + " bytes, have " +
                std::to_string(destinationFreeBytes) + " bytes free.");
        }
    }

    // ─── Directories ──────────────────────────────────────────────────
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    if (ec) {
        return makeError<CopyStats>(Error::Kind::Io,
            "Cannot create destination directory: " + destination.string() +
            " (" + ec.message() + ")");
    }

    for (const CopyEntry& entry : plan.directories) {
        if (cancelRequested && cancelRequested()) {
            return makeCancelled<CopyStats>();
        }
        const std::filesystem::path target = destination / entry.relative;
        std::error_code dirEc;
        std::filesystem::create_directories(target, dirEc);
        if (dirEc) {
            return makeError<CopyStats>(Error::Kind::Io,
                "Cannot create directory '" + entry.relative.string() +
                "' (" + dirEc.message() + ")");
        }
        ++stats.directoriesCreated;
    }

    // ─── Files ─────────────────────────────────────────────────────────
    std::uint64_t copiedBytes = 0;
    for (const CopyEntry& entry : plan.files) {
        if (cancelRequested && cancelRequested()) {
            return makeCancelled<CopyStats>();
        }

        const std::filesystem::path target = destination / entry.relative;
        bool copied = false;
        std::error_code lastError;
        for (int attempt = 1; attempt <= std::max(1, limits.attempts); ++attempt) {
            if (cancelRequested && cancelRequested()) {
                return makeCancelled<CopyStats>();
            }
            std::error_code copyEc;
            std::filesystem::copy_file(entry.source, target,
                                       std::filesystem::copy_options::overwrite_existing,
                                       copyEc);
            if (!copyEc) {
                copied = true;
                break;
            }
            lastError = copyEc;
            if (attempt < limits.attempts) {
                // A file that is still held open (antivirus scanner,
                // indexer) fails with a sharing violation. Retry a few
                // times, like robocopy does.
                std::this_thread::sleep_for(std::chrono::milliseconds(limits.retryDelayMs));
            }
        }
        if (!copied) {
            return makeError<CopyStats>(Error::Kind::Io,
                "Failed to copy '" + entry.relative.string() + "' to '" +
                target.string() + "': " + lastError.message());
        }

        // The files of an ISO are read-only. The boot configuration is
        // patched on the staging partition afterwards, so the copies have
        // to be writable.
        std::error_code permEc;
        std::filesystem::permissions(target,
                                     std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add, permEc);
        if (permEc) {
            report(progress, 0, kPhaseCopying,
                   "Could not clear the read-only attribute of '" +
                   entry.relative.string() + "': " + permEc.message());
        }

        // Keep the modification time of the original, like robocopy does.
        std::error_code timeEc;
        const auto writeTime = std::filesystem::last_write_time(entry.source, timeEc);
        if (!timeEc) {
            std::error_code setTimeEc;
            std::filesystem::last_write_time(target, writeTime, setTimeEc);
        }

        ++stats.filesCopied;
        copiedBytes += entry.size;
        stats.bytesCopied = copiedBytes;

        const int percent = plan.totalBytes > 0
            ? static_cast<int>((copiedBytes * 100) / plan.totalBytes)
            : 0;
        report(progress, percent, kPhaseCopying,
               "Copied '" + entry.relative.string() + "' (" +
               std::to_string(entry.size / (1024 * 1024)) + " MiB)");
    }

    report(progress, 100, kPhaseFinalizing,
           "Copied " + std::to_string(stats.filesCopied) + " files (" +
           std::to_string(stats.bytesCopied / (1024 * 1024)) + " MiB)");
    return makeOk(stats);
}

}  // namespace ulli::core
