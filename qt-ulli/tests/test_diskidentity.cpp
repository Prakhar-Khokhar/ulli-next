// tests/test_diskidentity.cpp
//
// Tests for the disk identity policy: the approved device identity has to
// match the live enumeration before anything is written to a disk.
//
// The identifiers are the ones the Windows storage stack reports:
// UniqueId is built from the device identification descriptor (SCSI VPD
// page 0x83) and UniqueIdFormat names the descriptor type it came from
// ("Vendor Specific", "Vendor Id", "EUI64", "FCPH Name",
// "SCSI Name String"). The serial number comes from VPD page 0x80 or
// ATA IDENTIFY.
//
// No physical disk is touched: the tests only build the in-memory disk
// descriptions that the platform backend would produce.

#include "core/DiskIdentity.h"

#include <QtTest/QtTest>

using ulli::core::Disk;
using ulli::core::DiskIdentity;
using ulli::core::DiskIdentityMatch;

namespace {

// The descriptor types the Windows storage stack reports.
const QString kFormatVendorSpecific = QStringLiteral("Vendor Specific");
const QString kFormatVendorId = QStringLiteral("Vendor Id");
const QString kFormatEui64 = QStringLiteral("EUI64");
const QString kFormatFcphName = QStringLiteral("FCPH Name");
const QString kFormatScsiNameString = QStringLiteral("SCSI Name String");

// Builds a disk description like the platform enumeration does.
Disk makeDisk(int number, const QString& model, std::uint64_t sizeBytes,
              const QString& uniqueId, const QString& serial,
              const QString& uniqueIdFormat = QString()) {
    Disk disk;
    disk.number = number;
    disk.model = model.toStdString();
    disk.sizeBytes = sizeBytes;
    disk.identity.uniqueId = uniqueId.toStdString();
    disk.identity.uniqueIdFormat = uniqueIdFormat.toStdString();
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
    void hyphenPlacementDoesNotCollide();
    void underscorePlacementDoesNotCollide();
    void punctuationInScsiNameStaysSignificant();
    void incompatibleUniqueIdFormatsAreNotEquivalent();
    void missingFormatFallsBackToTheText();
    void knownFormatIsNotSilentlyLost();
    void numericZeroFormatIsAPresentFormat();
    void formatNamesAreComparedCaseInsensitively();
    void surroundingWhitespaceIsIgnored();
    void internalWhitespaceAndCaseAreSignificant();
    void differentIdentifiersDoNotCompareEqual();
    void normalizationRules();
};

void TestDiskIdentity::matchingIdentityIsAccepted() {
    const Disk live = makeDisk(2, "Samsung SSD 990 PRO", kOneTb,
                               "4C5311A60S5Z1NJ0R", "S5Z1NJ0R", kFormatEui64);
    DiskIdentity approved;
    approved.uniqueId = "4C5311A60S5Z1NJ0R";
    approved.uniqueIdFormat = kFormatEui64.toStdString();
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
                                "WD-WMC4N0K1234A", "WMC4N0K1234A",
                                kFormatScsiNameString);
    const Disk second = makeDisk(1, "WDC WD10EZEX", kOneTb,
                                 "WD-WMC4N0K5678B", "WMC4N0K5678B",
                                 kFormatScsiNameString);

    // The plan was approved for the first one.
    DiskIdentity approved;
    approved.uniqueId = "WD-WMC4N0K1234A";
    approved.uniqueIdFormat = kFormatScsiNameString.toStdString();
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
                               "4C5311A60S5Z1NJ0R", "S5Z1NJ0R", kFormatEui64);
    DiskIdentity approved;
    approved.uniqueId = "4C5311A60S5Z1NJ0R";
    approved.uniqueIdFormat = kFormatEui64.toStdString();

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
                               "4C5311A60S5Z1NJ0R", "S5Z1NJ0R", kFormatEui64);
    DiskIdentity approved;
    approved.uniqueId = "4C5311A60S5Z1NJ0X";
    approved.uniqueIdFormat = kFormatEui64.toStdString();

    const std::vector<Disk> liveDisks{live};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(!check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::NotFound);
    QVERIFY(check.disk == nullptr);
}

