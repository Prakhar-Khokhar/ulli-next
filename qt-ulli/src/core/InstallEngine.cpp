// core/InstallEngine.cpp
#include "core/InstallEngine.h"

#include "core/Catalog.h"

#include <QMetaType>

#include <utility>

namespace ulli::core {

namespace {
constexpr int kProgressStart     = 0;
constexpr int kProgressPreflight = 5;
constexpr int kProgressIso       = 20;
constexpr int kProgressResize    = 30;
constexpr int kProgressWipe      = 35;
constexpr int kProgressLayout    = 50;
constexpr int kProgressCopy      = 75;
constexpr int kProgressPatch     = 80;
constexpr int kProgressRefind    = 85;
constexpr int kProgressBootEntry = 90;
constexpr int kProgressCleanup   = 95;
constexpr int kProgressDone      = 100;
}

InstallEngine::InstallEngine(std::unique_ptr<IPlatformBackend> backend,
                             const Catalog* catalog,
                             ProgressLog* log,
                             QObject* parent)
    : QObject(parent)
    , backend_(std::move(backend))
    , catalog_(catalog)
    , log_(log) {
    qRegisterMetaType<ProgressLog::Severity>("ProgressLog::Severity");
}

InstallEngine::~InstallEngine() = default;

void InstallEngine::run(InstallPlan plan) {
    const bool cancelRequested = cancelFlag_.loadAcquire() != 0;
    if (cancelRequested) {
        emit finished(false, "Cancelled before start", false);
        return;
    }

    emit progressChanged(kProgressStart);

    const bool autoRestart = plan.autoRestart;
    auto r = runStages(plan);
    if (cancelFlag_.loadAcquire() != 0) {
        log_->append("Installation cancelled by user", ProgressLog::Severity::Warn);
        // Best-effort rollback of any bcdedit entry we may have created.
        if (plan.bcdGuid.has_value()) {
            backend_->rollbackBootEntry(plan);
        }
        emit finished(false, "Cancelled by user", false);
        return;
    }

    if (!r) {
        log_->append(QString("Installation failed: %1").arg(r.error().qmessage()),
                     ProgressLog::Severity::Error);
        if (plan.bcdGuid.has_value()) {
            backend_->rollbackBootEntry(plan);
        }
        emit finished(false, r.error().qmessage(), false);
        return;
    }

    emit progressChanged(kProgressDone);
    emit finished(true, "Installation completed", autoRestart);
}

void InstallEngine::requestCancel() {
    cancelFlag_.storeRelease(1);
}

Result<void> InstallEngine::runStages(InstallPlan& plan) {
    if (!plan.valid()) {
        return makeError(Error::Kind::InvalidInput, "Install plan failed validation");
    }
    const Distro* distro = catalog_->find(plan.distroKey);
    if (!distro) {
        return makeError(Error::Kind::InvalidInput,
                         "Unknown distro key: " + plan.distroKey);
    }

    // 1. Pre-flight
    emit stageChanged("Pre-flight checks");
    emit progressChanged(kProgressPreflight);
    if (auto r = backend_->preflight(plan); !r) return r;
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();

    // 2. Resolve ISO (download if needed, verify checksum)
    emit stageChanged("Resolving ISO");
    emit progressChanged(kProgressIso);
    auto isoR = backend_->resolveIso(*distro);
    if (!isoR) return makeError(isoR.error().kind(), isoR.error().message());
    plan.isoPath = isoR.value();
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();

    // 3. Resize
    if (plan.strategy == Strategy::ShrinkAll ||
        plan.strategy == Strategy::UseFreeBoot ||
        plan.strategy == Strategy::OtherDriveShrink) {
        emit stageChanged("Resizing existing partition");
        emit progressChanged(kProgressResize);
        if (cancelFlag_.loadAcquire() != 0) return makeCancelled();
        if (plan.shrinkDriveLetter.has_value()) {
            // The backend figures out the new size from the requested
            // shrink amount stored on the plan.
            auto r = backend_->shrinkPartition(plan.shrinkDriveLetter.value(),
                                              plan.shrinkAmountBytes);
            if (!r) {
                return makeError(r.error().kind(), r.error().message());
            }
        }
        if (cancelFlag_.loadAcquire() != 0) return makeCancelled();
    }

    // 4. Wipe (only for WipeDisk)
    if (plan.strategy == Strategy::WipeDisk) {
        emit stageChanged("Wiping target disk");
        emit progressChanged(kProgressWipe);
        if (cancelFlag_.loadAcquire() != 0) return makeCancelled();
        if (auto r = backend_->wipeDisk(plan.targetDiskNumber); !r) return r;
        if (cancelFlag_.loadAcquire() != 0) return makeCancelled();
    }

    // 5. Create layout
    emit stageChanged("Creating partitions and filesystems");
    emit progressChanged(kProgressLayout);
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();
    std::filesystem::path bootMount, refindMount;
    if (auto r = backend_->createLayout(plan, bootMount, refindMount); !r) return r;
    plan.bootPartitionMount = bootMount;
    if (!refindMount.empty()) plan.refindPartitionMount = refindMount;
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();

    // 6. Mount ISO + copy
    emit stageChanged("Copying live ISO contents");
    emit progressChanged(kProgressCopy);
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();
    auto isoMount = backend_->mountIso(plan.isoPath);
    if (!isoMount) return makeError(isoMount.error().kind(), isoMount.error().message());
    {
        auto copyR = backend_->copyFiles(isoMount.value(), bootMount);
        backend_->unmountIso(isoMount.value());
        if (!copyR) return copyR;
    }
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();

    // 7. Distro-specific patches
    emit stageChanged("Patching distro boot configuration");
    emit progressChanged(kProgressPatch);
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();
    if (auto r = backend_->patchDistroBootConfig(*distro, plan); !r) {
        log_->append(QString("Distro config patch failed: %1")
                         .arg(r.error().qmessage()),
                     ProgressLog::Severity::Warn);
    }

    // 8. rEFInd
    if (plan.bootMode == BootMode::Refind) {
        emit stageChanged("Installing rEFInd");
        emit progressChanged(kProgressRefind);
        if (cancelFlag_.loadAcquire() != 0) return makeCancelled();
        if (auto r = backend_->installRefind(plan); !r) {
            log_->append(QString("rEFInd install failed: %1")
                             .arg(r.error().qmessage()),
                         ProgressLog::Severity::Warn);
        }
    }

    // 9. UEFI boot entry
    emit stageChanged("Creating UEFI boot entry");
    emit progressChanged(kProgressBootEntry);
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();
    if (auto r = backend_->createBootEntry(plan); !r) {
        return r;  // engine.run will roll back via bcdGuid if set
    }

    // 10. Cleanup
    emit stageChanged("Cleaning up");
    emit progressChanged(kProgressCleanup);
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();

    return makeOk();
}

}  // namespace ulli::core
