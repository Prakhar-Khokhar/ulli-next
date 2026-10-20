// tests/test_workflow.cpp
//
// Regression tests for the ULLI install workflow state machine.

#include "ui/MainWindow.h"
#include "ui/DistroSelector.h"

#include "core/Catalog.h"
#include "core/DiskInfo.h"
#include "core/InstallEngine.h"
#include "core/InstallPlan.h"
#include "core/Result.h"
#include "core/Sha256.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMetaObject>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <optional>
#include <thread>
#include <vector>

namespace {

struct Recorder {
    std::mutex mutex;
    std::thread::id guiThread;
    std::thread::id scanThread;
    bool ranOffGuiThread = false;

    // scan
    int enumerateCalls = 0;
    int scanDelayMs = 0;
    int guiTicksDuringScan = 0;
    std::shared_ptr<std::atomic<int>> guiTicks =
        std::make_shared<std::atomic<int>>(0);

    // plan dialog
    int planDialogCalls = 0;
    bool planApprovedResult = true;
    bool planValid = true;
    std::string plannedTargetDiskModel;
    std::uint64_t plannedTargetDiskSizeBytes = 0;

    // download config
    bool downloadShouldFail = false;
    bool downloadCancelled = false;
    bool downloadVerifiedResult = true;
    int downloadDelayMs = 0;
    int downloadCalls = 0;
    bool downloadCancelObserved = false;
    bool downloadFinished = false;
    bool lastDownloadVerified = false;
    std::string lastDownloadSha256;
    std::filesystem::path lastDownloadDest;
    int downloadProgressMax = 0;

    // install result
    int installResultCalls = 0;
    bool installSucceeded = false;
    std::string installMessage;

    // engine revalidation
    std::vector<std::string> callOrder;
    std::optional<ulli::core::InstallPlan> preflightPlan;
    std::vector<ulli::core::InstallPlan> validatePlans;
    int wipeCalls = 0;
    int createLayoutCalls = 0;
    int resolveCalls = 0;
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
        disk.number = 0;
        disk.model = "Mock Disk Model";
        disk.sizeBytes = 500ull * 1024 * 1024 * 1024;
        disk.style = ulli::core::PartitionStyle::GPT;
        disk.busType = "NVMe";
        disks.push_back(disk);
        return disks;
    }

    ulli::core::Result<void> preflight(
        const ulli::core::InstallPlan& plan) override {
        std::lock_guard<std::mutex> lock(rec_->mutex);
        rec_->callOrder.push_back("preflight");
        rec_->preflightPlan = plan;
        return ulli::core::makeOk();
    }

    ulli::core::Result<void> validatePlanForDisk(
        const ulli::core::InstallPlan& plan) override {
        std::lock_guard<std::mutex> lock(rec_->mutex);
        rec_->callOrder.push_back("validate");
        rec_->validatePlans.push_back(plan);
        return ulli::core::makeOk();
    }

    ulli::core::Result<std::filesystem::path> resolveIso(
        const ulli::core::Distro&) override {
        std::lock_guard<std::mutex> lock(rec_->mutex);
        rec_->resolveCalls++;
        rec_->callOrder.push_back("resolveIso");
        return ulli::core::makeOk<std::filesystem::path>("/tmp/mock-resolved.iso");
    }