void TestDiskIdentity::ambiguousIdentityIsRefused() {
    // Two devices report the same identifier: the target is not unique.
    const Disk first = makeDisk(0, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                                "4C5311644S3ZNB0K", "S3ZNB0K", kFormatVendorId);
    const Disk second = makeDisk(1, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                                 "4C5311644S3ZNB0K", "S3ZNB0K", kFormatVendorId);
    DiskIdentity approved;
    approved.uniqueId = "4C5311644S3ZNB0K";
    approved.uniqueIdFormat = kFormatVendorId.toStdString();

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
                                   "4C5311644OTHERID", "S3ZNB0K", kFormatVendorId);
    DiskIdentity approved;
    approved.uniqueId = "4C5311644S3ZNB0K";
    approved.uniqueIdFormat = kFormatVendorId.toStdString();
    approved.serialNumber = "S3ZNB0K";

    const std::vector<Disk> liveDisks{impostor};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(!check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::NotFound);

    // The same holds when the unique id text matches but the formats do
    // not: the serial must not rescue the comparison.
    const Disk otherFormat = makeDisk(0, "Generic USB Disk",
                                      16ull * 1024 * 1024 * 1024,
                                      "4C5311644S3ZNB0K", "S3ZNB0K", kFormatEui64);
    const std::vector<Disk> otherFormatDisks{otherFormat};
    const auto formatCheck = ulli::core::checkDiskIdentity(approved, 0,
                                                           otherFormatDisks);
    QVERIFY(!formatCheck.ok());
    QCOMPARE(formatCheck.match, DiskIdentityMatch::NotFound);
}

void TestDiskIdentity::hyphenPlacementDoesNotCollide() {
    // "AB-CD" and "A-BCD" are different identifiers. Removing the hyphen
    // would make them equal and could send the install to the wrong disk.
    const Disk live = makeDisk(0, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                               "AB-CD", "AB-CD", kFormatVendorSpecific);
    DiskIdentity approved;
    approved.uniqueId = "A-BCD";
    approved.uniqueIdFormat = kFormatVendorSpecific.toStdString();
    approved.serialNumber = "A-BCD";

    const std::vector<Disk> liveDisks{live};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(!check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::NotFound);
    QVERIFY(check.disk == nullptr);
}

void TestDiskIdentity::underscorePlacementDoesNotCollide() {
    const Disk live = makeDisk(0, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                               "AB_CD", "AB_CD", kFormatVendorSpecific);
    DiskIdentity approved;
    approved.uniqueId = "A_BCD";
    approved.uniqueIdFormat = kFormatVendorSpecific.toStdString();
    approved.serialNumber = "A_BCD";

    const std::vector<Disk> liveDisks{live};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(!check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::NotFound);
}

void TestDiskIdentity::punctuationInScsiNameStaysSignificant() {
    // A SCSI name string (an iSCSI name) is a structured string: the
    // punctuation separates its parts and is part of the name.
    const QString name = QStringLiteral("iqn.2010-10.com.example:target-1");
    const Disk live = makeDisk(0, "Microsoft iSCSI Disk", 100ull * 1024 * 1024 * 1024,
                               name, "ISCSI-SERIAL", kFormatScsiNameString);

    // The very same name is the same device.
    DiskIdentity approved;
    approved.uniqueId = name.toStdString();
    approved.uniqueIdFormat = kFormatScsiNameString.toStdString();
    const std::vector<Disk> liveDisks{live};
    const auto same = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(same.ok());
    QCOMPARE(same.match, DiskIdentityMatch::Match);

    // Dropping the hyphen would make it a different name.
    DiskIdentity shortened;
    shortened.uniqueId = "iqn.2010-10.com.example:target1";
    shortened.uniqueIdFormat = kFormatScsiNameString.toStdString();
    const auto other = ulli::core::checkDiskIdentity(shortened, 0, liveDisks);
    QVERIFY(!other.ok());
    QCOMPARE(other.match, DiskIdentityMatch::NotFound);
}

void TestDiskIdentity::incompatibleUniqueIdFormatsAreNotEquivalent() {
    // The same text under two different descriptor types describes two
    // different things and must not be accepted as the same device.
    const Disk live = makeDisk(0, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                               "4C5311644S3ZNB0K", "S3ZNB0K", kFormatEui64);
    DiskIdentity approved;
    approved.uniqueId = "4C5311644S3ZNB0K";
    approved.uniqueIdFormat = kFormatScsiNameString.toStdString();
    approved.serialNumber = "S3ZNB0K";

    const std::vector<Disk> liveDisks{live};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(!check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::NotFound);
    QVERIFY(check.disk == nullptr);
}

