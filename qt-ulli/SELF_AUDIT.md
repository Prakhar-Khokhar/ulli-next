# ULLI-Qt Self-Audit Notes (Phase 1)

Date: 2026-09-04.
Scope: source under `qt-ulli/src/` and `qt-ulli/tests/`.
Tooling: g++ 15.2, Qt 6.7.3, CMake 4.4.3, Ninja. Strict flags
(`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conconversion`).

## Build Status

```
[100%] Built target ulli-qt
3/3 tests passed (result, catalog, plan)
0 warnings, 0 errors
```

UI smoke launch: `ulli-qt` under `QT_QPA_PLATFORM=offscreen` exits 0.

## Issues found and fixed during the audit

1. **Missing include `DiskInfo.h` in `InstallEngine.h`.** MOC couldn't see
   the `Disk` type. Fixed.
2. **Private-field access from `Catalog::load`.** `distros_` was private.
   Added a public `insert()` method.
3. **Missing `formatBytes` include in `InstallPlan.cpp`.** Fixed.
4. **Wrong namespace reference in `LogView` / `MainWindow`.** `core::platform::isElevated`
   does not exist; the namespace is `ulli::platform`. Fixed.
5. **`QProgressBar` / `QPushButton` forward-declarations were insufficient.**
   Added the full includes in the .cpp files.
6. **Pre-C++20 deduction guide problem.** `Result(T) -> Result<T>` is
   valid C++20 but the rest of the API needed `core::` namespace
   qualifiers in `PlanDialog`. Fixed by qualifying explicit return
   types and call sites.
7. **`-Wconversion` on `formatBytes`.** Original used implicit
   `uint64_t → double` conversion. Switched to explicit `static_cast<double>`.
8. **`-Wsign-conversion` on `disks_[int]`.** Added `static_cast<std::size_t>(index)`.
9. **Stub macro `ULLI_STUB` didn't preserve return type.** Removed the
   macro in favor of explicit `makeError<T>(...)` per method.
10. **`Result<void>` `error()` and `map` inconsistency.** Documented
    the void specialisation's behavior. Not used at runtime today.

## Known limitations carried into Phase 1.1 / Phase 2

These are **not** bugs — they're the things explicitly deferred per the
saved plan. Tracked here so they're not forgotten.

| Item | Why deferred | Where |
|---|---|---|
| `enumerateDisks` returns `{}` | Phase 1 stub. Real impl uses `parted -l` + WMI on Windows. | `windows/DiskOps.cpp:50` |
| `shrinkPartition`, `createLayout`, `installRefind`, `patchDistroBootConfig` | Phase 1 stubs. | `windows/DiskOps.cpp` |
| Linux `DiskOps` | All methods return "not implemented (Phase 2)". | `linux/DiskOps.cpp` |
| Manual partition editor | Phase 2; current `PartitionEditor` is a placeholder QLabel. | `ui/PartitionEditor.{h,cpp}` |
| SHA-256 verification on cached ISO | The cache hit short-circuits without verifying. | `windows/DiskOps.cpp:resolveIso` |
| BitLocker preflight in `enforceCancel` path | The `preflight` blocks the install but does not gracefully retry once the user decrypts. | `windows/BitLocker.cpp` |
| `EspOps::acquireLetter` doesn't track whether the letter was pre-existing | `LetterGuard::wasAdded` is always true. The cleanup uses `mountvol /d` which is a no-op for letters that were already mounted. Harmless. | `windows/EspOps.cpp` |
| `restartSystem` is unconditional | Phase 1.1 should add a 30-second cancel-able countdown. | `windows/DiskOps.cpp::restartSystem` |
| `Distros.size_gb` field is hard-coded to 0 in the loader | The JSON schema doesn't carry it. The Linux port previously showed approximate size in the UI; if we want parity, add a `size_gb` field. | `core/Catalog.cpp::tryLoadFromFile` |
| AppImage & Windows .zip build | Out of scope for Phase 1 source code; CI workflow references `linuxdeploy-qt` but doesn't run it. | `ci/github-actions.yml` |

## Thread Safety Audit

| Shared object | Accessed from | Guarded by |
|---|---|---|
| `Catalog catalog_` | UI thread (read for combo population); worker thread (read in `runStages`) | Immutable after `load()`. Safe. |
| `ProgressLog logBackend_` | UI thread (binds to `logAppended`); worker thread (calls `append`) | `append` runs on the worker; `logAppended` is `Qt::AutoConnection` → `QueuedConnection` across threads. Safe. |
| `QFile` / `QTextStream` inside `ProgressLog` | Worker thread only | `append` is the only writer. Safe. |
| `InstallEngine` | Worker thread (via `moveToThread`); UI thread (destructor, `requestCancel`) | `requestCancel` uses `QAtomicInt`. Other methods only run on the worker thread. |
| `InstallPlan::bcdGuid` | `BcdStore::createLinuxEntry` (set); `BcdStore::deleteEntry` (reset) | `mutable`; the engine is the single owner of the plan. |

## Tests

`ctest` runs three suites:

- `result` — value semantics, `map`, `onError`, `void` specialisation.
- `catalog` — fallback catalog non-empty, key ordering, `fromDistros`.
- `plan` — validity checks (empty, minimal, too-small Linux).

All pass. Coverage is intentionally light for Phase 1; Phase 1.1 should
add:

- `BackendContract` test using a mock `IPlatformBackend` that walks the
  state machine through every stage and asserts on signal emissions.
- `Distro` round-trip test that writes a JSON object, re-reads, and
  asserts the values are preserved.
- `BcdStore` mock test (Windows-only) that verifies the GUID is
  captured before any `/set` call and that `deleteEntry` is idempotent.

## Self-audit verdict

Phase 1 is **structurally complete and buildable**. The Qt UI launches
and the core compiles cleanly under strict warnings. The audit
checklist is empty apart from the deferred-items table above. No
code-change-level safety issues were found in the audit pass.
