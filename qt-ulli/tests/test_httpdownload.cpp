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
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

using ulli::platform::HttpDownloadLimits;
using ulli::platform::HttpDownloadOutcome;

namespace {

// The test report is written to stdout, which is buffered. Flushing it
// keeps the report readable even when the binary dies later on.
void flushOutput() {
    std::fflush(stdout);
    std::fflush(stderr);
}

// A loopback server that behaves exactly as the test asks for.
class StubServer : public QTcpServer {
public:
    enum class Behaviour {
        Stall,      // read the request and never send anything
        SendBody,   // send the body and close
        Truncate,   // announce more bytes than are sent, then close
    };

    StubServer(Behaviour behaviour, QByteArray body = QByteArray())
        : behaviour_(behaviour), body_(std::move(body)) {}

    ~StubServer() override {
        // Abort the accepted sockets before the server disappears, so the
        // client sees an orderly shutdown. The sockets are children of
        // the server and are deleted with it.
        const QList<QTcpSocket*> sockets = sockets_;
        sockets_.clear();
        for (QTcpSocket* socket : sockets) {
            socket->abort();
        }
    }

    bool start() {
        if (!listen(QHostAddress::LocalHost, 0)) return false;
        connect(this, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket* socket = nextPendingConnection();
            if (!socket) return;
            sockets_.append(socket);
            // The response may only be sent once the request has arrived:
            // closing the connection earlier makes the client see a
            // truncated transfer.
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                socket->readAll();
                if (answered_.contains(socket)) return;
                answered_.insert(socket);
                if (behaviour_ == Behaviour::Stall) {
                    // Read the request and stay silent: the client has to
                    // notice the stall on its own.
                    return;
                }
                QByteArray payload = body_;
                if (behaviour_ == Behaviour::Truncate) payload = payload.left(4);
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " +
                              QByteArray::number(body_.size()) + "\r\n\r\n" + payload);
                socket->flush();
                // The response has to be on the wire before the connection
                // is closed, otherwise the client can lose it.
                socket->waitForBytesWritten(5000);
                socket->disconnectFromHost();
            });
        });
        return true;
    }

    QUrl url() const {
        return QUrl(QString("http://127.0.0.1:%1/ulli-test.iso")
                        .arg(serverPort()));
    }

private:
    Behaviour behaviour_ = Behaviour::Stall;
    QByteArray body_;
    QList<QTcpSocket*> sockets_;
    QSet<QTcpSocket*> answered_;
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
    void cleanupTestCase();
    void stalledTransferTimesOut();
    void stalledTransferIsCancelled();
    void successfulTransferWritesThePayload();
    void truncatedTransferFails();
};

void TestHttpDownload::cleanupTestCase() {
    // Flush the report before the process leaves the test binary.
    flushOutput();
}

void TestHttpDownload::stalledTransferTimesOut() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto dest = std::filesystem::path(dir.path().toStdString()) / "test.iso";

    StubServer server(StubServer::Behaviour::Stall);
    if (!server.start()) {
        QSKIP("no loopback TCP server available in this environment");
    }

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
    flushOutput();
}

void TestHttpDownload::stalledTransferIsCancelled() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto dest = std::filesystem::path(dir.path().toStdString()) / "test.iso";

    StubServer server(StubServer::Behaviour::Stall);
    if (!server.start()) {
        QSKIP("no loopback TCP server available in this environment");
    }

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
    flushOutput();
}

void TestHttpDownload::successfulTransferWritesThePayload() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto dest = std::filesystem::path(dir.path().toStdString()) / "test.iso";

    const QByteArray payload("ULLI test payload for the bounded download");
    StubServer server(StubServer::Behaviour::SendBody, payload);
    if (!server.start()) {
        QSKIP("no loopback TCP server available in this environment");
    }

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
    flushOutput();
}

void TestHttpDownload::truncatedTransferFails() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto dest = std::filesystem::path(dir.path().toStdString()) / "test.iso";

    const QByteArray payload("ULLI test payload that never arrives completely");
    StubServer server(StubServer::Behaviour::Truncate, payload);
    if (!server.start()) {
        QSKIP("no loopback TCP server available in this environment");
    }

    const auto result = ulli::platform::httpDownload(server.url(), dest, nullptr,
                                                     nullptr, HttpDownloadLimits());
    QCOMPARE(result.outcome, HttpDownloadOutcome::Failed);
    QVERIFY2(!result.errorMessage.isEmpty(), "the failure must be reported");
    // A truncated transfer must not leave a file behind that could be
    // mistaken for a complete ISO.
    QVERIFY(!std::filesystem::exists(dest.string() + ".part"));
    QVERIFY(!std::filesystem::exists(dest));
    flushOutput();
}

QTEST_MAIN(TestHttpDownload)
#include "test_httpdownload.moc"
