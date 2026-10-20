// tests/test_ui_lifecycle.cpp
//
// Regression tests for the "Start installation" flow:
//
//  * The disk enumeration must not run on the GUI thread. It used to run
//    synchronously inside the click handler, which froze the window
//    (a PowerShell round trip on Windows) before the planning screen could
//    even be shown.
//  * The install worker thread must be stopped before the window goes
//    away. Destroying a running QThread aborts the process
//    ("QThread: Destroyed while thread is still running").
//  * Closing the window while the worker is stuck in a non-cancellable
//    operation must neither abort nor hang forever.

#include "ui/MainWindow.h"

#include "core/DiskInfo.h"
#include "core/InstallEngine.h"
#include "core/InstallPlan.h"

#include <QElapsedTimer>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <QtTest/QtTest>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

// Shared between the test and the mock backend so both can observe what
// happened on the worker thread.
struct Recorder {
    std::mutex mutex;
    std::thread::id guiThread;
    std::thread::id scanThread;
    bool ranOffGuiThread = false;
    int enumerateCalls = 0;
    int scanDelayMs = 0;
    // GUI timer ticks observed while the scan was running. If the GUI
    // thread had been blocked, this would stay at zero.
    int guiTicksDuringScan = 0;
    bool blockingStageEntered = false;

    // Filled in by the planning step and the install result handler.
    int plannedDiskCount = 0;
    std::string plannedDistroKey;
    std::string plannedIsoPath;
    bool installSucceeded = false;
    std::string installMessage;

    std::shared_ptr<std::atomic<int>> guiTicks =
        std::make_shared<std::atomic<int>>(0);
};

class MockBackend : public ulli::core::IPlatformBackend {
public:
    explicit MockBackend(std::shared_ptr<Recorder> rec) : rec_(std::move(rec)) {}

    std::vector<ulli::core::Disk> enumerateDisks(
        std::string* errorMessage = nullptr) override {
        if (errorMessage) errorMessage->clear();
        const int before = rec_->guiTicks->load();
        {
            std::lock_guard<std::mutex> lock(rec_->mutex);
            rec_->enumerateCalls++;
            rec_->scanThread = std::this_thread::get_id();
            rec_->ranOffGuiThread = (std::this_thread::get_id() != rec_->guiThread);
        }
        if (rec_->scanDelayMs > 0) {
            QThread::msleep(static_cast<unsigned long>(rec_->scanDelayMs));
        }
        {
            std::lock_guard<std::mutex> lock(rec_->mutex);
            rec_->guiTicksDuringScan = rec_->guiTicks->load() - before;
        }

        std::vector<ulli::core::Disk> disks;
        ulli::core::Disk disk;
        disk.number = 0;  // Windows disk numbers are 0-based.
        disk.model = "Mock Disk";
        disk.sizeBytes = 500ull * 1024 * 1024 * 1024;
        disk.style = ulli::core::PartitionStyle::GPT;
        disks.push_back(disk);
        return disks;
    }

    ulli::core::Result<void> preflight(
        const ulli::core::InstallPlan&) override {
        enterBlockingStage();
        return ulli::core::makeOk();
    }

    ulli::core::Result<void> validatePlanForDisk(
        const ulli::core::InstallPlan&) override {
        return ulli::core::makeOk();
    }

    ulli::core::Result<std::filesystem::path> resolveIso(
        const ulli::core::Distro&) override {
        return ulli::core::makeOk<std::filesystem::path>("/tmp/mock.iso");
    }

    ulli::core::Result<std::filesystem::path> downloadIso(
        const ulli::core::Distro&,
        const std::filesystem::path&,
        std::function<void(int, const QString&)>) override {
        return ulli::core::makeOk<std::filesystem::path>("/tmp/mock.iso");
    }

    ulli::core::Result<std::uint64_t> shrinkPartition(
        char, std::uint64_t) override {
        return ulli::core::makeOk<std::uint64_t>(0);
    }

    ulli::core::Result<void> wipeDisk(int) override {
        return ulli::core::makeOk();
    }

