# ULLI → Qt 6.x Port Plan (C++)

This plan supersedes the earlier Windows-only architecture proposal. It describes
the full rewrite of the ULLI installer in **C++ + Qt 6.x**, first for Windows,
then for Linux, with the legacy PowerShell and Python implementations removed
after the new port reaches feature parity + audit-flagged safety fixes.

## Goals

1. Replace the PowerShell and Python front-ends with a single Qt 6.x C++ codebase
   that ships as a portable AppImage (Linux) and a .zip of raw binaries + DLLs
   (Windows).
2. Improve the partition/filesystem story: bundle `parted`, `e2fsprogs`, and
   `btrfs-progs` so the install can resize ext2/3/4, btrfs, NTFS (via
   `ntfsresize`) and create FAT32 / NTFS / ext4 / btrfs from one toolchain —
   not just the current "shrink the only NTFS volume we see, create one FAT32
   partition" flow.
3. Reproduce the audit-flagged safety fixes (BitLocker preflight, bcdedit GUID
   rollback on partial failure, double-confirm before `Clear-Disk`, MBR/GPT
   fallthrough handling).
4. Add manual partition editing (advanced) in a later phase.
5. Drop the legacy PowerShell and Python installers once the C++ port is the
   only supported way to run ULLI.

## Target Stack

| Layer        | Choice                                       |
| ------------ | -------------------------------------------- |
| Language     | C++20                                        |
| UI           | Qt 6.x (`QtCore`, `QtWidgets`, `QtNetwork`)  |
| Build        | CMake 3.21+                                  |
| Packaging    | linuxdeploy-qt → AppImage ; windeployqt + zip |
| Test         | Qt Test ; CI on Linux + Windows runners      |

AppImage and Windows .zip both ship `distros.json` next to the binary. The
loader falls back to a built-in catalog (with a non-fatal warning) when the
file is missing, exactly as the PowerShell and Python loaders do today.

## Release Layout

```
windows/                       ← .zip release
  ulli-qt.exe
  Qt6Core.dll, Qt6Gui.dll, …   (windeployqt output)
  parted.exe, mke2fs.exe, …   (e2fsprogs + btrfs-progs for Windows)
  distros.json

ulli-x86_64.AppImage           ← Linux release (self-extracting)
  ├─ usr/bin/ulli-qt           (the binary)
  ├─ usr/lib/...               (Qt + bundled deps)
  ├─ usr/bin/parted, mke2fs, btrfs, mkfs.fat, mkfs.btrfs, ntfs-3g
  └─ distros.json
```

## Three Bundled Tools (third-party partitioning & filesystem utilities)