    ulli::core::Result<ulli::core::IsoDownload> downloadIso(
        const ulli::core::Distro&,
        const std::filesystem::path& destPath,
        std::function<void(int, const QString&)> progressCallback,
        const std::atomic<bool>* cancelFlag) override {
        {
            std::lock_guard<std::mutex> lock(rec_->mutex);
            rec_->downloadCalls++;
            rec_->callOrder.push_back("downloadIso");
            rec_->lastDownloadDest = destPath;
        }

        std::filesystem::create_directories(destPath.parent_path());

        // honour an already-set cancel flag
        if (cancelFlag && cancelFlag->load()) {
            std::lock_guard<std::mutex> lock(rec_->mutex);
            rec_->downloadCancelObserved = true;
            return ulli::core::makeError<ulli::core::IsoDownload>(
                ulli::core::Error::Kind::Cancelled, "Cancelled");
        }

        // write a real, deterministic payload
        {
            std::ofstream f(destPath, std::ios::binary | std::ios::trunc);
            for (int i = 0; i < 64; ++i) {
                f << "ULLI mock ISO payload block " << i << "\n";
            }
            f.flush();
        }

        const int delay = rec_->downloadDelayMs > 0 ? rec_->downloadDelayMs : 0;
        const int steps = delay > 0 ? 10 : 1;
        const int stepDelay = delay > 0 ? (delay / steps) : 0;

        for (int i = 0; i <= steps; ++i) {
            if (cancelFlag && cancelFlag->load()) {
                std::error_code ec;
                std::filesystem::remove(destPath, ec);
                {
                    std::lock_guard<std::mutex> lock(rec_->mutex);
                    rec_->downloadCancelObserved = true;
                }
                return ulli::core::makeError<ulli::core::IsoDownload>(
                    ulli::core::Error::Kind::Cancelled, "Cancelled");
            }
            int percent = (steps == 0) ? 100 : (i * 100 / steps);
            if (percent > 100) percent = 100;
            if (progressCallback) progressCallback(percent, QString("Downloading %1%").arg(percent));
            {
                std::lock_guard<std::mutex> lock(rec_->mutex);
                if (percent > rec_->downloadProgressMax) rec_->downloadProgressMax = percent;
            }
            if (i < steps && stepDelay > 0) {
                QThread::msleep(static_cast<unsigned long>(stepDelay));
            }
        }

        // failure modes
        {
            std::lock_guard<std::mutex> lock(rec_->mutex);
            if (rec_->downloadCancelled) {
                rec_->downloadCancelObserved = true;
                std::error_code ec;
                std::filesystem::remove(destPath, ec);
                return ulli::core::makeError<ulli::core::IsoDownload>(
                    ulli::core::Error::Kind::Cancelled, "Cancelled by mock");
            }
            if (rec_->downloadShouldFail) {
                std::error_code ec;
                std::filesystem::remove(destPath, ec);
                return ulli::core::makeError<ulli::core::IsoDownload>(
                    ulli::core::Error::Kind::Platform, "Mock download failure");
            }
        }

        auto digest = ulli::core::Sha256::hashFile(destPath);
        const std::string sha = digest ? digest.value() : std::string();

        if (progressCallback) progressCallback(100, QString("Transfer complete"));

        {
            std::lock_guard<std::mutex> lock(rec_->mutex);
            rec_->downloadFinished = true;
            rec_->lastDownloadVerified = rec_->downloadVerifiedResult;
            rec_->lastDownloadSha256 = sha;
            rec_->downloadProgressMax = 100;
        }

        ulli::core::IsoDownload out;
        out.path = destPath;
        out.verified = rec_->downloadVerifiedResult;
        out.sha256 = sha;
        return ulli::core::makeOk(out);
    }

    ulli::core::Result<std::uint64_t> shrinkPartition(char, std::uint64_t) override {
        return ulli::core::makeOk<std::uint64_t>(0);
    }
    ulli::core::Result<void> wipeDisk(int) override {
        std::lock_guard<std::mutex> lock(rec_->mutex);
        rec_->wipeCalls++;
        rec_->callOrder.push_back("wipeDisk");
        return ulli::core::makeOk();
    }
    ulli::core::Result<void> createLayout(
        const ulli::core::InstallPlan&, std::filesystem::path& bootMount,
        std::filesystem::path&, std::function<bool()>) override {
        std::lock_guard<std::mutex> lock(rec_->mutex);
        rec_->createLayoutCalls++;
        rec_->callOrder.push_back("createLayout");
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
    ulli::core::Result<void> installRefind(const ulli::core::InstallPlan&) override {
        return ulli::core::makeOk();
    }
    ulli::core::Result<void> createBootEntry(const ulli::core::InstallPlan&) override {
        return ulli::core::makeOk();
    }
    void rollbackBootEntry(const ulli::core::InstallPlan&) override {}
    void rollbackStagingPartition(const ulli::core::InstallPlan&) override {}
    void restartSystem() override {}

private:
    std::shared_ptr<Recorder> rec_;
};

class TestWindow : public ulli::ui::MainWindow {
public:
    explicit TestWindow(std::shared_ptr<Recorder> rec,
                        const std::filesystem::path& cacheDir,
                        bool privileges = true)
        : rec_(std::move(rec)), cacheDir_(cacheDir), privileges_(privileges) {
        rec_->guiThread = std::this_thread::get_id();
    }

