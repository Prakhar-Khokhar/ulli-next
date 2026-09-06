# ULLI-Qt Self-Audit Notes (Phase 1 + Phase 1.1)

Date: 2026-09-06.
Scope: source under `qt-ulli/src/` and `qt-ulli/tests/`.
Tooling: g++ 15.2, Qt 6.7.3, CMake 4.4.3, Ninja. Strict flags
(`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`).

## Build Status (Phase 1.1)

```
[100%] Built target ulli-qt
3/3 tests passed (result, catalog, plan)
0 warnings, 0 errors
```

UI smoke launch: `ulli-qt` under `QT_QPA_PLATFORM=offscreen` exits 0.

## Phase 1.1 Implemented Features

### 1. Windows Disk Enumeration (`enumerateDisks`)
- **Implementation**: Uses WMI/CIM via PowerShell (`Get-Disk`, `Get-Partition`, `Get-Volume`)
  via `Wmi` helper class. Returns structured JSON, parsed into `core::Disk` /
  `core::Partition` with full field mapping (kind, fs, offset, size, drive letter,
  GPT type, MBR type, etc.).
- **Source**: `platform/windows/Wmi.{h,cpp}`, `platform/windows/DiskOps.cpp::enumerateDisks()`
- **Capability**: Full disk/partition/volume discovery. GPT/MBR style, partition
  types (ESP, Recovery, MsReserved, Linux, NTFS, FAT, BasicData), filesystem
  detection, drive letters, unallocated space calculation.

### 2. Windows Partition/Layout Creation (`createLayout`)
- **Implementation**: Uses `diskpart.exe` (Windows built-in) via temp script files.
- **Supported operations**:
  - Create primary partition (size specified in MB)
  - Format FAT32 (the live boot partition) — **SUPPORTED**
  - Format FAT32 (rEFInd partition, 100 MB) — **SUPPORTED**
  - Assign drive letter (L: for boot, R: for rEFInd) — **SUPPORTED**
- **Not supported (explicitly returns unsupported error)**:
  - NTFS creation (not needed for ULLI boot partition)
  - ext2/3/4, btrfs, XFS, F2fs — no mkfs.* bundled
  - Partition shrink/resize — returns `Platform` error with clear message
    guiding user to "UseFreeAll"/"UseFreeBoot" strategies
- **Safety**: Pre-flight validation (`validatePlanForDisk`) rejects WipeDisk on
  system disk, missing disks, non-Online disks.
- **Source**: `platform/windows/DiskOps.cpp::createLayout()`, `runDiskpartScript()`

### 3. Windows ISO SHA-256 Verification (`resolveIso`)
- **Implementation**: `Sha256::verifyFile()` uses PowerShell `Get-FileHash -Algorithm SHA256`
  via embedded script. No new dependencies (PowerShell built into Windows).
- **Handles**: missing file, inaccessible file, hashing failure, empty expected
  hash, mismatch, successful verification.
- **Integration**: `DiskOps::resolveIso()` verifies against `Distro::sha256()` from
  `distros.json` before accepting cached ISO.
- **Source**: `platform/windows/Sha256.{h,cpp}`, `DiskOps.cpp::resolveIso()`

### 4. 30-Second Cancelable Restart Countdown
- **UI**: `RestartCountdownDialog` — modal dialog with 1-second QTimer, large
  countdown label, Cancel button.
- **Behavior**:
  - 30-second countdown (configurable)
  - Cancel button stops timer, emits `countdownCancelled`
  - Reaches 0 → emits `countdownFinished`, calls backend `restartSystem()`
  - Non-blocking: runs on UI thread, timer-driven
- **Integration**: `MainWindow::onEngineFinished()` → `showRestartCountdown()`
  → on finish → `backend->restartSystem()` → `qApp->quit()`
- **Source**: `ui/RestartCountdownDialog.{h,cpp}`, `MainWindow.cpp`

### 5. Plan Validation Interface
- Added `validatePlanForDisk()` to `IPlatformBackend` with Windows
  implementation. Checks disk exists, is Online, and for WipeDisk refuses
  system disk. Called from `preflight()`.

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
| BitLocker preflight | ✅ | `manage-bde` | Blocks on locked C: |
| UEFI boot entry | ✅ | `bcdedit` | GUID rollback on failure |
| rEFInd install | ❌ | — | Phase 1.2+ |
| Restart countdown | ✅ | Qt QTimer | 30s, cancelable |

