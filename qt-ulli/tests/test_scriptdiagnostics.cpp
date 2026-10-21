// tests/test_scriptdiagnostics.cpp
//
// Tests for the diagnostics of a failed helper script.
//
// A script can report its problem on standard error (the errors of
// PowerShell itself) or on standard output (the scripts of
// DiskOps::createLayout() write a JSON error object there and exit with
// 1). Both streams have to end up in the message, and a stream without
// text must not produce an empty label.
//
// The formatting is platform independent, so the test runs everywhere and
// needs neither PowerShell nor a Windows host.

#include "core/ScriptDiagnostics.h"

#include <QtTest/QtTest>

#include <cstddef>
#include <string>

namespace {

// Counts how often a needle occurs in the haystack.
std::size_t countOccurrences(const std::string& haystack,
                             const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = haystack.find(needle);
    while (pos != std::string::npos) {
        ++count;
        pos = haystack.find(needle, pos + needle.size());
    }
    return count;
}

}  // namespace

class TestScriptDiagnostics : public QObject {
    Q_OBJECT

private slots:
    void bothStreamsAreReported();
    void onlyStandardErrorIsReported();
    void onlyStandardOutputIsReported();
    void emptyStreamsAreReportedAsNoOutput();
    void whitespaceOnlyStreamsCountAsEmpty();
    void theStreamsAreNotDuplicated();
};

void TestScriptDiagnostics::bothStreamsAreReported() {
    const std::string joined = ulli::core::joinScriptOutput(
        QStringLiteral("Exception: something went wrong"),
        QStringLiteral("{\"error\":\"No suitable free region found\"}"));
    QVERIFY(joined.find("something went wrong") != std::string::npos);
    QVERIFY(joined.find("No suitable free region found") != std::string::npos);
    QVERIFY(joined.find("stderr:") != std::string::npos);
    QVERIFY(joined.find("stdout:") != std::string::npos);
}

void TestScriptDiagnostics::onlyStandardErrorIsReported() {
    const std::string joined = ulli::core::joinScriptOutput(
        QStringLiteral("Access is denied."), QString());
    QCOMPARE(joined, std::string("stderr: Access is denied."));
    // No empty label for the stream that had nothing to report.
    QVERIFY(joined.find("stdout") == std::string::npos);
}

void TestScriptDiagnostics::onlyStandardOutputIsReported() {
    // This is the case that used to be lost completely.
    const std::string joined = ulli::core::joinScriptOutput(
        QString(), QStringLiteral("{\"error\":\"Failed to create staging partition\"}"));
    QCOMPARE(joined,
             std::string("stdout: {\"error\":\"Failed to create staging partition\"}"));
    QVERIFY(joined.find("stderr") == std::string::npos);
}

void TestScriptDiagnostics::emptyStreamsAreReportedAsNoOutput() {
    const std::string joined = ulli::core::joinScriptOutput(QString(), QString());
    QCOMPARE(joined, std::string("(no output)"));
    QVERIFY(joined.find("stderr") == std::string::npos);
    QVERIFY(joined.find("stdout") == std::string::npos);
}

void TestScriptDiagnostics::whitespaceOnlyStreamsCountAsEmpty() {
    const std::string joined = ulli::core::joinScriptOutput(
        QStringLiteral("  \r\n "), QStringLiteral("\t\n"));
    QCOMPARE(joined, std::string("(no output)"));
}

void TestScriptDiagnostics::theStreamsAreNotDuplicated() {
    // Every stream is reported exactly once, and not twice.
    const std::string joined = ulli::core::joinScriptOutput(
        QStringLiteral("first stream"), QStringLiteral("second stream"));
    QCOMPARE(countOccurrences(joined, "stderr:"), std::size_t(1));
    QCOMPARE(countOccurrences(joined, "stdout:"), std::size_t(1));
    QCOMPARE(countOccurrences(joined, "first stream"), std::size_t(1));
    QCOMPARE(countOccurrences(joined, "second stream"), std::size_t(1));
}

QTEST_MAIN(TestScriptDiagnostics)
#include "test_scriptdiagnostics.moc"