void TestDiskIdentity::missingFormatFallsBackToTheText() {
    // An absent format in the plan is not a reason to reject a device:
    // the text comparison decides and the ambiguity check still applies.
    const Disk live = makeDisk(0, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                               "4C5311644S3ZNB0K", "S3ZNB0K", kFormatEui64);

    // The plan does not know the format.
    DiskIdentity withoutFormat;
    withoutFormat.uniqueId = "4C5311644S3ZNB0K";
    const std::vector<Disk> liveDisks{live};
    const auto check = ulli::core::checkDiskIdentity(withoutFormat, 0, liveDisks);
    QVERIFY(check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::Match);

    // The live disk does not report a format either.
    const Disk withoutFormatLive = makeDisk(0, "Generic USB Disk",
                                            16ull * 1024 * 1024 * 1024,
                                            "4C5311644S3ZNB0K", "S3ZNB0K");
    const std::vector<Disk> withoutFormatDisks{withoutFormatLive};
    const auto both = ulli::core::checkDiskIdentity(withoutFormat, 0,
                                                     withoutFormatDisks);
    QVERIFY(both.ok());
    QCOMPARE(both.match, DiskIdentityMatch::Match);

    // A different text is still refused.
    DiskIdentity different;
    different.uniqueId = "4C5311644S3ZNB0L";
    const auto mismatch = ulli::core::checkDiskIdentity(different, 0,
                                                         withoutFormatDisks);
    QVERIFY(!mismatch.ok());
    QCOMPARE(mismatch.match, DiskIdentityMatch::NotFound);
}

void TestDiskIdentity::knownFormatIsNotSilentlyLost() {
    // The plan knew the format, the live disk does not report one: the
    // meaning of the identifier cannot be confirmed, so the match is
    // refused instead of being accepted on the text alone.
    const Disk live = makeDisk(0, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                               "4C5311644S3ZNB0K", "S3ZNB0K");
    DiskIdentity approved;
    approved.uniqueId = "4C5311644S3ZNB0K";
    approved.uniqueIdFormat = kFormatEui64.toStdString();
    approved.serialNumber = "S3ZNB0K";

    const std::vector<Disk> liveDisks{live};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(!check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::NotFound);
    QVERIFY(check.disk == nullptr);
}

void TestDiskIdentity::numericZeroFormatIsAPresentFormat() {
    // 0 is a valid descriptor type ("Vendor Specific"). It has to be
    // treated as a known format, not as a missing one.
    const Disk live = makeDisk(0, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                               "4C5311644S3ZNB0K", "S3ZNB0K", "0");
    DiskIdentity approved;
    approved.uniqueId = "4C5311644S3ZNB0K";
    approved.uniqueIdFormat = "0";

    const std::vector<Disk> liveDisks{live};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::Match);

    // "0" against a different, known format is a mismatch.
    const Disk otherFormat = makeDisk(0, "Generic USB Disk",
                                      16ull * 1024 * 1024 * 1024,
                                      "4C5311644S3ZNB0K", "S3ZNB0K", kFormatEui64);
    const std::vector<Disk> otherFormatDisks{otherFormat};
    const auto mismatch = ulli::core::checkDiskIdentity(approved, 0,
                                                         otherFormatDisks);
    QVERIFY(!mismatch.ok());
    QCOMPARE(mismatch.match, DiskIdentityMatch::NotFound);

    // "0" is a known format, so a live disk without a format is refused
    // as well - the same rule as for every other known format.
    const Disk withoutFormat = makeDisk(0, "Generic USB Disk",
                                        16ull * 1024 * 1024 * 1024,
                                        "4C5311644S3ZNB0K", "S3ZNB0K");
    const std::vector<Disk> withoutFormatDisks{withoutFormat};
    const auto lost = ulli::core::checkDiskIdentity(approved, 0,
                                                     withoutFormatDisks);
    QVERIFY(!lost.ok());
    QCOMPARE(lost.match, DiskIdentityMatch::NotFound);
}