    std::unique_ptr<ulli::core::IPlatformBackend> createBackend() override {
        return std::make_unique<MockBackend>(rec_);
    }

    bool showPlanDialog(const std::vector<ulli::core::Disk>& disks,
                        const std::string&,
                        const std::filesystem::path&,
                        ulli::core::InstallPlan& outPlan) override {
        {
            std::lock_guard<std::mutex> lock(rec_->mutex);
            rec_->planDialogCalls++;
            if (!disks.empty()) {
                rec_->plannedTargetDiskModel = disks[0].model;
                rec_->plannedTargetDiskSizeBytes = disks[0].sizeBytes;
            }
        }
        if (!rec_->planApprovedResult) return false;

        if (rec_->planValid) {
            outPlan.distroKey = "mint";
            outPlan.targetDiskNumber = 0;
            outPlan.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
            outPlan.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
            outPlan.strategy = ulli::core::Strategy::UseFreeAll;
            outPlan.allocationMode = ulli::core::AllocationMode::FullInstall;
            if (!disks.empty()) {
                outPlan.targetDiskModel = disks[0].model;
                outPlan.targetDiskSizeBytes = disks[0].sizeBytes;
            } else {
                outPlan.targetDiskModel = "Mock Disk Model";
                outPlan.targetDiskSizeBytes = 500ull * 1024 * 1024 * 1024;
            }
        } else {
            outPlan.distroKey = "mint";
            outPlan.targetDiskNumber = -1;
        }
        return true;
    }

    bool isoReadyForInstall() const override { return true; }
    bool hasInstallPrivileges() const override { return privileges_; }

    std::filesystem::path isoCachePathFor(const ulli::core::Distro& distro) const override {
        return cacheDir_ / distro.isoFilename();
    }

    void showInstallResult(bool success, const QString& message) override {
        std::lock_guard<std::mutex> lock(rec_->mutex);
        rec_->installResultCalls++;
        rec_->installSucceeded = success;
        rec_->installMessage = message.toStdString();
    }

    void startEngineForTest(ulli::core::InstallPlan plan) {
        startEngine(std::move(plan));
    }

    void setPrivileges(bool v) { privileges_ = v; }

private:
    std::shared_ptr<Recorder> rec_;
    std::filesystem::path cacheDir_;
    bool privileges_;
};

struct DialogDismisser {
    QObject* ctx = nullptr;
    QTimer* timer = nullptr;
    DialogDismisser() {
        ctx = new QObject();
        timer = new QTimer(ctx);
        QObject::connect(timer, &QTimer::timeout, ctx, []() {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                if (auto* mb = qobject_cast<QMessageBox*>(w)) {
                    mb->done(QDialog::Accepted);
                }
            }
        });
        timer->start(10);
    }
    ~DialogDismisser() { delete ctx; }
};

}  // namespace

static ulli::core::InstallPlan makePlan() {
    ulli::core::InstallPlan plan;
    plan.distroKey = "mint";
    plan.targetDiskNumber = 0;
    plan.linuxSizeBytes = 30ull * 1024 * 1024 * 1024;
    plan.bootSizeBytes = 7ull * 1024 * 1024 * 1024;
    plan.strategy = ulli::core::Strategy::UseFreeAll;
    plan.allocationMode = ulli::core::AllocationMode::FullInstall;
    plan.targetDiskModel = "Mock Disk Model";
    plan.targetDiskSizeBytes = 500ull * 1024 * 1024 * 1024;
    return plan;
}