`parted`, `e2fsprogs` (resize2fs, mke2fs, e2fsck), and `btrfs-progs`
(btrfs resize, mkfs.btrfs, btrfs-convert) are MIT/GPL/LGPL utilities that
are present on nearly every Linux distro and have official upstream Windows
builds. `ntfs-3g` ships on every Linux distro and has a community Windows
port (`ntfs-3g-winbuild` or Tuxera's). We do not fork any of them; we
shell out to them via `QProcess` with a wrapped `QStringList` argument
list (no shell interpolation).

Linux: bundled inside the AppImage. The Qt app finds them via
`QStandardPaths::AppDataLocation + "/bin/<tool>"`.

Windows: shipped inside the .zip next to the .exe. `windeployqt` adds Qt
DLLs; the rest are placed by the build's `cmake --install` step into the
output directory.

The QProcess wrappers are placed in `core/DiskOps.{h,cpp}` so the UI layer
never calls `QProcess::start` directly. Each wrapper returns a typed
`Result<T, Error>` struct so callers must handle failures — a deliberate
departure from the PowerShell script's "log and return" pattern.

## Module Layout

```
qt-ulli/
  CMakeLists.txt
  src/
    main.cpp
    app/                     # QApplication wiring, single-instance
    ui/                      # Qt Designer .ui files (or hand-built QWidget)
      MainWindow.{h,cpp}
      PlanDialog.{h,cpp}
      PartitionEditor.{h,cpp}    # advanced manual editor (phase 2)
      DistroSelector.{h,cpp}
    core/                    # platform-agnostic installer logic
      Catalog.{h,cpp}        # loads distros.json
      Distro.{h,cpp}
      InstallPlan.{h,cpp}
      InstallEngine.{h,cpp}  # orchestrates the install, emits progress
      Result.{h,cpp}         # Result<T,E> sum type
    platform/
      windows/               # Windows-specific backend
        DiskOps.{h,cpp}      # parted, bcdedit, robocopy wrappers
        BcdStore.{h,cpp}     # GUID creation + rollback
        EspOps.{h,cpp}       # find & write to Windows ESP
        BitLocker.{h,cpp}    # preflight check
      linux/                 # Linux-specific backend (phase 2)
        DiskOps.{h,cpp}      # parted, mkfs.*, mount wrappers
        GrubOps.{h,cpp}      # grub-install, efibootmgr
    qml/                     # empty in phase 1; reserved for QML later
  third_party/
    distros.json
    licenses/                # GPL, MIT, LGPL notices for bundled tools
  tests/
    test_catalog.cpp
    test_result.cpp
    test_installplan.cpp
  ci/
    github-actions.yml
```

## distros.json Resolution Order

Loaded by `core::Catalog::load()`:

1. `<exe-dir>/distros.json`  (release layout — primary)
2. `<exe-parent>/distros.json` (dev layout — one level up, in case the
   binary is in a `build/` subdirectory)
3. Built-in fallback (the in-binary copy the PowerShell/Python scripts
   also have). On fallback, the UI shows a non-blocking banner:
   `"distros.json not found. Using built-in catalog — some mirrors may
   be dead and newer distros are missing."`

Schema stays the same as today. We will add `linux_*` fields later
(`linux_live_path`, `linux_hybrid`) when the Linux port lands.

## Phase Plan

### Phase 1 — Windows Qt port, parity + safety (v1.0)

Scope:
- One C++/Qt 6.x codebase.
- `windows/ulli-qt.exe` shipped as a .zip with `windeployqt` + `parted` +
  e2fsprogs + btrfs-progs (Windows builds) + `distros.json`.
- Feature parity with the current `ulli-windows.ps1`:
  - distro selection + custom ISO
  - disk detection + partition discovery (uses `parted` instead of `Get-Disk`)
  - the six current strategies (shrink_all, use_free_all, use_free_boot,
    other_drive, other_drive_shrink, wipe_disk)
  - ISO download, mount, robocopy-equivalent (`xcopy` or `robocopy`)
  - rEFInd partition + install
  - UEFI boot entry creation
- Audit-flagged safety fixes:
  - **BitLocker preflight** — refuse (or strongly warn) if C: is
    BitLocker-encrypted before any resize/clear.
  - **bcdedit GUID rollback** — if `bcdedit /copy` succeeds but later
    `/set` calls fail, the new GUID is deleted in a `finally`.
  - **Clear-Disk double-confirm** — type-the-disk-number confirm.
  - **MBR/GPT fallthrough** — if ESP not found on MBR, refuse rather
    than silently copy bootloader nowhere.
- UI: QMainWindow, QStackedWidget, log QTextEdit, Copy Log / Open Folder
  buttons (mirrors the just-added PowerShell file sink). Run the install
  on a background `QThread` with cancellation.
- A real installer pipeline that can be unit-tested via mocked
  `DiskOps` and `BcdStore`.

Deliverables:
- `qt-ulli/` source tree, building on both Linux (host) and Windows
  (cross via `mingw` or MSVC).
- `windows/ulli-qt-1.0.zip` with everything in Release/.
- README updates describing the new install path and dropping the
  `windows/ulli-windows.ps1` mention.

### Phase 2 — Linux Qt port + manual partition editor (v1.1)

Scope:
- Implement `platform/linux/`:
  - `DiskOps` wrapping `parted`, `mkfs.fat`, `mke2fs`, `mkfs.btrfs`,
    `mount`, `umount`, `losetup`.
  - `GrubOps` wrapping `grub-install`, `efibootmgr`, optional `refind-install`.
- The new `core/InstallEngine` is platform-agnostic; Linux just plugs in.
- Manual partition editor: `ui/PartitionEditor.{h,cpp}`.
  - QTreeView + QTableView of `parted -l` output.
  - Drag handles to resize. `parted` does the actual move.
  - FS-type column with a QComboBox.
  - Mount-point column.
  - Live preview of free space and "what the final layout will look
    like".
  - Operations: create, delete, resize, change FS, set flags (boot, esp,
    lvm, raid).
- AppImage built with `linuxdeploy-qt`. The `third_party/` binaries
  (parted, e2fsprogs, btrfs-progs, ntfs-3g) are embedded.
- Same `distros.json` lookup rules.

Deliverables:
- `ulli-1.1-x86_64.AppImage`.
- First-class Linux install path; the existing `linux/ulli-linux.py`
  is left in the repo for now, marked "deprecated — will be removed
  in v1.3".

### Phase 3 — Remove legacy (v1.3)

Scope:
- Delete `windows/ulli-windows.ps1`, `windows/run-ulli-windows.bat`,
  `linux/ulli-linux.py`.
- Update `README.md` and `AGENTS.md` to describe only the Qt port.
- `distros.json` stays at the repo root for source-tree convenience, but
  the build copies it next to the binary for the .zip / AppImage.

Deliverables:
- `windows/ulli-qt-1.3.zip` and `ulli-1.3-x86_64.AppImage` only.
- Repository contains `qt-ulli/` + `distros.json` + docs.

## Data Flow (Phase 1, Windows)

```
[user picks distro + disk + strategy]
            │
            ▼
   core::InstallPlan    (immutable, value-typed)
            │
            ▼
   core::InstallEngine::run(plan)
            │
   ┌────────┼────────────────────────────┐
   ▼        ▼                            ▼
  [validate] [check BitLocker on C: if shrinking]   [if wipe: double-confirm]
   │        │
   ▼        ▼
   ┌──────────────────────────────────────────┐
   │   platform::windows::DiskOps             │
   │   - parted -s <disk> mklabel gpt         │
   │   - parted -s <disk> mkpart ...          │
   │   - mke2fs -t ext4 / mkfs.fat / mkfs.btrfs│
   │   - bcdedit /copy "{bootmgr}" /d ...     │
   │   - bcdedit /set {guid} ...              │
   │   - (in finally: bcdedit /delete {guid}) │
   └──────────────────────────────────────────┘
            │
            ▼
   progress signals → ui::MainWindow (QTextEdit + QProgressBar)
            │
            ▼
   on success: post-install dialog (distro link, rEFInd option, restart)
   on error:  show partial state, log path, "Open log folder" button
```

## Failure Modes and Mitigations

| Failure                       | Today (PowerShell)              | Qt port (phase 1)                                 |
| ----------------------------- | ------------------------------- | -------------------------------------------------- |
| bcdedit partial failure       | orphan GUID in BCD              | GUID deleted in `finally`                          |
| BitLocker on C:               | silent resize attempt          | preflight, refuse with explanation                |
| Clear-Disk on wrong disk      | single OKCancel                 | type-the-disk-number confirm                      |
| Cancel during file copy       | no cancel button                | QThread with cancellation token                   |
| distros.json missing          | silent fallback                | non-blocking banner in UI                         |
| Power loss mid-install        | half-shunk C:, no boot entry   | persistent log + restart-aware resume (phase 2)   |
| Resize NTFS with open handles | diskpart fails, logged only    | `ntfsresize -i` precheck, refuse if bad           |

## Risks

- **`parted` on Windows** is real but quirky. The build will pin to a
  specific GNU Parted for Windows version and freeze the bundle in CI.
  If `parted` chokes on a layout that `diskpart` handled, we have a
  fallback path to call `diskpart` via `QProcess` for the operations
  parted cannot do. This is tracked as a known gap in v1.0.
- **Windows ESP manipulation** still requires admin and the same UAC
  dance. The .zip is portable but the executable must be run elevated
  (same as today).
- **C++/Qt is a larger change** than PySide6 would have been. Mitigation:
  the v1.0 scope is parity + safety only; the new features live in v1.1.
- **AppImage size** grows to ~80-120 MB (Qt + parted + e2fsprogs +
  btrfs-progs). Acceptable for a one-time download of a disk-management
  tool.

## Validation Plan

Per-phase:
- CI on Linux (Ubuntu 24.04) and Windows (GitHub Actions runners).
- PSScriptAnalyzer no longer applies — replaced by `clang-tidy` + Qt Test.
- Smoke test: launch the binary on a clean Win11 VM, verify UI renders,
  verify the plan dialog populates from `distros.json`, verify the
  "missing distros.json" banner appears when the file is renamed away.
- Real install test: a known-good Win11 VM with a 100 GB secondary disk.
  Wipe strategy end-to-end. Confirm the system still boots.
- Failure-mode test: a VM with BitLocker on C: — confirm the preflight
  blocks shrink.

## Open Questions (deferred to v1.1 or later)

- QML vs Widgets for the manual partition editor.
- Whether to also ship a `linux/flatpak/Flatpak` package once the
  AppImage is stable.
- Auto-update mechanism (currently none — user re-downloads).
- Localized UI strings.
- Signed binaries (Authenticode on Windows, GPG on the AppImage) —
  separate concern from the port.

## Out of Scope (v1)

- Migrating the existing `windows/ulli-windows.ps1` and
  `linux/ulli-linux.py` to PySide6 as a stepping stone. The C++ rewrite
  is the only path forward; the legacy files stay untouched in v1.0
  and v1.1, then are deleted in v1.3.
- QML-based UI. Widgets first; QML is a possible v2 migration.
- A non-portable Windows installer (Inno Setup). The .zip of raw
  binaries + DLLs is the v1 deliverable per your choice.
- Changing the distros.json schema. The shared snake_case schema stays;
  Linux adds `linux_*` fields in v1.1.