    ulli::core::Result<void> createLayout(
        const ulli::core::InstallPlan&, std::filesystem::path& bootMount,
        std::filesystem::path&, std::function<bool()>) override {
        bootMount = "/tmp/mock-boot";
        return ulli::core::makeOk();
    }

    ulli::core::Result<std::filesystem::path> mountIso(
        const std::filesystem::path&) override {
        return ulli::core::makeOk<std::filesystem::path>("/tmp/mock-iso");
    }

    void unmountIso(const std::filesystem::path&) override {}

    ulli::core::Result<void> copyFiles(
        const std::filesystem::path&, const std::filesystem::path&,
        const ulli::core::Distro*, std::function<bool()>) override {
        return ulli::core::makeOk();
    }

    ulli::core::Result<void> patchDistroBootConfig(
        const ulli::core::Distro&, const ulli::core::InstallPlan&) override {
        return ulli::core::makeOk();
    }

    ulli::core::Result<void> installRefind(
        const ulli::core::InstallPlan&) override {
        return ulli::core::makeOk();
    }

    ulli::core::Result<void> createBootEntry(
        const ulli::core::InstallPlan&) override {
        return ulli::core::makeOk();
    }

    void rollbackBootEntry(const ulli::core::InstallPlan&) override {}
    void rollbackStagingPartition(const ulli::core::InstallPlan&) override {}
    void restartSystem() override {}

    // Simulates a backend call that cannot be interrupted (an external
    // command that is still running). The engine cannot cancel it.
    void enterBlockingStage() {
        if (blockForMs_ <= 0) return;
        {
            std::lock_guard<std::mutex> lock(rec_->mutex);
            rec_->blockingStageEntered = true;
        }
        QThread::msleep(static_cast<unsigned long>(blockForMs_));
    }

    void setBlockForMs(int ms) { blockForMs_ = ms; }

private:
    std::shared_ptr<Recorder> rec_;
    int blockForMs_ = 0;
};

}  // namespace

class TestWindow : public ulli::ui::MainWindow {
public:
    explicit TestWindow(std::shared_ptr<Recorder> rec)
        : rec_(std::move(rec)) {
        rec_->guiThread = std::this_thread::get_id();
    }

    std::unique_ptr<ulli::core::IPlatformBackend> createBackend() override {
        auto backend = std::make_unique<MockBackend>(rec_);
        backend->setBlockForMs(blockForMs_);
        return backend;
    }

    bool showPlanDialog(const std::vector<ulli::core::Disk>& disks,
                        const std::string& distroKey,
                        const std::filesystem::path& isoPath,
                        ulli::core::InstallPlan& outPlan) override {
        {
            std::lock_guard<std::mutex> lock(rec_->mutex);
            rec_->plannedDiskCount = static_cast<int>(disks.size());
            rec_->plannedDistroKey = distroKey;
            rec_->plannedIsoPath = isoPath.string();
        }
        planStep_.release();
        // Cancel: the tests drive the engine directly.
        return false;
    }

    bool isoReadyForInstall() const override { return true; }
    bool hasInstallPrivileges() const override { return true; }

    void showInstallResult(bool success, const QString& message) override {
        {
            std::lock_guard<std::mutex> lock(rec_->mutex);
            rec_->installSucceeded = success;
            rec_->installMessage = message.toStdString();
        }
        installDone_.release();
    }

    void startEngineForTest(ulli::core::InstallPlan plan) {
        startEngine(std::move(plan));
    }

    void setBlockForMs(int ms) { blockForMs_ = ms; }

    // Polls instead of blocking on the semaphore: the test must keep the
    // event loop running, otherwise it cannot prove that the GUI stayed
    // responsive while the disks were being scanned.
    bool waitForPlanStep(int timeoutMs = 5000) {
        QElapsedTimer timer;
        timer.start();
        while (!planStep_.tryAcquire(1)) {
            if (timer.elapsed() > timeoutMs) return false;
            QTest::qWait(10);
        }
        return true;
    }

