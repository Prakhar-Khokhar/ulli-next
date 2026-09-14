// tests/test_engine.cpp
#include "core/InstallEngine.h"
#include "core/Catalog.h"
#include "core/ProgressLog.h"

#include <QEventLoop>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>
#include <QtTest/QtTest>

#include <memory>
#include <vector>

namespace ulli::core {

// Mock backend that can simulate various scenarios
class MockBackend : public IPlatformBackend {
public:
    struct Expectation {
        QString stage;
        bool shouldSucceed = true;
        int delayMs = 0;
    };

    MockBackend() = default;

    void setExpectations(std::vector<Expectation> exps) {
        expectations_ = std::move(exps);
        currentExpectation_ = 0;
    }

    void setFailAtStage(const QString& stage) {
        for (auto& e : expectations_) {
            if (e.stage == stage) e.shouldSucceed = false;
        }
    }

    // IPlatformBackend interface
    std::vector<Disk> enumerateDisks() override {
        return disks_;
    }

    Result<void> preflight(const InstallPlan&) override {
        auto r = checkExpectation("preflight");
        if (!r) return r;
        return checkExpectation("validate");
    }

    Result<void> validatePlanForDisk(const InstallPlan&) override {
        // Called internally by preflight, don't double-count
        return makeOk();
    }

    Result<std::filesystem::path> resolveIso(const Distro&) override {
        auto r = checkExpectation("iso");
        if (!r) return makeError<std::filesystem::path>(r.error().kind(), r.error().message());
        return makeOk<std::filesystem::path>("/tmp/test.iso");
    }

    Result<std::filesystem::path> downloadIso(
        const Distro&, const std::filesystem::path&,
        std::function<void(int, const QString&)>) override {
        return makeOk<std::filesystem::path>("/tmp/test.iso");
    }

    Result<std::uint64_t> shrinkPartition(char, std::uint64_t) override {
        auto r = checkExpectation("resize");
        if (!r) return makeError<std::uint64_t>(r.error().kind(), r.error().message());
        return makeOk<std::uint64_t>(100ull * 1024 * 1024 * 1024);
    }

    Result<void> wipeDisk(int) override {
        return checkExpectation("wipe");
    }

    Result<void> createLayout(const InstallPlan&, std::filesystem::path& bootMount,
                              std::filesystem::path& refindMount,
                              std::function<bool()>) override {
        auto r = checkExpectation("layout");
        if (!r) return r;
        bootMount = "/mnt/boot";
        refindMount = "/mnt/refind";
        return makeOk();
    }

    Result<std::filesystem::path> mountIso(const std::filesystem::path&) override {
        return checkExpectation<std::filesystem::path>("mount");
    }

    void unmountIso(const std::filesystem::path&) override {}

    Result<void> copyFiles(const std::filesystem::path&, const std::filesystem::path&,
                           const Distro*, std::function<bool()>) override {
        return checkExpectation("copy");
    }

    Result<void> patchDistroBootConfig(const Distro&, const InstallPlan&) override {
        return checkExpectation("patch");
    }

    Result<void> installRefind(const InstallPlan&) override {
        return checkExpectation("refind");
    }

    Result<void> createBootEntry(const InstallPlan& plan) override {
        auto r = checkExpectation("bootentry");
        if (r) {
            const_cast<InstallPlan&>(plan).bcdGuid = "test-guid";
        }
        return r;
    }

    void rollbackBootEntry(const InstallPlan&) override {}

    void restartSystem() override {}

    // Test setup
    void setDisks(std::vector<Disk> d) { disks_ = std::move(d); }

private:
    std::vector<Disk> disks_;
    std::vector<Expectation> expectations_;
    size_t currentExpectation_ = 0;

