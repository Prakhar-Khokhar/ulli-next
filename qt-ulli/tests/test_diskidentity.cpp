// tests/test_diskidentity.cpp
//
// Tests for the disk identity policy: the approved device identity has to
// match the live enumeration before anything is written to a disk.
//
// No physical disk is touched: the tests only build the in-memory disk
// descriptions that the platform backend would produce.

#include "core/DiskIdentity.h"

#include <QtTest/QtTest>

using ulli::core::Disk;
using ulli::core::DiskIdentity;
using ulli::core::DiskIdentityMatch;

namespace {

// Builds a disk description like the platform enumeration does.
Disk makeDisk(int number, const QString& model, std::uint64_t sizeBytes,
              const QString& uniqueId, const QString& serial) {
    Disk disk;
    disk.number = number;
    disk.model = model.toStdString();
    disk.sizeBytes = sizeBytes;
    disk.identity.uniqueId = uniqueId.toStdString();
    disk.identity.serialNumber = serial.toStdString();
    return disk;
}

constexpr std::uint64_t kOneTb = 1000ull * 1024 * 1024 * 1024;

}  // namespace

class TestDiskIdentity : public QObject {
    Q_OBJECT

private slots:
    void matchingIdentityIsAccepted();
    void duplicateModelAndSizeAreToldApart();
    void missingIdentityIsRefused();
    void placeholderSerialIsTreatedAsAbsent();
    void changedDiskNumberIsRefused();
    void mismatchedIdentityIsRefused();
    void ambiguousIdentityIsRefused();
    void weakerIdentifierDoesNotOverrideStrongerOne();
    void formattingDifferencesAreTolerated();
    void differentIdentifiersDoNotCompareEqual();
    void normalizationRules();
};

void TestDiskIdentity::matchingIdentityIsAccepted() {
    const Disk live = makeDisk(2, "Samsung SSD 990 PRO", kOneTb,
                               "4C5311A60S5Z1NJ0R", "S5Z1NJ0R");
    DiskIdentity approved;
    approved.uniqueId = "4C5311A60S5Z1NJ0R";
    approved.serialNumber = "S5Z1NJ0R";

    const std::vector<Disk> liveDisks{live};
    const auto check = ulli::core::checkDiskIdentity(approved, 2, liveDisks);
    QVERIFY(check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::Match);
    QVERIFY(check.disk != nullptr);
    QCOMPARE(check.disk->number, 2);
}

void TestDiskIdentity::duplicateModelAndSizeAreToldApart() {
    // Two identical drives: same model, same capacity, different device.
    const Disk first = makeDisk(0, "WDC WD10EZEX", kOneTb,
                                "WD-WMC4N0K1234A", "WMC4N0K1234A");
    const Disk second = makeDisk(1, "WDC WD10EZEX", kOneTb,
                                 "WD-WMC4N0K5678B", "WMC4N0K5678B");

    // The plan was approved for the first one.
    DiskIdentity approved;
    approved.uniqueId = "WD-WMC4N0K1234A";
    approved.serialNumber = "WMC4N0K1234A";

    const std::vector<Disk> liveDisks{first, second};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(check.ok());
    QVERIFY(check.disk != nullptr);
    QCOMPARE(check.disk->number, 0);
    QCOMPARE(check.disk->identity.uniqueId, std::string("WD-WMC4N0K1234A"));

    // The same model and size must never be enough on their own: without
    // an identity the plan is refused.
    DiskIdentity withoutIdentity;
    const auto refused = ulli::core::checkDiskIdentity(withoutIdentity, 0,
                                                        liveDisks);
    QVERIFY(!refused.ok());
    QCOMPARE(refused.match, DiskIdentityMatch::NoIdentity);
    QVERIFY(!refused.detail.empty());
}

void TestDiskIdentity::missingIdentityIsRefused() {
    const Disk live = makeDisk(0, "WDC WD10EZEX", kOneTb, "", "");
    const std::vector<Disk> liveDisks{live};
    const auto check = ulli::core::checkDiskIdentity(DiskIdentity{}, 0, liveDisks);
    QVERIFY(!check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::NoIdentity);
    QVERIFY(check.disk == nullptr);
    QVERIFY(!check.detail.empty());
}