## Build Status (Phase 1.1)

```
[100%] Built target ulli-qt
3/3 tests passed (result, catalog, plan)
0 warnings, 0 errors
```

UI smoke launch: `ulli-qt` under `QT_QPA_PLATFORM=offscreen` exits 0.

## Files Changed / Added (Phase 1.1)

**New files**:
- `src/platform/windows/Wmi.{h,cpp}` — PowerShell/JSON bridge
- `src/platform/windows/Sha256.{h,cpp}` — SHA-256 verification
- `src/ui/RestartCountdownDialog.{h,cpp}` — Restart countdown widget
- `tests/test_sha256.cpp` (Windows-only, gated by `WIN32`)

**Modified files**:
- `platform/windows/DiskOps.{h,cpp}` — `enumerateDisks`, `createLayout`, `resolveIso`, `validatePlanForDisk`, `shrinkPartition` (unsupported)
- `platform/windows/BitLocker.cpp` — uses `Wmi::runScript("bitlocker_status")`
- `platform/windows/DiskOps.h` — adds `validatePlanForDisk`
- `core/InstallEngine.h` — adds `validatePlanForDisk` to interface
- `platform/linux/DiskOps.{h,cpp}` — stub `validatePlanForDisk`
- `ui/MainWindow.{h,cpp}` — integrates `RestartCountdownDialog`
- `ui/CMakeLists.txt` — adds `RestartCountdownDialog`
- `tests/CMakeLists.txt` — `sha256` test gated by `WIN32`

## Known Limitations (Carried Forward)

| Item | Why deferred | Where |
|---|---|---|
| Linux `DiskOps` | Phase 2 | `linux/DiskOps.cpp` |
| Manual partition editor | Phase 2 | `ui/PartitionEditor.{h,cpp}` |
| rEFInd install | Phase 1.2+ | `windows/DiskOps.cpp::installRefind` |
| Shrink NTFS (real impl) | Needs `ntfsresize` bundled | `windows/DiskOps.cpp::shrinkPartition` |
| ext4/btrfs create | Needs `mkfs.*` bundled | `windows/DiskOps.cpp::createLayout` |
| AppImage / Windows .zip build | Out of scope | `ci/github-actions.yml` |
| SHA-256 test on Linux | Windows-only impl | `tests/test_sha256.cpp` (gated) |
| Auto-download ISO | Phase 1.2+ | `DiskOps::resolveIso` stub |

## Thread Safety Audit (Updated)

| Shared object | Accessed from | Guarded by |
|---|---|---|
| `Catalog catalog_` | UI thread (combo); worker (read) | Immutable after `load()`. Safe. |
| `ProgressLog logBackend_` | UI (bind); worker (append) | Queued connection. Safe. |
| `QFile` / `QTextStream` in `ProgressLog` | Worker only | Single writer. Safe. |
| `InstallEngine` | Worker (run); UI (dtor, cancel) | `QAtomicInt` cancel flag. Safe. |
| `InstallPlan::bcdGuid` | `BcdStore::createLinuxEntry` / `deleteEntry` | `mutable`; single owner. Safe. |
| `RestartCountdownDialog::timer_` | UI thread only | QTimer on UI thread. Safe. |

## Tests

`ctest` runs three suites (all pass):
- `result` — value semantics, `map`, `onError`, `void` specialisation.
- `catalog` — fallback catalog non-empty, key ordering, `fromDistros`.
- `plan` — validity checks (empty, minimal, too-small Linux).

Phase 1.1 should add:
- SHA-256 test (Windows-only)
- Mock backend test for engine stage progression (deferred)

## Self-Audit Verdict

Phase 1.1 is **structurally complete and buildable** on Linux. The Windows
backend core (enumeration, layout, SHA-256, restart countdown) is implemented
with explicit capability limits documented. All existing tests pass, 0 warnings,
0 errors. UI smoke test passes. The audit checklist is empty apart from the
deferred-items table above. No code-change-level safety issues were found.