    template <typename T>
    Result<T> checkExpectation(const QString& stage) {
        qDebug() << "Mock called with stage:" << stage << "currentExpectation:" << currentExpectation_;
        if (currentExpectation_ >= expectations_.size()) {
            qDebug() << "No more expectations, returning ok";
            if constexpr (std::is_void_v<T>) {
                return makeOk();
            } else {
                return makeOk<T>(T{});
            }
        }
        const auto& exp = expectations_[currentExpectation_++];
        qDebug() << "Expected stage:" << exp.stage << "shouldSucceed:" << exp.shouldSucceed;
        if (exp.stage != stage) {
            return makeError<T>(Error::Kind::Internal, "Unexpected stage: " + stage.toStdString());
        }
        if (exp.delayMs > 0) {
            QThread::msleep(static_cast<unsigned long>(exp.delayMs));
        }
        if (!exp.shouldSucceed) {
            return makeError<T>(Error::Kind::Platform, "Simulated failure at " + stage.toStdString());
        }
        if constexpr (std::is_void_v<T>) {
            return makeOk();
        } else {
            return makeOk<T>(T{});
        }
    }

    Result<void> checkExpectation(const QString& stage) {
        return checkExpectation<void>(stage);
    }
};

class TestEngine : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        // Create a minimal catalog with one distro
        std::vector<Distro> distros;
        distros.emplace_back("test", "Test Distro", "test.iso",
                           "0000000000000000000000000000000000000000000000000000000000000000",
                           std::vector<std::string>{"http://example.com/test.iso"});
        catalog_ = Catalog::fromDistros(std::move(distros));
    }

    void waitForFinished(InstallEngine* engine, QSignalSpy& finishedSpy, int timeoutMs = 5000) {
        QEventLoop loop;
        connect(engine, &InstallEngine::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        loop.exec();
        QVERIFY2(finishedSpy.count() == 1, qPrintable(QString("Expected 1 finished signal, got %1").arg(finishedSpy.count())));
    }

    void testSuccessfulInstall() {
        MockBackend backend;
        backend.setExpectations({
            {"preflight", true},
            {"validate", true},
            {"iso", true},
            {"layout", true},
            {"mount", true},
            {"copy", true},
            {"patch", true},
            {"bootentry", true},
        });

        // Add a test disk
        Disk disk;
        disk.number = 1;
        disk.model = "Test Disk";
        disk.sizeBytes = 500ull * 1024 * 1024 * 1024;
        disk.style = PartitionStyle::GPT;
        backend.setDisks({disk});

        ProgressLog log;
        InstallEngine engine(std::make_unique<MockBackend>(std::move(backend)), &catalog_, &log);

        QSignalSpy stageSpy(&engine, &InstallEngine::stageChanged);
        QSignalSpy progressSpy(&engine, &InstallEngine::progressChanged);
        QSignalSpy finishedSpy(&engine, &InstallEngine::finished);

        InstallPlan plan;
        plan.distroKey = "test";
        plan.isoPath = "/tmp/test.iso";
        plan.targetDiskNumber = 1;
        plan.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        plan.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        plan.strategy = Strategy::UseFreeAll;

        engine.run(std::move(plan));

        waitForFinished(&engine, finishedSpy);

        // Check results
        auto args = finishedSpy.first();
        qDebug() << "Finished args:" << args;
        QVERIFY(args[0].toBool() == true);  // success
        QVERIFY(args[2].toBool() == false); // autoRestart (default false)

        // Check stages were emitted in order
        QStringList expectedStages = {
            "Pre-flight checks",
            "Resolving ISO",
            "Creating partitions and filesystems",
            "Copying live ISO contents",
            "Patching distro boot configuration",
            "Creating UEFI boot entry",
            "Cleaning up"
        };
        for (int i = 0; i < expectedStages.size(); ++i) {
            QCOMPARE(stageSpy.at(i)[0].toString(), expectedStages[i]);
        }

        // Progress should reach 100
        QVERIFY(progressSpy.count() > 0);
        QCOMPARE(progressSpy.last()[0].toInt(), 100);
    }

    void testFailurePropagation() {
        MockBackend backend;
        backend.setExpectations({
            {"preflight", true},
            {"validate", true},
            {"iso", true},
            {"layout", true},
            {"mount", true},
            {"copy", false},  // Fail at copy stage
        });

        Disk disk;
        disk.number = 1;
        disk.sizeBytes = 500ull * 1024 * 1024 * 1024;
        backend.setDisks({disk});

        ProgressLog log;
        InstallEngine engine(std::make_unique<MockBackend>(std::move(backend)), &catalog_, &log);

        QSignalSpy finishedSpy(&engine, &InstallEngine::finished);

        InstallPlan plan;
        plan.distroKey = "test";
        plan.isoPath = "/tmp/test.iso";
        plan.targetDiskNumber = 1;
        plan.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        plan.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        plan.strategy = Strategy::UseFreeAll;

        engine.run(std::move(plan));

        waitForFinished(&engine, finishedSpy);

        auto args = finishedSpy.first();
        QVERIFY(args[0].toBool() == false);  // failure
        QVERIFY(args[1].toString().contains("Simulated failure"));
    }

    void testCancellation() {
        MockBackend backend;
        backend.setExpectations({
            {"preflight", true, 100},
            {"validate", true, 100},
            {"iso", true, 100},
            {"layout", true, 100},
            {"mount", true, 100},
            {"copy", true, 100},
            {"patch", true, 100},
            {"bootentry", true, 100},
        });

        Disk disk;
        disk.number = 1;
        disk.sizeBytes = 500ull * 1024 * 1024 * 1024;
        backend.setDisks({disk});

        ProgressLog log;
        // Run engine in a thread like the real UI does
        QThread engineThread;
        InstallEngine engine(std::make_unique<MockBackend>(std::move(backend)), &catalog_, &log);
        engine.moveToThread(&engineThread);

        QSignalSpy finishedSpy(&engine, &InstallEngine::finished);

        InstallPlan plan;
        plan.distroKey = "test";
        plan.isoPath = "/tmp/test.iso";
        plan.targetDiskNumber = 1;
        plan.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        plan.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        plan.strategy = Strategy::UseFreeAll;

        // Start engine in thread
        QEventLoop loop;
        connect(&engineThread, &QThread::started, &engine, [&engine, plan = std::move(plan)]() mutable {
            engine.run(std::move(plan));
        });
        connect(&engine, &InstallEngine::finished, &loop, &QEventLoop::quit);
        engineThread.start();

        // Give it a moment to start, then cancel
        QThread::msleep(100);
        engine.requestCancel();

        // Wait for finished
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();

        engineThread.quit();
        engineThread.wait(1000);

        QVERIFY(finishedSpy.count() == 1);
        auto args = finishedSpy.first();
        QVERIFY(args[0].toBool() == false);
        QVERIFY(args[1].toString().contains("Cancelled"));
    }

    void testWipeDiskStrategy() {
        MockBackend backend;
        backend.setExpectations({
            {"preflight", true},
            {"validate", true},
            {"iso", true},
            {"wipe", true},
            {"layout", true},
            {"mount", true},
            {"copy", true},
            {"patch", true},
            {"bootentry", true},
        });

        Disk disk;
        disk.number = 2;
        disk.sizeBytes = 250ull * 1024 * 1024 * 1024;
        backend.setDisks({disk});

        ProgressLog log;
        InstallEngine engine(std::make_unique<MockBackend>(std::move(backend)), &catalog_, &log);

        QSignalSpy stageSpy(&engine, &InstallEngine::stageChanged);
        QSignalSpy finishedSpy(&engine, &InstallEngine::finished);

        InstallPlan plan;
        plan.distroKey = "test";
        plan.isoPath = "/tmp/test.iso";
        plan.targetDiskNumber = 2;
        plan.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        plan.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        plan.strategy = Strategy::WipeDisk;

        engine.run(std::move(plan));

        waitForFinished(&engine, finishedSpy);

        auto args = finishedSpy.first();
        QVERIFY(args[0].toBool() == true);

        // Check that "Wiping target disk" stage was emitted
        bool foundWipe = false;
        for (int i = 0; i < stageSpy.count(); ++i) {
            if (stageSpy.at(i)[0].toString() == "Wiping target disk") {
                foundWipe = true;
                break;
            }
        }
        QVERIFY(foundWipe);
    }

    void testShrinkAllStrategy() {
        MockBackend backend;
        backend.setExpectations({
            {"preflight", true},
            {"validate", true},
            {"iso", true},
            {"resize", true},
            {"layout", true},
            {"mount", true},
            {"copy", true},
            {"patch", true},
            {"bootentry", true},
        });

        Disk disk;
        disk.number = 1;
        disk.sizeBytes = 500ull * 1024 * 1024 * 1024;
        backend.setDisks({disk});

        ProgressLog log;
        InstallEngine engine(std::make_unique<MockBackend>(std::move(backend)), &catalog_, &log);

        QSignalSpy stageSpy(&engine, &InstallEngine::stageChanged);
        QSignalSpy finishedSpy(&engine, &InstallEngine::finished);

        InstallPlan plan;
        plan.distroKey = "test";
        plan.isoPath = "/tmp/test.iso";
        plan.targetDiskNumber = 1;
        plan.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;  // 30 GB - meets FullInstall minimum
        plan.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        plan.strategy = Strategy::ShrinkAll;
        plan.shrinkDriveLetter = 'C';
        plan.shrinkAmountBytes = plan.linuxSizeBytes;  // Must equal linuxSizeBytes for ShrinkAll
        plan.allocationMode = AllocationMode::FullInstall;

        engine.run(std::move(plan));

        waitForFinished(&engine, finishedSpy);

        auto args = finishedSpy.first();
        QVERIFY(args[0].toBool() == true);

        // Check that "Resizing existing partition" stage was emitted
        bool foundResize = false;
        for (int i = 0; i < stageSpy.count(); ++i) {
            if (stageSpy.at(i)[0].toString() == "Resizing existing partition") {
                foundResize = true;
                break;
            }
        }
        QVERIFY(foundResize);
    }

    void testInvalidPlanRejected() {
        MockBackend backend;
        Disk disk;
        disk.number = 1;
        disk.sizeBytes = 500ull * 1024 * 1024 * 1024;
        backend.setDisks({disk});

        ProgressLog log;
        InstallEngine engine(std::make_unique<MockBackend>(std::move(backend)), &catalog_, &log);

        QSignalSpy finishedSpy(&engine, &InstallEngine::finished);

        // Plan with empty distroKey - should be invalid
        InstallPlan plan;
        plan.distroKey = "";
        plan.isoPath = "/tmp/test.iso";
        plan.targetDiskNumber = 1;
        plan.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        plan.bootSizeBytes = 7ull * 1024 * 1024 * 1024;

        engine.run(std::move(plan));

        waitForFinished(&engine, finishedSpy, 1000);

        auto args = finishedSpy.first();
        QVERIFY(args[0].toBool() == false);
        QVERIFY(args[1].toString().contains("validation"));
    }

    // Test cancellation during createLayout stage
    void testCancellationDuringLayout() {
        MockBackend backend;
        backend.setExpectations({
            {"preflight", true, 10},
            {"validate", true, 10},
            {"iso", true, 10},
            {"layout", true, 500},  // Long delay in layout
        });

        Disk disk;
        disk.number = 1;
        disk.sizeBytes = 500ull * 1024 * 1024 * 1024;
        backend.setDisks({disk});

        ProgressLog log;
        QThread engineThread;
        InstallEngine engine(std::make_unique<MockBackend>(std::move(backend)), &catalog_, &log);
        engine.moveToThread(&engineThread);

        QSignalSpy finishedSpy(&engine, &InstallEngine::finished);

        InstallPlan plan;
        plan.distroKey = "test";
        plan.isoPath = "/tmp/test.iso";
        plan.targetDiskNumber = 1;
        plan.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        plan.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        plan.strategy = Strategy::UseFreeAll;

        QEventLoop loop;
        connect(&engineThread, &QThread::started, &engine, [&engine, plan = std::move(plan)]() mutable {
            engine.run(std::move(plan));
        });
        connect(&engine, &InstallEngine::finished, &loop, &QEventLoop::quit);
        engineThread.start();

        // Wait for layout stage to start, then cancel
        QThread::msleep(200);
        engine.requestCancel();

        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();

        engineThread.quit();
        engineThread.wait(1000);

        QVERIFY(finishedSpy.count() == 1);
        auto args = finishedSpy.first();
        QVERIFY(args[0].toBool() == false);
        QVERIFY(args[1].toString().contains("Cancelled"));
    }

    // Test LiveOnly allocation mode
    void testLiveOnlyAllocation() {
        MockBackend backend;
        backend.setExpectations({
            {"preflight", true},
            {"validate", true},
            {"iso", true},
            {"layout", true},
            {"mount", true},
            {"copy", true},
            {"patch", true},
            {"bootentry", true},
        });

        Disk disk;
        disk.number = 1;
        disk.sizeBytes = 500ull * 1024 * 1024 * 1024;
        backend.setDisks({disk});

        ProgressLog log;
        InstallEngine engine(std::make_unique<MockBackend>(std::move(backend)), &catalog_, &log);

        QSignalSpy finishedSpy(&engine, &InstallEngine::finished);

        InstallPlan plan;
        plan.distroKey = "test";
        plan.isoPath = "/tmp/test.iso";
        plan.targetDiskNumber = 1;
        plan.linuxSizeBytes = 7ull * 1024 * 1024 * 1024;  // 7 GB for LiveOnly
        plan.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        plan.strategy = Strategy::UseFreeAll;
        plan.allocationMode = AllocationMode::LiveOnly;

        engine.run(std::move(plan));

        waitForFinished(&engine, finishedSpy);

        auto args = finishedSpy.first();
        QVERIFY(args[0].toBool() == true);
    }

    // Test FullInstall with larger size (50 GB)
    void testFullInstall50GB() {
        MockBackend backend;
        backend.setExpectations({
            {"preflight", true},
            {"validate", true},
            {"iso", true},
            {"layout", true},
            {"mount", true},
            {"copy", true},
            {"patch", true},
            {"bootentry", true},
        });

        Disk disk;
        disk.number = 1;
        disk.sizeBytes = 500ull * 1024 * 1024 * 1024;
        backend.setDisks({disk});

        ProgressLog log;
        InstallEngine engine(std::make_unique<MockBackend>(std::move(backend)), &catalog_, &log);

        QSignalSpy finishedSpy(&engine, &InstallEngine::finished);

        InstallPlan plan;
        plan.distroKey = "test";
        plan.isoPath = "/tmp/test.iso";
        plan.targetDiskNumber = 1;
        plan.linuxSizeBytes = 50ull * 1024 * 1024 * 1024;  // 50 GB
        plan.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        plan.strategy = Strategy::UseFreeAll;
        plan.allocationMode = AllocationMode::FullInstall;

        engine.run(std::move(plan));

        waitForFinished(&engine, finishedSpy);

        auto args = finishedSpy.first();
        QVERIFY(args[0].toBool() == true);
    }

    // Test layout failure (simulated)
    void testLayoutFailure() {
        MockBackend backend;
        backend.setExpectations({
            {"preflight", true},
            {"validate", true},
            {"iso", true},
            {"layout", false},  // Fail at layout
        });

        Disk disk;
        disk.number = 1;
        disk.sizeBytes = 500ull * 1024 * 1024 * 1024;
        backend.setDisks({disk});

        ProgressLog log;
        InstallEngine engine(std::make_unique<MockBackend>(std::move(backend)), &catalog_, &log);

        QSignalSpy finishedSpy(&engine, &InstallEngine::finished);

        InstallPlan plan;
        plan.distroKey = "test";
        plan.isoPath = "/tmp/test.iso";
        plan.targetDiskNumber = 1;
        plan.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        plan.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        plan.strategy = Strategy::UseFreeAll;

        engine.run(std::move(plan));

        waitForFinished(&engine, finishedSpy);

        auto args = finishedSpy.first();
        QVERIFY(args[0].toBool() == false);
        QVERIFY(args[1].toString().contains("Simulated failure"));
    }

private:
    Catalog catalog_;
};

}  // namespace ulli::core

QTEST_GUILESS_MAIN(ulli::core::TestEngine)
#include "test_engine.moc"