// platform/HttpDownload.h
//
// One HTTP(S) transfer with a bounded wait.
//
// The transfer runs its own event loop, so the caller keeps control of
// the surrounding flow. Two bounds are enforced:
//
//   * Cancellation is polled by a watchdog timer. It is therefore
//     observed even when the connection is stalled and neither a progress
//     nor a data signal arrives at all.
//   * An inactivity timeout aborts a transfer that stopped making
//     progress. It is an *inactivity* limit and not a total limit: a slow
//     but progressing download of a multi gigabyte ISO is never cut off.
//
// The bytes are written to "<destPath>.part". The caller publishes the
// file under its final name only after it has been verified, and the
// temporary file is removed on every failure path, so a partial transfer
// can never be mistaken for a complete one.

#pragma once

#include <QString>
#include <QUrl>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace ulli::platform {

// Bounds for a single transfer.
struct HttpDownloadLimits {
    // No bytes received for this long aborts the transfer. 0 disables
    // the inactivity timeout.
    int inactivityTimeoutMs = 60000;
    // How often the watchdog wakes up. This is also the upper bound for
    // how long a cancellation request can stay unnoticed.
    int pollIntervalMs = 250;
};

enum class HttpDownloadOutcome {
    Completed,  // the whole body was received
    Cancelled,  // the caller asked for cancellation
    TimedOut,   // no progress within the inactivity timeout
    Failed,     // network error or a local I/O error
};

struct HttpDownloadResult {
    HttpDownloadOutcome outcome = HttpDownloadOutcome::Failed;
    QString errorMessage;
    qint64 bytesReceived = 0;
    qint64 totalBytes = -1;  // -1 when the server announced no size

    bool ok() const { return outcome == HttpDownloadOutcome::Completed; }
};

// Downloads `url` into `destPath`. The payload is written to
// "<destPath>.part"; the caller renames it after a successful
// verification. `progress` is called with (percent, status) while the
// transfer runs.
HttpDownloadResult httpDownload(
    const QUrl& url,
    const std::filesystem::path& destPath,
    const std::function<void(int percent, const QString& status)>& progress,
    const std::atomic<bool>* cancelFlag,
    const HttpDownloadLimits& limits = HttpDownloadLimits());

}  // namespace ulli::platform