static bool waitForPred(std::function<bool()> pred, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!pred()) {
        if (timer.elapsed() > timeoutMs) return false;
        QTest::qWait(10);
    }
    return true;
}

class TestWorkflow : public QObject {
    Q_OBJECT
private:
    DialogDismisser* dismisser_ = nullptr;

private slots:
    void init() { dismisser_ = new DialogDismisser(); }
    void cleanup() { delete dismisser_; dismisser_ = nullptr; }

    void cachedIsoReachesPlanReviewWithoutDownload() {
        auto rec = std::make_shared<Recorder>();
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        const auto cachePath = std::filesystem::path(cacheDir.path().toStdString());
        // create a cached ISO file
        {
            std::ofstream f(cachePath / "linuxmint-22.3-cinnamon-64bit.iso", std::ios::binary);
            f << "cached";
        }

        TestWindow w(rec, cachePath, true);
        w.show();

        QMetaObject::invokeMethod(&w, "onStartClicked", Qt::QueuedConnection);
        QVERIFY2(waitForPred([&]() { return rec->planDialogCalls >= 1; }, 10000),
                 "plan review was never reached");
        QVERIFY2(rec->downloadCalls == 0, "a download was started for a cached ISO");
    }

    void missingIsoReachesPlanReviewWithoutDownload() {
        auto rec = std::make_shared<Recorder>();
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        const auto cachePath = std::filesystem::path(cacheDir.path().toStdString());

        TestWindow w(rec, cachePath, true);
        w.show();

        QMetaObject::invokeMethod(&w, "onStartClicked", Qt::QueuedConnection);
        QVERIFY2(waitForPred([&]() { return rec->planDialogCalls >= 1; }, 10000),
                 "plan review was never reached");
        QVERIFY2(rec->downloadCalls == 0, "downloadIso called before approval");
    }

    void approvedPlanDownloadsAndBecomesReady() {
        auto rec = std::make_shared<Recorder>();
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        const auto cachePath = std::filesystem::path(cacheDir.path().toStdString());

        TestWindow w(rec, cachePath, true);
        w.show();

        QMetaObject::invokeMethod(&w, "onStartClicked", Qt::QueuedConnection);
        QVERIFY2(waitForPred([&]() { return rec->downloadFinished; }, 10000),
                 "download never finished");
        QVERIFY2(waitForPred([&]() {
            return w.workflowState() == ulli::ui::MainWindow::WorkflowState::Ready ||
                   w.workflowState() == ulli::ui::MainWindow::WorkflowState::Installing;
        }, 10000), "workflow did not reach Ready/Installing");
        QVERIFY2(rec->downloadCalls == 1, "expected exactly one download");
    }

    void invalidPlanPreventsDownload() {
        auto rec = std::make_shared<Recorder>();
        rec->planValid = false;
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        const auto cachePath = std::filesystem::path(cacheDir.path().toStdString());

        TestWindow w(rec, cachePath, true);
        w.show();

        QMetaObject::invokeMethod(&w, "onStartClicked", Qt::QueuedConnection);
        QVERIFY2(waitForPred([&]() { return rec->planDialogCalls >= 1; }, 10000),
                 "plan review was never reached");
        QTest::qWait(200);
        QVERIFY2(rec->downloadCalls == 0, "download started for an invalid plan");
        QVERIFY2(w.workflowState() != ulli::ui::MainWindow::WorkflowState::Ready,
                 "workflow reached Ready with an invalid plan");
    }

    void noDownloadBeforeApproval() {
        auto rec = std::make_shared<Recorder>();
        rec->planApprovedResult = false;
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        const auto cachePath = std::filesystem::path(cacheDir.path().toStdString());

        TestWindow w(rec, cachePath, true);
        w.show();

        QMetaObject::invokeMethod(&w, "onStartClicked", Qt::QueuedConnection);
        QVERIFY2(waitForPred([&]() { return rec->planDialogCalls >= 1; }, 10000),
                 "plan review was never reached");
        QTest::qWait(200);
        QVERIFY2(rec->downloadCalls == 0, "download started without approval");
    }

