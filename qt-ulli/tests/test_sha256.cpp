// tests/test_sha256.cpp
// SHA-256 verification tests. These tests are only compiled on Windows
// where the platform::windows::Sha256 implementation is available.

#if defined(Q_OS_WIN)

#include "platform/windows/Sha256.h"

#include <QDir>
#include <QFile>
#include <QtTest/QtTest>

class TestSha256 : public QObject {
    Q_OBJECT
private slots:
    void verifyFile_notFound() {
        std::filesystem::path missing("/nonexistent/path/that/does/not/exist.iso");
        auto r = ulli::platform::windows::Sha256::verifyFile(missing, "anysha256");
        QVERIFY(!r);
        QCOMPARE(r.error().kind(), ulli::core::Error::Kind::NotFound);
    }
};

QTEST_GUILESS_MAIN(TestSha256)
#include "test_sha256.moc"

#endif // Q_OS_WIN