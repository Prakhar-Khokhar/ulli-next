// tests/test_plan.cpp
#include "core/InstallPlan.h"

#include <QtTest/QtTest>

class TestPlan : public QObject {
    Q_OBJECT
private slots:
    void emptyPlanInvalid() {
        ulli::core::InstallPlan p;
        QVERIFY(!p.valid());
    }
    void minimalValid() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::UseFreeAll;  // Doesn't require shrink
        QVERIFY(p.valid());
    }

    // Bug A regression: a catalog distro plan with an empty isoPath must be
    // valid — InstallEngine::runStages resolves the ISO (download or rediscover
    // the cached file) via backend_->resolveIso() *after* valid() is called.
    // Requiring isoPath here wrongly rejected a previously-downloaded ISO on
    // restart. UseFreeAll needs no shrink drive letter.
    void catalogPlanWithEmptyIsoPathIsValid() {
        ulli::core::InstallPlan p;
        p.distroKey = "linux-lite";
        // isoPath intentionally empty — resolved at install time.
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::UseFreeAll;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        QVERIFY(p.valid());
    }
    void tooSmallLinuxInvalid() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 1ull * 1024 * 1024 * 1024;  // 1 GB, below 20 GB min
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        QVERIFY(!p.valid());
    }
    void summaryContainsDistro() {
        ulli::core::InstallPlan p;
        p.distroKey = "ubuntu";
        p.isoPath = "/tmp/u.iso";
        p.targetDiskNumber = 2;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        QVERIFY(p.summary().contains("ubuntu"));
    }
    void shrinkAllRequiresShrinkAmount() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;  // 30 GB - meets FullInstall minimum
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::ShrinkAll;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        p.shrinkDriveLetter = 'C';  // shrink strategy requires a target partition
        p.shrinkPartitionNumber = 3; // stable on-disk partition identity
        // No shrinkAmountBytes set - should be invalid
        QVERIFY(!p.valid());
        
        // With proper shrinkAmountBytes (must equal linuxSizeBytes for ShrinkAll) - should be valid
        p.shrinkAmountBytes = p.linuxSizeBytes;  // 30 GB
        QVERIFY(p.valid());
        
        // Test LiveOnly mode with 7 GB
        p.allocationMode = ulli::core::AllocationMode::LiveOnly;
        p.linuxSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.shrinkAmountBytes = 0;
        QVERIFY(!p.valid());
        p.shrinkAmountBytes = p.linuxSizeBytes;  // 7 GB
        QVERIFY(p.valid());
        
        // Test FullInstall mode with less than 30 GB - should be invalid
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        p.linuxSizeBytes = 20ull * 1024 * 1024 * 1024;  // 20 GB - below 30 GB minimum
        p.shrinkAmountBytes = p.linuxSizeBytes;
        QVERIFY(!p.valid());
    }

    void diskZeroIsValid() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 0;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::UseFreeAll;
        QVERIFY(p.valid());
    }

    void unsetDiskNumberIsInvalid() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::UseFreeAll;
        QVERIFY(!p.valid());
    }

    // Regression: shrink strategy requires both drive letter and partition
    // number; having only one is an inconsistent plan that must fail closed.
    void shrinkRequiresPartitionNumber() {
        ulli::core::InstallPlan p;
        p.distroKey = "mint";
        p.isoPath = "/tmp/x.iso";
        p.targetDiskNumber = 1;
        p.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        p.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        p.strategy = ulli::core::Strategy::ShrinkAll;
        p.allocationMode = ulli::core::AllocationMode::FullInstall;
        p.shrinkAmountBytes = p.linuxSizeBytes;
        p.shrinkDriveLetter = 'C';
        // Missing shrinkPartitionNumber: invalid
        QVERIFY(!p.valid());

        p.shrinkPartitionNumber = 3;
        QVERIFY(p.valid());
    }
};

QTEST_GUILESS_MAIN(TestPlan)
#include "test_plan.moc"
