// platform/HttpDownload.cpp
#include "platform/HttpDownload.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStorageInfo>
#include <QTimer>

#include <algorithm>
#include <string>

namespace ulli::platform {

namespace {

constexpr int kChunkSize = 256 * 1024;
constexpr int kProgressThrottleMs = 250;
constexpr qint64 kFreeSpaceHeadroomBytes = 64LL * 1024 * 1024;
// Upper bound for the wait until an aborted reply has finished.
constexpr int kSettleTimeoutMs = 2000;

}  // namespace

HttpDownloadResult httpDownload(
    const QUrl& url,
    const std::filesystem::path& destPath,
    const std::function<void(int percent, const QString& status)>& progress,
    const std::atomic<bool>* cancelFlag,
    const HttpDownloadLimits& limits) {

    HttpDownloadResult result;

    const auto cancelled = [cancelFlag]() {
        return cancelFlag != nullptr && cancelFlag->load();
    };

    // The payload is written next to the destination and published by the
    // caller only after it has been verified. A partial transfer is
    // therefore never visible as a cached ISO.
    const std::filesystem::path partPath(destPath.string() + ".part");
    const auto removePart = [&]() {
        std::error_code e;
        std::filesystem::remove(partPath, e);
    };

    QFile file(QString::fromStdString(partPath.string()));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        result.outcome = HttpDownloadOutcome::Failed;
        result.errorMessage = QString::fromStdString(
            "Cannot open temporary file for writing: " + partPath.string());
        return result;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "ULLI/1.0");
    QNetworkReply* reply = manager.get(request);

    QEventLoop loop;
    QTimer watchdog;
    QElapsedTimer lastActivity;
    lastActivity.start();
    QElapsedTimer throttle;
    throttle.start();

    bool replyFinished = false;
    bool aborted = false;
    bool timedOut = false;
    bool writeFailed = false;
    QString writeError;
    bool freeSpaceFailed = false;
    QString freeSpaceError;
    int lastPercent = -1;

    // Every failure path removes the partial file, so nothing half
    // written can be mistaken for a complete download later on.
    // The reply belongs to the network access manager and is destroyed
    // with it: giving it a deferred delete would leave it alive until
    // the outer event loop runs.
    const auto fail = [&](HttpDownloadOutcome outcome, const QString& message) {
        removePart();
        result.outcome = outcome;
        result.errorMessage = message;
        return result;
    };

    // Moves everything the reply has buffered into the file. It is called
    // on every readyRead and once more when the reply finishes, so the
    // last chunk is never left in the network buffer.
    const auto drain = [&]() {
        while (reply->bytesAvailable() > 0) {
            const QByteArray chunk = reply->read(kChunkSize);
            if (chunk.isEmpty()) break;
            const qint64 written = file.write(chunk);
            if (written != chunk.size()) {
                writeFailed = true;
                writeError = QString::fromStdString(
                    "Short write to temporary file: " + partPath.string());
                return;
            }
            result.bytesReceived += chunk.size();
        }
    };

    // All handlers use the event loop as context object: the loop is
    // destroyed when this function returns, which removes the connections.
    // A late signal of the reply can therefore never touch a destroyed
    // local object.
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&]() {
        lastActivity.restart();
        if (cancelled()) {
            aborted = true;
            reply->abort();
            loop.quit();
            return;
        }
        drain();
        if (writeFailed) {
            reply->abort();
            loop.quit();
        }
    });

    QObject::connect(reply, &QNetworkReply::downloadProgress, &loop,
        [&](qint64 bytesReceived, qint64 bytesTotal) {
            lastActivity.restart();
            if (cancelled()) {
                aborted = true;
                reply->abort();
                loop.quit();
                return;
            }
            if (bytesTotal <= 0) return;
            if (result.totalBytes < 0) {
                result.totalBytes = bytesTotal;
                const QStorageInfo storage(
                    QString::fromStdString(destPath.parent_path().string()));
                if (storage.bytesAvailable() > 0) {
                    const qint64 needed = bytesTotal + kFreeSpaceHeadroomBytes;
                    if (storage.bytesAvailable() < needed) {
                        freeSpaceFailed = true;
                        freeSpaceError = QString::fromStdString(
                            "Not enough free space in " +
                            destPath.parent_path().string() + ": need " +
                            std::to_string(needed) + ", have " +
                            std::to_string(storage.bytesAvailable()));
                        reply->abort();
                        loop.quit();
                        return;
                    }
                }
            }
            const int percent = static_cast<int>((bytesReceived * 100) / bytesTotal);
            if (percent != lastPercent && throttle.elapsed() >= kProgressThrottleMs) {
                lastPercent = percent;
                throttle.restart();
                if (progress) {
                    progress(percent, QString("Downloading: %1 / %2 MB")
                        .arg(bytesReceived / (1024 * 1024))
                        .arg(bytesTotal / (1024 * 1024)));
                }
            }
        });

    QObject::connect(reply, &QNetworkReply::finished, &loop, [&]() {
        // The last bytes may still be buffered when the reply finishes.
        drain();
        replyFinished = true;
        loop.quit();
    });

    // The watchdog bounds the wait. It observes the cancellation request
    // even when the connection is stalled and no signal arrives, and it
    // aborts a transfer that stopped making progress.
    watchdog.setInterval(std::max(50, limits.pollIntervalMs));
    QObject::connect(&watchdog, &QTimer::timeout, &loop, [&]() {
        if (replyFinished || aborted) return;
        if (cancelled()) {
            aborted = true;
            reply->abort();
            loop.quit();
            return;
        }
        if (limits.inactivityTimeoutMs > 0 &&
            lastActivity.elapsed() > limits.inactivityTimeoutMs) {
            timedOut = true;
            aborted = true;
            reply->abort();
            loop.quit();
        }
    });
    watchdog.start();

    loop.exec();
    watchdog.stop();

    // The reply must not be running any more when the manager goes away:
    // an in-flight request would be torn down with it. The abort is
    // asynchronous, so the reply is given a bounded moment to finish.
    // The handlers are bound to the event loop, which is destroyed when
    // this function returns, so a late signal cannot touch a destroyed
    // local object.
    if (!replyFinished) {
        reply->abort();
        QEventLoop settle;
        QTimer guard;
        guard.setSingleShot(true);
        guard.setInterval(kSettleTimeoutMs);
        QObject::connect(&guard, &QTimer::timeout, &settle, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::finished, &settle, &QEventLoop::quit);
        guard.start();
        settle.exec();
    }

    file.close();

    if (timedOut) {
        return fail(HttpDownloadOutcome::TimedOut,
            QString("No data received for %1 s - the server stopped responding.")
                .arg(limits.inactivityTimeoutMs / 1000));
    }
    if (aborted || cancelled()) {
        return fail(HttpDownloadOutcome::Cancelled, "Download cancelled");
    }
    if (writeFailed) {
        return fail(HttpDownloadOutcome::Failed, writeError);
    }
    if (freeSpaceFailed) {
        return fail(HttpDownloadOutcome::Failed, freeSpaceError);
    }
    if (reply->error() != QNetworkReply::NoError) {
        const QString err = reply->errorString();
        if (progress) progress(0, QString("Mirror failed: %1").arg(err));
        return fail(HttpDownloadOutcome::Failed, err);
    }
    if (result.bytesReceived <= 0) {
        // An empty body is not a download. Publishing it would leave an
        // unusable file behind that looks like a finished transfer.
        return fail(HttpDownloadOutcome::Failed,
                    "The server returned an empty response.");
    }

    result.outcome = HttpDownloadOutcome::Completed;
    return result;
}

}  // namespace ulli::platform