void TestDiskIdentity::placeholderSerialIsTreatedAsAbsent() {
    // A device that only reports the usual dummy serial is not
    // identifiable: the plan has to be refused instead of guessing.
    const Disk live = makeDisk(0, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                               "", "0000000000");
    DiskIdentity approved;
    approved.serialNumber = "0000000000";

    const std::vector<Disk> liveDisks{live};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(!check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::NoIdentity);
}

void TestDiskIdentity::changedDiskNumberIsRefused() {
    // Same device, but it was re-enumerated under a different number. The
    // plan was approved for disk 0 and the destructive commands address
    // the disk by number, so the plan has to be reviewed again.
    const Disk live = makeDisk(3, "Samsung SSD 990 PRO", kOneTb,
                               "4C5311A60S5Z1NJ0R", "S5Z1NJ0R");
    DiskIdentity approved;
    approved.uniqueId = "4C5311A60S5Z1NJ0R";

    const std::vector<Disk> liveDisks{live};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(!check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::DiskNumberChanged);
    QVERIFY(check.disk == nullptr);
    QVERIFY(check.detail.find("3") != std::string::npos);
}

void TestDiskIdentity::mismatchedIdentityIsRefused() {
    // A different device is now behind the approved disk number.
    const Disk live = makeDisk(0, "Samsung SSD 990 PRO", kOneTb,
                               "4C5311A60S5Z1NJ0R", "S5Z1NJ0R");
    DiskIdentity approved;
    approved.uniqueId = "4C5311A60S5Z1NJ0X";

    const std::vector<Disk> liveDisks{live};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(!check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::NotFound);
    QVERIFY(check.disk == nullptr);
}

void TestDiskIdentity::ambiguousIdentityIsRefused() {
    // Two devices report the same identifier: the target is not unique.
    const Disk first = makeDisk(0, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                                "4C5311644S3ZNB0K", "S3ZNB0K");
    const Disk second = makeDisk(1, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                                 "4C5311644S3ZNB0K", "S3ZNB0K");
    DiskIdentity approved;
    approved.uniqueId = "4C5311644S3ZNB0K";

    const std::vector<Disk> liveDisks{first, second};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(!check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::Ambiguous);
    QVERIFY(check.disk == nullptr);
}

void TestDiskIdentity::weakerIdentifierDoesNotOverrideStrongerOne() {
    // A different device that happens to report the same serial must not
    // be accepted: the unique id decides.
    const Disk impostor = makeDisk(0, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                                   "4C5311644OTHERID", "S3ZNB0K");
    DiskIdentity approved;
    approved.uniqueId = "4C5311644S3ZNB0K";
    approved.serialNumber = "S3ZNB0K";

    const std::vector<Disk> liveDisks{impostor};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(!check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::NotFound);
}

void TestDiskIdentity::formattingDifferencesAreTolerated() {
    const Disk live = makeDisk(0, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                               "4C5311644S3ZNB0K", "S3ZN NB0K");
    DiskIdentity approved;
    approved.serialNumber = "s3znnB0k";

    const std::vector<Disk> liveDisks{live};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::Match);
}

void TestDiskIdentity::differentIdentifiersDoNotCompareEqual() {
    const Disk live = makeDisk(0, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                               "4C5311644S3ZNB0K", "S3ZNB0K");
    DiskIdentity approved;
    approved.serialNumber = "S3ZNB0X";

    const std::vector<Disk> liveDisks{live};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(!check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::NotFound);
}

void TestDiskIdentity::normalizationRules() {
    QCOMPARE(ulli::core::normalizeDiskIdentifier(" s3zn-nb0_k "),
             std::string("S3ZNNB0K"));
    QCOMPARE(ulli::core::normalizeDiskIdentifier(""), std::string());
    QVERIFY(ulli::core::isPlaceholderDiskIdentifier(""));
    QVERIFY(ulli::core::isPlaceholderDiskIdentifier("0000000000"));
    QVERIFY(ulli::core::isPlaceholderDiskIdentifier("FFFFFFFF"));
    QVERIFY(ulli::core::isPlaceholderDiskIdentifier("NONE"));
    QVERIFY(ulli::core::isPlaceholderDiskIdentifier("TOBEFILLEDBYOEM"));
    QVERIFY(!ulli::core::isPlaceholderDiskIdentifier("S3ZNNB0K"));
    QVERIFY(!ulli::core::isPlaceholderDiskIdentifier("0A1B2C3D4E5F"));
}

QTEST_MAIN(TestDiskIdentity)
#include "test_diskidentity.moc"
