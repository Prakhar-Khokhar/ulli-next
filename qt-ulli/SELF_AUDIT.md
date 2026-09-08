# ULLI-Qt Self-Audit Notes (Phase 1 + Phase 1.1 + Phase 1.2)

Date: 2026-09-08.
Scope: source under `qt-ulli/src/` and `qt-ulli/tests/`.
Tooling: g++ 15.2, Qt 6.10.2, CMake 4.4.3, Ninja. Strict flags
(`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`).

## Build Status (Phase 1.2)

```
[100%] Built target ulli-qt
4/4 tests passed (result, catalog, plan, engine)
0 warnings, 0 errors
```

UI smoke launch: `ulli-qt` under `QT_QPA_PLATFORM=offscreen` exits 0.

## Phase 1.2 Implemented Features (This Session)

### 6. Enhanced PlanDialog with Detailed Disk Visualization
- **Implementation**: Complete rewrite of `PlanDialog` showing:
  - Partition tree with columns: Partition, Type, Filesystem, Size, Offset, Flags
  - System partition highlighting (yellow background)
  - Unallocated space display
  - Strategy availability based on actual disk state
  - Real-time planned layout preview with HTML formatting
  - Safety warnings for each strategy (red for WipeDisk, orange for Shrink, green for UseFree)
- **Strategies**: ShrinkAll, UseFreeAll, WipeDisk with proper enable/disable logic
- **Explicit confirmation**: WipeDisk requires double-confirmation dialog
- **Auto-restart checkbox**: Added to Options group
- **Source**: `ui/PlanDialog.{h,cpp}`

### 7. Restart Countdown Respects `autoRestart` Flag
- **Engine**: `InstallEngine::finished()` signal now includes `autoRestart` parameter
- **MainWindow**: Only shows restart countdown if plan has `autoRestart=true`
- **PlanDialog**: Added checkbox "Restart automatically after successful installation"
- **Source**: `core/InstallEngine.{h,cpp}`, `ui/MainWindow.cpp`, `ui/PlanDialog.{h,cpp}`

### 8. ISO Download and Verification Workflow
- **Backend**: Added `downloadIso()` to `IPlatformBackend` with progress callback
- **Windows implementation**: Uses `QNetworkAccessManager` with mirror fallthrough
  - Tries each mirror in order from `distros.json`
  - Reports progress (bytes received/total, percentage)
  - Verifies SHA-256 after download using existing `Sha256::verifyFile()`
  - Cleans up on verification failure
- **UI**: Enhanced `DistroSelector` with:
  - Download button (enabled when ISO missing or unverified)
  - Progress bar with percentage and status text
  - Status label showing: missing, unverified, or verified
  - "ISO Ready" state when verified
- **Source**: `core/InstallEngine.h`, `platform/windows/DiskOps.{h,cpp}`, `ui/DistroSelector.{h,cpp}`, `ui/MainWindow.cpp`

### 9. Cancellation Handling and Thread Lifecycle Fixes
- **Engine**: Added cancel flag checks after every stage in `runStages()`
- **Engine**: `finished()` signal emitted with `Cancelled by user` on cancel
- **Thread lifecycle**: Proper cleanup in `MainWindow` destructor
- **Mock backend test**: Added `testCancellation()` verifying cancel works mid-operation
- **Source**: `core/InstallEngine.cpp`, `ui/MainWindow.cpp`, `tests/test_engine.cpp`

### 10. Mock Backend Test Suite for InstallEngine
- **New test**: `tests/test_engine.cpp` with comprehensive coverage:
  - `testSuccessfulInstall()` — full pipeline with all stages
  - `testFailurePropagation()` — failure at copy stage
  - `testCancellation()` — user cancel mid-operation (threaded)
  - `testWipeDiskStrategy()` — WipeDisk strategy stages
  - `testShrinkAllStrategy()` — ShrinkAll strategy stages
  - `testInvalidPlanRejected()` — empty plan validation
- **Mock backend**: Simulates stages with configurable delays and failures
- **Source**: `tests/test_engine.cpp`, `tests/CMakeLists.txt`

## Updated Capability Matrix (Windows Backend)

| Operation | Supported? | Mechanism | Notes |
|---|---|---|---|
| Enumerate disks/partitions | ✅ | WMI (PowerShell) | Full GPT/MBR, all kinds, unallocated |
| Wipe disk (GPT) | ✅ | diskpart | `clean; convert gpt` |
| Create FAT32 partition | ✅ | diskpart | `create partition primary; format fs=fat32; assign letter=X` |
| Create NTFS partition | ✅ | diskpart | `format fs=ntfs` |
| Shrink NTFS | ❌ | — | Returns explicit unsupported error |
| Shrink ext4/btrfs | ❌ | — | No bundled tools |
| Create ext4/btrfs | ❌ | — | No mkfs.* bundled |
| ISO SHA-256 verify | ✅ | PowerShell `Get-FileHash` | vs `Distro::sha256()` |
| **ISO download + verify** | ✅ | **QNetworkAccessManager** | **Mirror fallthrough, progress reporting** |
| BitLocker preflight | ✅ | `manage-bde` | Blocks on locked C: |
| UEFI boot entry | ✅ | `bcdedit` | GUID rollback on failure |
| rEFInd install | ❌ | — | Phase 1.3+ |
| Restart countdown | ✅ | Qt QTimer | 30s, cancelable, respects autoRestart |

