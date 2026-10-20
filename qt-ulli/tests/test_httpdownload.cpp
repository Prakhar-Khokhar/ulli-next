// tests/test_httpdownload.cpp
//
// Tests for the bounded HTTP transfer.
//
// A stalled transfer has to end on its own: the cancellation request must
// be noticed even when not a single progress or data signal arrives, and
// an inactivity timeout must stop a transfer that stopped making
// progress. A partial transfer must never be left behind as if it were
// complete.
//
// The tests talk to a local server on the loopback interface. No test
// depends on internet access.

#include "platform/HttpDownload.h"

#include <QtTest/QtTest>

#include <QByteArray>
#include <QFile>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

using ulli::platform::HttpDownloadLimits;
using ulli::platform::HttpDownloadOutcome;

namespace {

// A loopback server that behaves exactly as the test asks for.
class StubServer : public QObject {
public:
    enum class Behaviour {
        Stall,      // accept the connection and never send anything
        SendBody,   // send the body and close
        Truncate,   // announce more bytes than are sent, then close
    };

    StubServer(Behaviour behaviour, QByteArray body = QByteArray())
        : behaviour_(behaviour), body_(std::move(body)) {}

    bool listen() {
        if (!server_.listen(QHostAddress::LocalHost, 0)) return false;
        QObject::connect(&server_, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket* socket = server_.nextPendingConnection();
            if (!socket) return;
            socket->setParent(&server_);
            if (behaviour_ == Behaviour::Stall) {
                // Accept the connection and stay silent: the client has to
                // notice the stall on its own.
                return;
            }
            QByteArray payload = body_;
            if (behaviour_ == Behaviour::Truncate) payload = payload.left(4);
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: " +
                          QByteArray::number(body_.size()) + "\r\n\r\n" + payload);
            socket->flush();
            socket->disconnectFromHost();
        });
        return true;
    }

    QUrl url() const {
        return QUrl(QString("http://127.0.0.1:%1/ulli-test.iso")
                        .arg(server_.serverPort()));
    }

private:
    QTcpServer server_;
    Behaviour behaviour_ = Behaviour::Stall;
    QByteArray body_;
};

QByteArray readFile(const std::filesystem::path& path) {
    QFile f(QString::fromStdString(path.string()));
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

}  // namespace

class TestHttpDownload : public QObject {
    Q_OBJECT

private slots:
    void stalledTransferTimesOut();
    void stalledTransferIsCancelled();
    void successfulTransferWritesThePayload();
    void truncatedTransferFails();
};

void TestHttpDownload::stalledTransferTimesOut() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto dest = std::filesystem::path(dir.path().toStdString()) / "test.iso";

    StubServer server(StubServer::Behaviour::Stall);
    QVERIFY(server.listen());

    HttpDownloadLimits limits;
    limits.inactivityTimeoutMs = 500;
    limits.pollIntervalMs = 50;

    const auto result = ulli::platform::httpDownload(server.url(), dest, nullptr,
                                                     nullptr, limits);
    QCOMPARE(result.outcome, HttpDownloadOutcome::TimedOut);
    QVERIFY2(!result.errorMessage.isEmpty(),
             "the timeout must produce a distinct diagnostic");
    // The partial file must be gone: it can never be mistaken for a
    // complete download, and the destination is published by the caller.
    QVERIFY(!std::filesystem::exists(dest.string() + ".part"));
    QVERIFY(!std::filesystem::exists(dest));
}

void TestHttpDownload::stalledTransferIsCancelled() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto dest = std::filesystem::path(dir.path().toStdString()) / "test.iso";

    StubServer server(StubServer::Behaviour::Stall);
    QVERIFY(server.listen());

    // The request is raised while the transfer is already running and
    // stalled: no progress or data signal will ever arrive.
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    QTimer::singleShot(200, [cancel]() { cancel->store(true); });

    HttpDownloadLimits limits;
    limits.inactivityTimeoutMs = 30000;  // the inactivity timeout must not fire
    limits.pollIntervalMs = 50;

    const auto result = ulli::platform::httpDownload(server.url(), dest, nullptr,
                                                     cancel.get(), limits);
    QCOMPARE(result.outcome, HttpDownloadOutcome::Cancelled);
    QVERIFY(!std::filesystem::exists(dest.string() + ".part"));
    QVERIFY(!std::filesystem::exists(dest));
}

void TestHttpDownload::successfulTransferWritesThePayload() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto dest = std::filesystem::path(dir.path().toStdString()) / "test.iso";

    const QByteArray payload("ULLI test payload for the bounded download");
    StubServer server(StubServer::Behaviour::SendBody, payload);
    QVERIFY(server.listen());

    const auto result = ulli::platform::httpDownload(server.url(), dest, nullptr,
                                                     nullptr, HttpDownloadLimits());
    QCOMPARE(result.outcome, HttpDownloadOutcome::Completed);
    QVERIFY(result.errorMessage.isEmpty());
    QCOMPARE(result.bytesReceived, static_cast<qint64>(payload.size()));

    // The payload sits in the temporary file; publishing it under the
    // final name is the caller's job (it verifies the checksum first).
    const auto part = std::filesystem::path(dest.string() + ".part");
    QVERIFY(std::filesystem::exists(part));
    QCOMPARE(readFile(part), payload);
    QVERIFY(!std::filesystem::exists(dest));
}

void TestHttpDownload::truncatedTransferFails() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto dest = std::filesystem::path(dir.path().toStdString()) / "test.iso";

    const QByteArray payload("ULLI test payload that never arrives completely");
    StubServer server(StubServer::Behaviour::Truncate, payload);
    QVERIFY(server.listen());

    const auto result = ulli::platform::httpDownload(server.url(), dest, nullptr,
                                                     nullptr, HttpDownloadLimits());
    QCOMPARE(result.outcome, HttpDownloadOutcome::Failed);
    QVERIFY2(!result.errorMessage.isEmpty(), "the failure must be reported");
    // A truncated transfer must not leave a file behind that could be
    // mistaken for a complete ISO.
    QVERIFY(!std::filesystem::exists(dest.string() + ".part"));
    QVERIFY(!std::filesystem::exists(dest));
}

QTEST_MAIN(TestHttpDownload)
#include "test_httpdownload.moc"