void TestDiskIdentity::formatNamesAreComparedCaseInsensitively() {
    // The format names come from a fixed vocabulary, so their case does
    // not carry information.
    const Disk live = makeDisk(0, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                               "4C5311644S3ZNB0K", "S3ZNB0K", kFormatEui64);
    DiskIdentity approved;
    approved.uniqueId = "4C5311644S3ZNB0K";
    approved.uniqueIdFormat = "eui64";

    const std::vector<Disk> liveDisks{live};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::Match);
}

void TestDiskIdentity::surroundingWhitespaceIsIgnored() {
    // The serial strings of SCSI and ATA devices are space padded. The
    // padding is not part of the serial.
    const Disk live = makeDisk(0, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                               "4C5311644S3ZNB0K", "S3ZNB0K", kFormatVendorId);
    DiskIdentity approved;
    approved.uniqueId = "  4C5311644S3ZNB0K  ";
    approved.uniqueIdFormat = kFormatVendorId.toStdString();
    approved.serialNumber = "S3ZNB0K   ";

    const std::vector<Disk> liveDisks{live};
    const auto check = ulli::core::checkDiskIdentity(approved, 0, liveDisks);
    QVERIFY(check.ok());
    QCOMPARE(check.match, DiskIdentityMatch::Match);
}

void TestDiskIdentity::internalWhitespaceAndCaseAreSignificant() {
    // Inside the identifier every character counts: a different spacing or
    // a different case describes a different value.
    const Disk live = makeDisk(0, "Generic USB Disk", 16ull * 1024 * 1024 * 1024,
                               "4C5311644S3ZNB0K", "S3ZN NB0K", kFormatVendorId);

    DiskIdentity differentSpacing;
    differentSpacing.serialNumber = "S3ZNNB0K";
    const std::vector<Disk> liveDisks{live};
    const auto spacing = ulli::core::checkDiskIdentity(differentSpacing, 0,
                                                        liveDisks);
    QVERIFY(!spacing.ok());
    QCOMPARE(spacing.match, DiskIdentityMatch::NotFound);

    DiskIdentity differentCase;
    differentCase.serialNumber = "s3zn nb0k";
    const auto letterCase = ulli::core::checkDiskIdentity(differentCase, 0,
                                                           liveDisks);
    QVERIFY(!letterCase.ok());
    QCOMPARE(letterCase.match, DiskIdentityMatch::NotFound);
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
    // Only the surrounding whitespace is removed.
    QCOMPARE(ulli::core::trimDiskIdentifier(" s3zn-nb0_k "),
             std::string("s3zn-nb0_k"));
    QCOMPARE(ulli::core::trimDiskIdentifier(""), std::string());
    QCOMPARE(ulli::core::trimDiskIdentifier("   "), std::string());
    QCOMPARE(ulli::core::trimDiskIdentifier("AB-CD"), std::string("AB-CD"));
    QCOMPARE(ulli::core::trimDiskIdentifier("A B"), std::string("A B"));

    // The placeholder vocabulary is matched case insensitively, because
    // rejecting more is the safe direction.
    QVERIFY(ulli::core::isPlaceholderDiskIdentifier(""));
    QVERIFY(ulli::core::isPlaceholderDiskIdentifier("  "));
    QVERIFY(ulli::core::isPlaceholderDiskIdentifier("0000000000"));
    QVERIFY(ulli::core::isPlaceholderDiskIdentifier("00000000"));
    QVERIFY(ulli::core::isPlaceholderDiskIdentifier("FFFFFFFF"));
    QVERIFY(ulli::core::isPlaceholderDiskIdentifier("ffffffff"));
    QVERIFY(ulli::core::isPlaceholderDiskIdentifier("none"));
    QVERIFY(ulli::core::isPlaceholderDiskIdentifier("None"));
    QVERIFY(ulli::core::isPlaceholderDiskIdentifier("TO BE FILLED BY O.E.M."));
    QVERIFY(!ulli::core::isPlaceholderDiskIdentifier("S3ZNNB0K"));
    QVERIFY(!ulli::core::isPlaceholderDiskIdentifier("0A1B2C3D4E5F"));
}

QTEST_MAIN(TestDiskIdentity)
#include "test_diskidentity.moc"