## Build Status (Phase 1.2)

```
[100%] Built target ulli-qt
4/4 tests passed (result, catalog, plan, engine)
0 warnings, 0 errors
```

UI smoke launch: `ulli-qt` under `QT_QPA_PLATFORM=offscreen` exits 0.

## Files Changed / Added (Phase 1.2)

**New files**:
- `tests/test_engine.cpp` — Mock backend test suite for InstallEngine
- `src/platform/linux/DiskOps.cpp` — Added `downloadIso` stub

**Modified files**:
- `core/InstallEngine.h` — Added `downloadIso` to `IPlatformBackend`, `finished(bool, QString, bool autoRestart)`
- `core/InstallEngine.cpp` — Added cancel checks after every stage, pass `autoRestart` in finished signal
- `platform/windows/DiskOps.h` — Added `downloadIso` declaration
- `platform/windows/DiskOps.cpp` — Implemented `downloadIso` with mirror fallthrough and progress
- `platform/linux/DiskOps.h` — Added `downloadIso` declaration
- `ui/PlanDialog.{h,cpp}` — Complete rewrite with partition tree, preview, safety warnings, autoRestart checkbox
- `ui/DistroSelector.{h,cpp}` — Added download button, progress bar, status display, download callback
- `ui/MainWindow.cpp` — Wired up download callback, respects autoRestart in restart countdown
- `tests/CMakeLists.txt` — Added `engine` test
- `tests/test_engine.cpp` — Added `downloadIso` to MockBackend

## Known Limitations (Carried Forward)

| Item | Why deferred | Where |
|---|---|---|
| Linux `DiskOps` | Phase 2 | `linux/DiskOps.cpp` |
| Manual partition editor | Phase 2 | `ui/PartitionEditor.{h,cpp}` |
| rEFInd install | Phase 1.3+ | `windows/DiskOps.cpp::installRefind` |
| Shrink NTFS (real impl) | Needs `ntfsresize` bundled | `windows/DiskOps.cpp::shrinkPartition` |
| ext4/btrfs create | Needs `mkfs.*` bundled | `windows/DiskOps.cpp::createLayout` |
| AppImage / Windows .zip build | Out of scope | `ci/github-actions.yml` |
| SHA-256 test on Linux | Windows-only impl | `tests/test_sha256.cpp` (gated) |

## Thread Safety Audit (Updated)

| Shared object | Accessed from | Guarded by |
|---|---|---|
| `Catalog catalog_` | UI thread (combo); worker (read) | Immutable after `load()`. Safe. |
| `ProgressLog logBackend_` | UI (bind); worker (append) | Queued connection. Safe. |
| `QFile` / `QTextStream` in `ProgressLog` | Worker only | Single writer. Safe. |
| `InstallEngine` | Worker (run); UI (dtor, cancel) | `QAtomicInt` cancel flag. Safe. |
| `InstallPlan::bcdGuid` | `BcdStore::createLinuxEntry` / `deleteEntry` | `mutable`; single owner. Safe. |
| `RestartCountdownDialog::timer_` | UI thread only | QTimer on UI thread. Safe. |
| `DistroSelector download callback` | UI thread (signals); worker thread (download) | Qt::QueuedConnection for cross-thread signals. Safe. |

## Tests

`ctest` runs four suites (all pass):
- `result` — value semantics, `map`, `onError`, `void` specialisation.
- `catalog` — fallback catalog non-empty, key ordering, `fromDistros`.
- `plan` — validity checks (empty, minimal, too-small Linux).
- `engine` — InstallEngine state machine: success, failure, cancel, WipeDisk, ShrinkAll, invalid plan.

## Self-Audit Verdict

Phase 1.2 is **structurally complete and buildable** on Linux. The Windows
backend now includes ISO download with mirror fallthrough and progress reporting,
a significantly enhanced PlanDialog with real-time partition visualization and
safety warnings, proper restart countdown integration with autoRestart flag,
robust cancellation handling at every pipeline stage, and a comprehensive
mock-backend test suite exercising the InstallEngine state machine.

All tests pass (4/4), 0 warnings, 0 errors. UI smoke test passes.
The deferred-items table above reflects remaining Phase 2+ work.
No code-change-level safety issues were found.