    void approvalRevalidatesPrerequisitesBeforeDownload() {
        auto rec = std::make_shared<Recorder>();
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        const auto cachePath = std::filesystem::path(cacheDir.path().toStdString());

        TestWindow w(rec, cachePath, false);  // no privileges
        w.show();

        QMetaObject::invokeMethod(&w, "onStartClicked", Qt::QueuedConnection);
        QVERIFY2(waitForPred([&]() { return rec->planDialogCalls >= 1; }, 10000),
                 "plan review was never reached");
        QTest::qWait(200);
        QVERIFY2(rec->downloadCalls == 0, "download started without privileges");
        QVERIFY2(w.workflowState() != ulli::ui::MainWindow::WorkflowState::Downloading,
                 "workflow reached Downloading without privileges");
    }

    void failedDownloadDoesNotBecomeReady() {
        auto rec = std::make_shared<Recorder>();
        rec->downloadShouldFail = true;
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        const auto cachePath = std::filesystem::path(cacheDir.path().toStdString());

        TestWindow w(rec, cachePath, true);
        w.show();

        QMetaObject::invokeMethod(&w, "onStartClicked", Qt::QueuedConnection);
        QVERIFY2(waitForPred([&]() { return rec->downloadCalls >= 1; }, 10000),
                 "download never started");
        QTest::qWait(500);
        QVERIFY2(w.workflowState() != ulli::ui::MainWindow::WorkflowState::Ready,
                 "workflow became Ready after a failed download");
        QVERIFY2(rec->installResultCalls == 0, "engine started after a failed download");
    }

    void cancelledDownloadDoesNotBecomeReady() {
        auto rec = std::make_shared<Recorder>();
        rec->downloadCancelled = true;
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        const auto cachePath = std::filesystem::path(cacheDir.path().toStdString());

        TestWindow w(rec, cachePath, true);
        w.show();

        QMetaObject::invokeMethod(&w, "onStartClicked", Qt::QueuedConnection);
        QVERIFY2(waitForPred([&]() { return rec->downloadCalls >= 1; }, 10000),
                 "download never started");
        QTest::qWait(500);
        QVERIFY2(w.workflowState() != ulli::ui::MainWindow::WorkflowState::Ready,
                 "workflow became Ready after a cancelled download");
    }

    void transferCompletionIsNotReadyWithoutVerification() {
        auto rec = std::make_shared<Recorder>();
        rec->downloadVerifiedResult = false;
        rec->downloadDelayMs = 200;
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        const auto cachePath = std::filesystem::path(cacheDir.path().toStdString());

        TestWindow w(rec, cachePath, true);
        w.show();

        QMetaObject::invokeMethod(&w, "onStartClicked", Qt::QueuedConnection);
        // while downloading, must not be Ready
        QVERIFY2(waitForPred([&]() { return rec->downloadCalls >= 1; }, 10000), "download never started");
        for (int i = 0; i < 5; ++i) {
            QVERIFY2(w.workflowState() != ulli::ui::MainWindow::WorkflowState::Ready,
                     "claimed Ready before download result");
            QTest::qWait(20);
        }
        QVERIFY2(waitForPred([&]() { return rec->downloadFinished; }, 10000),
                 "download never finished");
        QVERIFY2(rec->lastDownloadVerified == false, "mock did not record verified==false");
    }

