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
};

QTEST_GUILESS_MAIN(TestPlan)
#include "test_plan.moc"
