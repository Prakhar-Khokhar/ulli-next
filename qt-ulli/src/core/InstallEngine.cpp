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

// A custom ISO is not in the catalog, so there is no expected checksum to
// compare against. We still verify that the path points at a usable file so
// a typo cannot reach the partitioning stage.
Result<void> validateCustomIso(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return makeError(Error::Kind::NotFound,
                         "Custom ISO file does not exist: " + path.string());
    }
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return makeError(Error::Kind::InvalidInput,
                         "Custom ISO path is not a regular file: " + path.string());
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return makeError(Error::Kind::Io,
                         "Cannot read custom ISO size: " + path.string());
    }
    if (size == 0) {
        return makeError(Error::Kind::InvalidInput,
                         "Custom ISO file is empty: " + path.string());
    }
    return makeOk();
}
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
    if (!distro && plan.isoPath.empty()) {
        // Neither a known distro nor a custom ISO path: nothing to install.
        return makeError(Error::Kind::InvalidInput,
                         "Unknown distro key: " + plan.distroKey);
    }
    if (!distro) {
        log_->append(QString("Using custom ISO: %1")
                         .arg(QString::fromStdString(plan.isoPath.string())),
                     ProgressLog::Severity::Info);
    }

    // 1. Pre-flight
    emit stageChanged("Pre-flight checks");
    emit progressChanged(kProgressPreflight);
    if (auto r = backend_->preflight(plan); !r) return r;
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();

    // 2. Resolve ISO (download if needed, verify checksum)
    emit stageChanged("Resolving ISO");
    emit progressChanged(kProgressIso);
    if (distro) {
        // Catalog distro: the backend resolves (and if needed downloads and
        // checksum-verifies) the ISO and returns the resolved path.
        auto isoR = backend_->resolveIso(*distro);
        if (!isoR) return makeError(isoR.error().kind(), isoR.error().message());
        plan.isoPath = isoR.value();
    } else {
        // Custom ISO: the plan already carries the path. There is no
        // catalog checksum for it, so validate the file itself.
        if (auto r = validateCustomIso(plan.isoPath); !r) return r;
    }
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();

    // 3. Resize
    if (plan.strategy == Strategy::ShrinkAll ||
        plan.strategy == Strategy::UseFreeBoot ||
        plan.strategy == Strategy::OtherDriveShrink) {
        emit stageChanged("Resizing existing partition");
        emit progressChanged(kProgressResize);
        if (cancelFlag_.loadAcquire() != 0) return makeCancelled();
        // Re-validate before destructive operation
        if (auto r = backend_->validatePlanForDisk(plan); !r) return r;
        if (plan.shrinkDriveLetter.has_value()) {
            // The backend figures out the new size from the requested
            // shrink amount stored on the plan. shrinkPartition returns the
            // actual post-resize partition size; we record the requested
            // shrink amount as the expected freed space, which createLayout
            // validates against the real on-disk gap before mutating.
            auto r = backend_->shrinkPartition(plan.targetDiskNumber,
                                              plan.shrinkDriveLetter.value(),
                                              plan.shrinkPartitionNumber.value(),
                                              plan.shrinkAmountBytes);
            if (!r) {
                return makeError(r.error().kind(), r.error().message());
            }
            plan.actualFreedBytes = plan.shrinkAmountBytes;
        }
        if (cancelFlag_.loadAcquire() != 0) return makeCancelled();
    }

    // 4. Wipe (only for WipeDisk)
    if (plan.strategy == Strategy::WipeDisk) {
        emit stageChanged("Wiping target disk");
        emit progressChanged(kProgressWipe);
        if (cancelFlag_.loadAcquire() != 0) return makeCancelled();
        // Re-validate before destructive operation
        if (auto r = backend_->validatePlanForDisk(plan); !r) return r;
        if (auto r = backend_->wipeDisk(plan.targetDiskNumber); !r) return r;
        if (cancelFlag_.loadAcquire() != 0) return makeCancelled();
    }

    // 5. Create layout
    emit stageChanged("Creating partitions and filesystems");
    emit progressChanged(kProgressLayout);
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();
    // Re-validate before partition creation
    if (auto r = backend_->validatePlanForDisk(plan); !r) return r;
    std::filesystem::path bootMount, refindMount;
    auto cancelCallback = [this]() { return cancelFlag_.loadAcquire() != 0; };
    if (auto r = backend_->createLayout(plan, bootMount, refindMount, cancelCallback); !r) return r;
    plan.bootPartitionMount = bootMount;
    if (!refindMount.empty()) plan.refindPartitionMount = refindMount;
    // The staging partition identity and the mount root it was verified at.
    // The copy uses exactly this root.
    log_->append(QString("Staging partition on disk %1 (partition %2) mounted at %3")
                     .arg(plan.targetDiskNumber)
                     .arg(plan.stagingPartitionNumber.value_or(0))
                     .arg(QString::fromStdString(bootMount.string())),
                 ProgressLog::Severity::Info);
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();

    // 6. Mount ISO + copy
    emit stageChanged("Mounting live ISO");
    emit progressChanged(kProgressCopy);
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();
    // Re-validate before copy
    if (auto r = backend_->validatePlanForDisk(plan); !r) return r;
    auto isoMount = backend_->mountIso(plan.isoPath);
    if (!isoMount) return makeError(isoMount.error().kind(), isoMount.error().message());
    log_->append(QString("Copying the ISO contents from %1 to %2")
                     .arg(QString::fromStdString(isoMount.value().string()))
                     .arg(QString::fromStdString(bootMount.string())),
                 ProgressLog::Severity::Info);
    {
        // The backend reports the phase and the progress from the worker
        // thread. The engine forwards it to the UI, which receives the
        // engine signals through their queued connection.
        std::string lastPhase;
        auto report = [this, &lastPhase](int percent, const std::string& phase,
                                         const std::string& detail) {
            if (phase != lastPhase) {
                lastPhase = phase;
                emit stageChanged(QString::fromStdString(phase));
            }
            if (!detail.empty()) {
                log_->append(QString::fromStdString(detail),
                             ProgressLog::Severity::Info);
            }
            const int mapped = kProgressCopy +
                percent * (kProgressPatch - kProgressCopy) / 100;
            emit progressChanged(mapped);
        };
        auto copyR = backend_->copyFiles(isoMount.value(), bootMount, distro, plan,
                                         cancelCallback, report);
        // Dismount only the image this installation used.
        backend_->unmountIso(plan.isoPath);
        if (!copyR) {
            // Rollback staging partition on copy failure
            backend_->rollbackStagingPartition(plan);
            return copyR;
        }
    }
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();

    // 7. Distro-specific patches (only meaningful for a catalog distro)
    emit stageChanged("Patching distro boot configuration");
    emit progressChanged(kProgressPatch);
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();
    if (distro) {
        if (auto r = backend_->patchDistroBootConfig(*distro, plan); !r) {
            log_->append(QString("Distro config patch failed: %1")
                             .arg(r.error().qmessage()),
                         ProgressLog::Severity::Warn);
        }
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
        // Rollback staging partition on boot entry failure
        backend_->rollbackStagingPartition(plan);
        return r;  // engine.run will roll back via bcdGuid if set
    }

    // 10. Cleanup
    emit stageChanged("Cleaning up");
    emit progressChanged(kProgressCleanup);
    if (cancelFlag_.loadAcquire() != 0) return makeCancelled();

    return makeOk();
}

}  // namespace ulli::core
