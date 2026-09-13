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
};

QTEST_GUILESS_MAIN(TestPlan)
#include "test_plan.moc"
