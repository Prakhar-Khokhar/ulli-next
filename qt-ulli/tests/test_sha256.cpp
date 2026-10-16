// tests/test_sha256.cpp
// SHA-256 verification tests. These tests are only compiled on Windows
// where the platform::windows::Sha256 implementation is available.

#include <QtGlobal>

#if defined(Q_OS_WIN)

#include "platform/windows/Sha256.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <filesystem>

#include <iostream>

class TestSha256 : public QObject {
    Q_OBJECT
private slots:
    void verifyFile_notFound() {
        std::cerr << "verifyFile_notFound: running" << std::endl;
        std::filesystem::path missing("/nonexistent/path/that/does/not/exist.iso");
        auto r = ulli::platform::windows::Sha256::verifyFile(missing, "anysha256");
        QVERIFY(!r);
        QCOMPARE(r.error().kind(), ulli::core::Error::Kind::NotFound);
    }

    void verifyFile_mismatch() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path file = std::filesystem::path(tmp.path().toStdString()) / "test.bin";
        QFile f(QString::fromStdString(file.string()));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hello world");
        f.close();

        auto r = ulli::platform::windows::Sha256::verifyFile(file, "deadbeef");
        std::cerr << "verifyFile_mismatch: ok=" << r.isOk()
                  << " kind=" << static_cast<int>(r.error().kind())
                  << " msg=" << r.error().message() << std::endl;
        QVERIFY(!r);
        QCOMPARE(r.error().kind(), ulli::core::Error::Kind::Platform);
        QVERIFY(r.error().message().find("mismatch") != std::string::npos);
    }

    void verifyFile_match() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        std::filesystem::path file = std::filesystem::path(tmp.path().toStdString()) / "test.bin";
        QFile f(QString::fromStdString(file.string()));
        QVERIFY(f.open(QIODevice::WriteOnly));
        const QByteArray contents = "hello world";
        f.write(contents);
        f.close();

        QString expected = QString(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
        std::cerr << "verifyFile_match: expected=" << expected.toStdString()
                  << " file=" << file.string() << std::endl;
        auto r = ulli::platform::windows::Sha256::verifyFile(file, expected.toStdString());
        std::cerr << "verifyFile_match: ok=" << r.isOk()
                  << " kind=" << static_cast<int>(r.error().kind())
                  << " msg=" << r.error().message() << std::endl;
        QVERIFY(r);
    }
};

QTEST_GUILESS_MAIN(TestSha256)
#include "test_sha256.moc"

#endif // Q_OS_WIN