    bool waitForInstallResult(int timeoutMs = 5000) {
        QElapsedTimer timer;
        timer.start();
        while (!installDone_.tryAcquire(1)) {
            if (timer.elapsed() > timeoutMs) return false;
            QTest::qWait(10);
        }
        return true;
    }

private:
    std::shared_ptr<Recorder> rec_;
    int blockForMs_ = 0;
    QSemaphore planStep_;
    QSemaphore installDone_;
};

class TestUiLifecycle : public QObject {
    Q_OBJECT
private slots:
    // Priority 1: the click handler must not block the GUI thread.
    void diskScanRunsOffTheGuiThread() {
        auto rec = std::make_shared<Recorder>();
        rec->scanDelayMs = 400;

        TestWindow w(rec);
        w.show();

        // A timer that only fires while the GUI event loop is running.
        QTimer ticker;
        ticker.setInterval(20);
        QObject::connect(&ticker, &QTimer::timeout, [rec]() {
            rec->guiTicks->fetch_add(1);
        });
        ticker.start();

        QVERIFY(QMetaObject::invokeMethod(&w, "onStartClicked",
                                          Qt::QueuedConnection));
        QVERIFY2(w.waitForPlanStep(10000), "the disk scan never completed");

        {
            std::lock_guard<std::mutex> lock(rec->mutex);
            QCOMPARE(rec->enumerateCalls, 1);
            QVERIFY2(rec->ranOffGuiThread,
                     "enumerateDisks() ran on the GUI thread - the window "
                     "would freeze before the planning screen appears");
            QVERIFY2(rec->guiTicksDuringScan > 0,
                     "the GUI event loop was blocked while the disks were "
                     "being scanned");
            QCOMPARE(rec->plannedDiskCount, 1);
        }
    }

    // Priority 2: a finished install must leave no live worker behind, so
    // destroying the window cannot abort the process.
    void windowDestroyedAfterInstallCompletes() {
        auto rec = std::make_shared<Recorder>();

        {
            TestWindow w(rec);
            w.startEngineForTest(makePlan());
            QVERIFY2(w.waitForInstallResult(10000),
                     "the install never finished");
        }
        // The window destructor ran here. Before the fix this aborted the
        // process with "QThread: Destroyed while thread is still running".
        {
            std::lock_guard<std::mutex> lock(rec->mutex);
            QVERIFY(rec->installSucceeded);
        }
    }

    // Priority 2: closing the window while the worker is stuck in a
    // non-cancellable operation must not abort and must not hang.
    void windowClosedDuringActiveOperation() {
        auto rec = std::make_shared<Recorder>();

        TestWindow* w = new TestWindow(rec);
        // The backend blocks for 8 s inside preflight; the engine cannot
        // cancel that, so the shutdown path has to detach the worker.
        w->setBlockForMs(8000);
        w->startEngineForTest(makePlan());

        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000) {
            {
                std::lock_guard<std::mutex> lock(rec->mutex);
                if (rec->blockingStageEntered) break;
            }
            QTest::qWait(10);
        }
        {
            std::lock_guard<std::mutex> lock(rec->mutex);
            QVERIFY2(rec->blockingStageEntered,
                     "the worker never reached the blocking stage");
        }

        // Destroying the window must return (bounded wait) and must not
        // abort the process.
        delete w;
        QVERIFY2(timer.elapsed() < 30000,
                 "the window took too long to close");
    }

private:
    static ulli::core::InstallPlan makePlan() {
        ulli::core::InstallPlan plan;
        plan.distroKey = "mint";  // present in the built-in catalog
        plan.targetDiskNumber = 0;  // 0 is a valid Windows disk number
        plan.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
        plan.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
        plan.strategy = ulli::core::Strategy::UseFreeAll;
        plan.allocationMode = ulli::core::AllocationMode::FullInstall;
        return plan;
    }
};

QTEST_MAIN(TestUiLifecycle)
#include "test_ui_lifecycle.moc"
