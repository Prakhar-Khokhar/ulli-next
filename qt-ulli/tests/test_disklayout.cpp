// tests/test_disklayout.cpp
//
// Tests for the layout arithmetic of the disk operations.
//
// The installer creates a staging partition in a free region of the
// target disk and afterwards verifies that the region still holds the
// free space that is left over. The region is the one that was measured
// before the creation, so the check must not be influenced by free space
// elsewhere on the same disk.
//
// The arithmetic is platform independent: the tests only work with
// numbers, no disk is touched.

#include "core/DiskLayout.h"

#include <QtTest/QtTest>

using ulli::core::DiskExtent;
using ulli::core::freeSpaceInRegion;
using ulli::core::freeSpaceMatches;

namespace {

constexpr std::uint64_t kMiB = 1024ull * 1024;
constexpr std::uint64_t kGiB = 1024ull * 1024 * 1024;

}  // namespace

class TestDiskLayout : public QObject {
    Q_OBJECT

private slots:
    void theStagingPartitionIsTheOnlyOccupantOfTheRegion();
    void freeSpaceElsewhereOnTheDiskIsIgnored();
    void aPartitionReachingIntoTheRegionCounts();
    void aFullRegionHasNoFreeSpace();
    void anEmptyRegionHasNoFreeSpace();
    void theToleranceIsRespected();
    void theVmScenarioIsAccepted();
};

void TestDiskLayout::theStagingPartitionIsTheOnlyOccupantOfTheRegion() {
    // The reported case: a 49.98 GB disk with a 25.10 GB NTFS partition,
    // a 7 GB staging partition and 17.78 GB of free space behind it.
    const std::uint64_t diskSize = 49980ull * kMiB;
    const std::uint64_t targetSize = 25100ull * kMiB;
    const std::uint64_t stagingSize = 7ull * kGiB;
    const std::uint64_t regionOffset = 100ull * kMiB + targetSize;
    const std::uint64_t regionSize = diskSize - regionOffset;

    // The staging partition sits at the start of the region, the rest of
    // the region is free.
    const std::vector<DiskExtent> extents{
        {0, 100ull * kMiB},                           // EFI
        {100ull * kMiB, targetSize},                  // NTFS, ends where the region starts
        {regionOffset, stagingSize},                  // staging partition
    };

    const std::uint64_t free = freeSpaceInRegion(regionOffset, regionSize, extents);
    QCOMPARE(free, regionSize - stagingSize);
    QVERIFY(freeSpaceMatches(free, regionSize - stagingSize, 10ull * kMiB));
}

void TestDiskLayout::freeSpaceElsewhereOnTheDiskIsIgnored() {
    // A gap in the middle of the disk and another free region behind the
    // last partition: only the gap counts.
    const std::uint64_t regionOffset = 10ull * kGiB;
    const std::uint64_t regionSize = 4ull * kGiB;
    const std::uint64_t stagingSize = 7ull * kMiB;

    const std::vector<DiskExtent> extents{
        {0, 1ull * kGiB},                          // first partition
        {regionOffset, stagingSize},               // staging partition
        {20ull * kGiB, 2ull * kGiB},                // partition behind the region
    };

    const std::uint64_t free = freeSpaceInRegion(regionOffset, regionSize, extents);
    QCOMPARE(free, regionSize - stagingSize);
}

void TestDiskLayout::aPartitionReachingIntoTheRegionCounts() {
    // A partition that starts before the region and reaches into it
    // occupies the overlapping part only.
    const std::uint64_t regionOffset = 100ull * kMiB;
    const std::uint64_t regionSize = 10ull * kMiB;

    const std::vector<DiskExtent> extents{
        {0, 105ull * kMiB},  // reaches 5 MiB into the region
    };
    QCOMPARE(freeSpaceInRegion(regionOffset, regionSize, extents), 5ull * kMiB);

    // A partition that ends exactly at the region start does not count.
    const std::vector<DiskExtent> outside{
        {0, 100ull * kMiB},
    };
    QCOMPARE(freeSpaceInRegion(regionOffset, regionSize, outside), regionSize);
}

void TestDiskLayout::aFullRegionHasNoFreeSpace() {
    const std::uint64_t regionOffset = 0;
    const std::uint64_t regionSize = 8ull * kGiB;
    const std::vector<DiskExtent> extents{
        {0, regionSize},
    };
    QCOMPARE(freeSpaceInRegion(regionOffset, regionSize, extents), std::uint64_t(0));
}

void TestDiskLayout::anEmptyRegionHasNoFreeSpace() {
    QCOMPARE(freeSpaceInRegion(0, 0, {}), std::uint64_t(0));
    QCOMPARE(freeSpaceInRegion(1024, 0, {{0, 4096}}), std::uint64_t(0));
}

void TestDiskLayout::theToleranceIsRespected() {
    QVERIFY(freeSpaceMatches(100, 100, 0));
    QVERIFY(freeSpaceMatches(100, 105, 5));
    QVERIFY(freeSpaceMatches(105, 100, 5));
    QVERIFY(!freeSpaceMatches(100, 106, 5));
    QVERIFY(!freeSpaceMatches(106, 100, 5));
}

void TestDiskLayout::theVmScenarioIsAccepted() {
    // The scenario of the failed VM run: the plan expected 7 GB of freed
    // space, the disk really had 24.78 GB of free space, the staging
    // partition took 7 GB of it. The leftover in the region is what the
    // region size says it is - the planned size must not be used.
    const std::uint64_t regionOffset = 25100ull * kMiB + 100ull * kMiB;
    const std::uint64_t regionSize = 19093520384ull;
    const std::uint64_t stagingSize = 7ull * kGiB;

    const std::vector<DiskExtent> extents{
        {0, 100ull * kMiB},
        {100ull * kMiB, 25100ull * kMiB},
        {regionOffset, stagingSize},
    };

    const std::uint64_t free = freeSpaceInRegion(regionOffset, regionSize, extents);
    QCOMPARE(free, regionSize - stagingSize);

    // The old check compared the free space of the whole disk against the
    // planned freed size minus the staging size and rejected the run.
    const std::uint64_t plannedFreed = stagingSize;
    const std::uint64_t oldExpected = plannedFreed > stagingSize
        ? plannedFreed - stagingSize : 0;
    QCOMPARE(oldExpected, std::uint64_t(0));
    QVERIFY(!freeSpaceMatches(free, oldExpected, 10ull * kMiB));
}

QTEST_MAIN(TestDiskLayout)
#include "test_disklayout.moc"
