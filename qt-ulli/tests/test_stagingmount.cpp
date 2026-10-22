// tests/test_stagingmount.cpp
//
// Tests for the mount root of the staging partition.
//
// The drive letter of the staging partition is never guessed: Windows
// assigns one and the code reads it back from the partition. These tests
// cover the decision that turns the verified letter into the root path
// the file copy uses - and the diagnostic for a partition that has no
// usable letter.
//
// The tests are platform independent: no partition is created and no
// disk is touched.

#include "core/StagingMount.h"

#include <QtTest/QtTest>

using ulli::core::decideStagingMount;
using ulli::core::stagingMountErrorMessage;
using ulli::core::stagingMountRoot;

class TestStagingMount : public QObject {
    Q_OBJECT

private slots:
    void theVerifiedLetterBecomesTheRoot();
    void aLetterOtherThanTheCandidateIsUsed();
    void withoutAVerifiedLetterTheMountFails();
    void theDiagnosticNamesDiskPartitionAndOutput();
    void anInvalidLetterIsNotAUsableRoot();
};

void TestStagingMount::theVerifiedLetterBecomesTheRoot() {
    QCOMPARE(stagingMountRoot('D'), std::string("D:\\"));

    const auto mount = decideStagingMount(0, 2, 'D', 'D', "");
    QVERIFY(mount.ok);
    QCOMPARE(mount.root, std::string("D:\\"));
    QVERIFY(mount.errorMessage.empty());
}

void TestStagingMount::aLetterOtherThanTheCandidateIsUsed() {
    // Windows picked 'D' although 'Z' was the candidate. The verified
    // letter wins - a guessed one would point at a partition that does
    // not exist.
    const auto mount = decideStagingMount(1, 3, 'D', 'Z', "");
    QVERIFY(mount.ok);
    QCOMPARE(mount.root, std::string("D:\\"));
}

void TestStagingMount::withoutAVerifiedLetterTheMountFails() {
    // The verification did not yield a letter: the copy has no directory
    // to write to, so the installation must stop.
    const auto mount = decideStagingMount(0, 2, '\0', 'Z', "no volume found");
    QVERIFY(!mount.ok);
    QVERIFY(mount.root.empty());
    QVERIFY(!mount.errorMessage.empty());
}

void TestStagingMount::theDiagnosticNamesDiskPartitionAndOutput() {
    const std::string message =
        stagingMountErrorMessage(2, 5, 'Z', "Add-PartitionAccessImage failed");
    QVERIFY(message.find("2") != std::string::npos);
    QVERIFY(message.find("5") != std::string::npos);
    QVERIFY(message.find('Z') != std::string::npos);
    QVERIFY(message.find("Add-PartitionAccessImage failed") != std::string::npos);
}

void TestStagingMount::anInvalidLetterIsNotAUsableRoot() {
    QVERIFY(stagingMountRoot(0).empty());
    QVERIFY(stagingMountRoot('z').empty());
    QVERIFY(stagingMountRoot('1').empty());

    const auto mount = decideStagingMount(0, 2, 'z', 0, "");
    QVERIFY(!mount.ok);
}

QTEST_MAIN(TestStagingMount)
#include "test_stagingmount.moc"
