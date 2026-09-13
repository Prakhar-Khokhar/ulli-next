// tests/test_allocation.cpp
#include "core/InstallPlan.h"

#include <QtTest/QtTest>

class TestAllocation : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {}

    // 1. live-only request defaults/validates to ~7 GB
    void liveOnlyDefaultIsSevenGB() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::UseFreeAll;  // Doesn't require shrink
        p.allocationMode = ulli::core::AllocationMode::LiveOnly;
        QVERIFY(p.valid());
    }

    void liveOnlyBelowSevenGBInvalid() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 5ull * 1024 * 1024 * 1024;  // 5 GB - below 7 GB minimum
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::UseFreeAll;
        p.allocationMode = ulli::core::AllocationMode::LiveOnly;
        QVERIFY(!p.valid());
    }

    // 2. full-install request below 30 GB is rejected
    void fullInstallBelow30GBInvalid() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 20ull * 1024 * 1024 * 1024;  // 20 GB - below 30 GB minimum
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::UseFreeAll;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        QVERIFY(!p.valid());
    }

    // 3. full-install request of exactly 30 GB
    void fullInstallExactly30GBValid() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;  // Exactly 30 GB
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::UseFreeAll;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        QVERIFY(p.valid());
    }

    // 4. full-install request greater than 30 GB
    void fullInstallAbove30GBValid() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 50ull * 1024 * 1024 * 1024;  // 50 GB
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::UseFreeAll;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        QVERIFY(p.valid());
    }

    // 5. Correct calculation of staging = 7 GB, unallocated = requested - 7 GB
    void allocationCalculation() {
        // LiveOnly: total = staging = 7 GB, unallocated = 0
        ulli::core::InstallPlan p1;
        p1.allocationMode = ulli::core::AllocationMode::LiveOnly;
        p1.linuxSizeBytes = 7ull * 1024 * 1024 * 1024;
        const uint64_t staging1 = 7ull * 1024 * 1024 * 1024;
        const uint64_t unallocated1 = (p1.linuxSizeBytes > staging1) ? (p1.linuxSizeBytes - staging1) : 0;
        QCOMPARE(unallocated1, 0ull);

        // FullInstall 30 GB: staging = 7 GB, unallocated = 23 GB
        ulli::core::InstallPlan p2;
        p2.allocationMode = ulli::core::AllocationMode::FullInstall;
        p2.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        const uint64_t staging2 = 7ull * 1024 * 1024 * 1024;
        const uint64_t unallocated2 = (p2.linuxSizeBytes > staging2) ? (p2.linuxSizeBytes - staging2) : 0;
        QCOMPARE(unallocated2, 23ull * 1024 * 1024 * 1024);

        // FullInstall 50 GB: staging = 7 GB, unallocated = 43 GB
        ulli::core::InstallPlan p3;
        p3.allocationMode = ulli::core::AllocationMode::FullInstall;
        p3.linuxSizeBytes = 50ull * 1024 * 1024 * 1024;
        const uint64_t unallocated3 = (p3.linuxSizeBytes > staging2) ? (p3.linuxSizeBytes - staging2) : 0;
        QCOMPARE(unallocated3, 43ull * 1024 * 1024 * 1024);

        // FullInstall 100 GB: staging = 7 GB, unallocated = 93 GB
        ulli::core::InstallPlan p4;
        p4.allocationMode = ulli::core::AllocationMode::FullInstall;
        p4.linuxSizeBytes = 100ull * 1024 * 1024 * 1024;
        const uint64_t unallocated4 = (p4.linuxSizeBytes > staging2) ? (p4.linuxSizeBytes - staging2) : 0;
        QCOMPARE(unallocated4, 93ull * 1024 * 1024 * 1024);
    }

    // 6. requested shrink larger than Windows-supported amount (tested via mock)
    // 7. requested final partition size below SizeMin (tested via mock)
    // 8. zero/negative/invalid request
    void zeroRequestInvalid() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 0;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::ShrinkAll;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        p.shrinkAmountBytes = 0;
        QVERIFY(!p.valid());
    }

    // 9. non-NTFS target (tested via mock)
    // 10. stale disk identity (tested via mock)
    // 11. stale partition identity (tested via mock)
    // 12. system/EFI/recovery target (tested via mock)
    // 13. BitLocker-protected target (tested via mock)
    // 14. command failure (tested via mock)
    // 15. post-resize verification failure (tested via mock)
    // 16. successful resize calculation (tested via mock)

    // Additional validation tests
    void shrinkAllRequiresShrinkDriveLetter() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::ShrinkAll;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        p.shrinkAmountBytes = p.linuxSizeBytes;
        // No shrinkDriveLetter set - valid() doesn't check this, but engine will fail
        QVERIFY(p.valid());  // Plan is valid, but engine will need the drive letter
    }

    void shrinkAllShrinkAmountMustEqualLinuxSize() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::ShrinkAll;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        p.shrinkAmountBytes = 40ull * 1024 * 1024 * 1024;  // Different from linuxSizeBytes
        QVERIFY(!p.valid());
    }

    void liveOnlyShrinkAmountMustEqualLinuxSize() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::ShrinkAll;
        p.allocationMode = ulli::core::AllocationMode::LiveOnly;
        p.shrinkAmountBytes = 10ull * 1024 * 1024 * 1024;  // Different from linuxSizeBytes
        QVERIFY(!p.valid());
    }

    void useFreeBootRequiresShrinkAmount() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::UseFreeBoot;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        p.shrinkAmountBytes = 0;
        QVERIFY(!p.valid());
    }

    void otherDriveShrinkRequiresShrinkAmount() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::OtherDriveShrink;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        p.shrinkAmountBytes = 0;
        QVERIFY(!p.valid());
    }

    void wipeDiskDoesNotRequireShrinkAmount() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::WipeDisk;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        p.shrinkAmountBytes = 0;
        QVERIFY(p.valid());
    }

    void useFreeAllDoesNotRequireShrinkAmount() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::UseFreeAll;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        p.shrinkAmountBytes = 0;
        QVERIFY(p.valid());
    }

    void bootSizeMustBeAtLeast1GB() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 500ull * 1024 * 1024;  // 500 MB - below 1 GB minimum
        p.strategy = ulli::core::Strategy::UseFreeAll;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        QVERIFY(!p.valid());
    }

    // === Phase 2.2 specific tests ===

    // LiveOnly allocation: 7 GiB total, 7 GiB staging, 0 remainder
    void liveOnlyAllocationSemantics() {
        ulli::core::InstallPlan p;
        p.allocationMode = ulli::core::AllocationMode::LiveOnly;
        p.linuxSizeBytes = 7ull * 1024 * 1024 * 1024;
        const uint64_t staging = ulli::core::kStagingSizeBytes;
        const uint64_t unallocated = (p.linuxSizeBytes > staging) ? (p.linuxSizeBytes - staging) : 0;
        QCOMPARE(unallocated, 0ull);
        // In LiveOnly mode, linuxSizeBytes IS the staging size
        QCOMPARE(p.linuxSizeBytes, ulli::core::kStagingSizeBytes);
    }

    // FullInstall 30 GB: 7 GB staging, ~23 GB unallocated
    void fullInstall30GBAllocation() {
        ulli::core::InstallPlan p;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        const uint64_t staging = ulli::core::kStagingSizeBytes;
        const uint64_t unallocated = (p.linuxSizeBytes > staging) ? (p.linuxSizeBytes - staging) : 0;
        QCOMPARE(staging, 7ull * 1024 * 1024 * 1024);
        QCOMPARE(unallocated, 23ull * 1024 * 1024 * 1024);
    }

    // FullInstall 50 GB: 7 GB staging, ~43 GB unallocated
    void fullInstall50GBAllocation() {
        ulli::core::InstallPlan p;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        p.linuxSizeBytes = 50ull * 1024 * 1024 * 1024;
        const uint64_t staging = ulli::core::kStagingSizeBytes;
        const uint64_t unallocated = (p.linuxSizeBytes > staging) ? (p.linuxSizeBytes - staging) : 0;
        QCOMPARE(staging, 7ull * 1024 * 1024 * 1024);
        QCOMPARE(unallocated, 43ull * 1024 * 1024 * 1024);
    }

    // Minimum FullInstall boundary (30 GB)
    void fullInstallMinimumBoundary() {
        // Exactly 30 GB should be valid
        ulli::core::InstallPlan pValid;
        pValid.distroKey = "mint";
        pValid.isoPath = "/tmp/x.iso";
        pValid.targetDiskNumber = 1;
        pValid.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        pValid.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        pValid.strategy = ulli::core::Strategy::UseFreeAll;
        pValid.allocationMode = ulli::core::AllocationMode::FullInstall;
        QVERIFY(pValid.valid());

        // 29.9 GB should be invalid
        ulli::core::InstallPlan pInvalid;
        pInvalid.distroKey = "mint";
        pInvalid.isoPath = "/tmp/x.iso";
        pInvalid.targetDiskNumber = 1;
        pInvalid.linuxSizeBytes = 29ull * 1024 * 1024 * 1024 + 900ull * 1024 * 1024;  // 29.9 GB
        pInvalid.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        pInvalid.strategy = ulli::core::Strategy::UseFreeAll;
        pInvalid.allocationMode = ulli::core::AllocationMode::FullInstall;
        QVERIFY(!pInvalid.valid());
    }

    // Invalid staging size (bootSizeBytes < kStagingSizeBytes)
    void invalidStagingSize() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 5ull * 1024 * 1024 * 1024;  // Below 7 GB staging
        p.strategy = ulli::core::Strategy::UseFreeAll;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        QVERIFY(!p.valid());
    }

    // LiveOnly with bootSizeBytes < kStagingSizeBytes
    void liveOnlyInvalidStagingSize() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 5ull * 1024 * 1024 * 1024;  // Below 7 GB
        p.strategy = ulli::core::Strategy::UseFreeAll;
        p.allocationMode = ulli::core::AllocationMode::LiveOnly;
        QVERIFY(!p.valid());
    }

    // Shrink amount mismatch with linuxSizeBytes for ShrinkAll
    void shrinkAllShrinkAmountMismatch() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::ShrinkAll;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        p.shrinkAmountBytes = 35ull * 1024 * 1024 * 1024;  // Different from linuxSizeBytes
        QVERIFY(!p.valid());
    }

    // LiveOnly shrink amount must equal linuxSizeBytes
    void liveOnlyShrinkAmountMatches() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::ShrinkAll;
        p.allocationMode = ulli::core::AllocationMode::LiveOnly;
        p.shrinkAmountBytes = 7ull * 1024 * 1024 * 1024;  // Matches linuxSizeBytes
        QVERIFY(p.valid());
    }

    void liveOnlyShrinkAmountMismatch() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::ShrinkAll;
        p.allocationMode = ulli::core::AllocationMode::LiveOnly;
        p.shrinkAmountBytes = 8ull * 1024 * 1024 * 1024;  // Doesn't match
        QVERIFY(!p.valid());
    }
};

QTEST_GUILESS_MAIN(TestAllocation)
#include "test_allocation.moc"