    void validChecksumBecomesVerified() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto file = std::filesystem::path(tmp.path().toStdString()) / "test.bin";
        {
            std::ofstream f(file, std::ios::binary);
            f << "hello world";
        }
        auto digest = ulli::core::Sha256::hashFile(file);
        QVERIFY(digest);
        auto v = ulli::core::Sha256::verifyFile(file, digest.value());
        QVERIFY(v);
    }

    void invalidChecksumIsRejected() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto file = std::filesystem::path(tmp.path().toStdString()) / "test.bin";
        {
            std::ofstream f(file, std::ios::binary);
            f << "hello world";
        }
        auto v = ulli::core::Sha256::verifyFile(file, std::string(64, '0'));
        QVERIFY(!v);
    }

    void cachedIsoIsReusedAfterRestart() {
        auto rec = std::make_shared<Recorder>();
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        const auto cachePath = std::filesystem::path(cacheDir.path().toStdString());
        {
            std::ofstream f(cachePath / "linuxmint-22.3-cinnamon-64bit.iso", std::ios::binary);
            f << "cached";
        }

        {
            TestWindow w(rec, cachePath, true);
            w.show();
            QMetaObject::invokeMethod(&w, "onStartClicked", Qt::QueuedConnection);
            QVERIFY2(waitForPred([&]() { return rec->planDialogCalls >= 1; }, 10000),
                     "first window never reached plan review");
        }
        const int downloadsAfterFirst = rec->downloadCalls;

        {
            TestWindow w2(rec, cachePath, true);
            w2.show();
            QMetaObject::invokeMethod(&w2, "onStartClicked", Qt::QueuedConnection);
            QVERIFY2(waitForPred([&]() { return rec->planDialogCalls >= 2; }, 10000),
                     "second window never reached plan review");
        }
        QVERIFY2(rec->downloadCalls == downloadsAfterFirst,
                 "cached ISO was not reused (a download started)");
    }

    void changingSelectionInvalidatesApproval() {
        auto rec = std::make_shared<Recorder>();
        rec->downloadDelayMs = 500;
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        const auto cachePath = std::filesystem::path(cacheDir.path().toStdString());

        TestWindow w(rec, cachePath, true);
        w.show();

        QMetaObject::invokeMethod(&w, "onStartClicked", Qt::QueuedConnection);
        QVERIFY2(waitForPred([&]() { return rec->downloadCalls >= 1; }, 10000),
                 "download never started");
        // invalidate while downloading
        QMetaObject::invokeMethod(&w, "onSelectionChanged", Qt::QueuedConnection);
        QVERIFY2(waitForPred([&]() { return rec->downloadCancelObserved || rec->downloadFinished; }, 10000),
                 "download was not cancelled");
        QTest::qWait(200);
        QVERIFY2(w.workflowState() != ulli::ui::MainWindow::WorkflowState::Ready,
                 "workflow became Ready after selection change");
    }

    void staleDownloadCallbackDoesNotChangeNewSelection() {
        auto rec = std::make_shared<Recorder>();
        rec->downloadDelayMs = 300;
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        const auto cachePath = std::filesystem::path(cacheDir.path().toStdString());

        TestWindow w(rec, cachePath, true);
        w.show();

        QMetaObject::invokeMethod(&w, "onStartClicked", Qt::QueuedConnection);
        QVERIFY2(waitForPred([&]() { return rec->downloadCalls >= 1; }, 10000),
                 "download never started");
        QMetaObject::invokeMethod(&w, "onSelectionChanged", Qt::QueuedConnection);
        // wait for the stale download to finish being ignored
        QTest::qWait(600);
        QVERIFY2(w.workflowState() != ulli::ui::MainWindow::WorkflowState::Ready,
                 "stale download callback made the new selection Ready");
    }

    void guiStaysResponsiveDuringScanDownloadAndVerification() {
        auto rec = std::make_shared<Recorder>();
        rec->scanDelayMs = 200;
        rec->downloadDelayMs = 300;
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        const auto cachePath = std::filesystem::path(cacheDir.path().toStdString());

        TestWindow w(rec, cachePath, true);
        w.show();

        QTimer ticker;
        ticker.setInterval(10);
        QObject::connect(&ticker, &QTimer::timeout, [rec]() {
            rec->guiTicks->fetch_add(1);
        });
        ticker.start();

        const int beforeScan = rec->guiTicks->load();
        QMetaObject::invokeMethod(&w, "onStartClicked", Qt::QueuedConnection);
        QVERIFY2(waitForPred([&]() { return rec->planDialogCalls >= 1; }, 10000),
                 "plan review never reached");
        QVERIFY2(rec->guiTicks->load() > beforeScan, "GUI thread blocked during scan");

        QVERIFY2(waitForPred([&]() { return rec->downloadFinished; }, 10000),
                 "download never finished");
        QVERIFY2(rec->guiTicks->load() > beforeScan + 1, "GUI thread blocked during download");
    }

    void closingWindowDuringDownloadIsSafe() {
        auto rec = std::make_shared<Recorder>();
        rec->downloadDelayMs = 5000;
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        const auto cachePath = std::filesystem::path(cacheDir.path().toStdString());

        TestWindow* w = new TestWindow(rec, cachePath, true);
        w->show();

        QMetaObject::invokeMethod(w, "onStartClicked", Qt::QueuedConnection);
        QVERIFY2(waitForPred([&]() { return rec->downloadCalls >= 1; }, 10000),
                 "download never started");

        QElapsedTimer timer;
        timer.start();
        delete w;
        QVERIFY2(timer.elapsed() < 15000, "window took too long to close");

        // wait for cancel observation
        bool observed = false;
        QElapsedTimer t2;
        t2.start();
        while (t2.elapsed() < 2000) {
            {
                std::lock_guard<std::mutex> lock(rec->mutex);
                if (rec->downloadCancelObserved) { observed = true; break; }
            }
            QThread::msleep(10);
        }
        QVERIFY2(observed, "mock did not observe the cancel flag");
    }

    void diskIdentityIsCarriedInThePlan() {
        auto rec = std::make_shared<Recorder>();
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        const auto cachePath = std::filesystem::path(cacheDir.path().toStdString());

        TestWindow w(rec, cachePath, true);
        w.show();

        QMetaObject::invokeMethod(&w, "onStartClicked", Qt::QueuedConnection);
        QVERIFY2(waitForPred([&]() { return rec->planDialogCalls >= 1; }, 10000),
                 "plan review never reached");
        QCOMPARE(QString::fromStdString(rec->plannedTargetDiskModel), QString("Mock Disk Model"));
        QCOMPARE(rec->plannedTargetDiskSizeBytes, 500ull * 1024 * 1024 * 1024);
    }

    void engineRevalidatesPlanBeforeMutation() {
        auto rec = std::make_shared<Recorder>();
        QTemporaryDir cacheDir;
        QVERIFY(cacheDir.isValid());
        const auto cachePath = std::filesystem::path(cacheDir.path().toStdString());

        TestWindow w(rec, cachePath, true);
        w.show();

        w.startEngineForTest(makePlan());
        QVERIFY2(waitForPred([&]() { return rec->installResultCalls >= 1; }, 10000),
                 "engine never finished");

        {
            std::lock_guard<std::mutex> lock(rec->mutex);
            QVERIFY2(rec->preflightPlan.has_value(), "preflight not called");
            QCOMPARE(QString::fromStdString(rec->preflightPlan->targetDiskModel),
                     QString("Mock Disk Model"));
            QCOMPARE(rec->preflightPlan->targetDiskSizeBytes,
                     500ull * 1024 * 1024 * 1024);
            QVERIFY2(!rec->validatePlans.empty(), "validatePlanForDisk not called");
            QCOMPARE(QString::fromStdString(rec->validatePlans.back().targetDiskModel),
                     QString("Mock Disk Model"));

            // order: preflight before validate before createLayout
            auto findIdx = [&](const std::string& name) {
                for (std::size_t i = 0; i < rec->callOrder.size(); ++i) {
                    if (rec->callOrder[i] == name) return static_cast<int>(i);
                }
                return -1;
            };
            const int pi = findIdx("preflight");
            const int vi = findIdx("validate");
            const int ci = findIdx("createLayout");
            QVERIFY2(pi >= 0 && vi >= 0 && ci >= 0, "missing expected calls");
            QVERIFY2(pi < vi, "preflight not before validate");
            QVERIFY2(vi < ci, "validate not before createLayout");
        }
    }
};

QTEST_MAIN(TestWorkflow)
#include "test_workflow.moc"
