# ULLI-Qt — USB-less Linux Installer (Qt 6.x C++ port)

Replacement for the legacy PowerShell (`windows/ulli-windows.ps1`) and
Python (`linux/ulli-linux.py`) installers. One C++/Qt 6.x codebase that
ships as a portable AppImage (Linux) and a .zip of raw binaries + DLLs
(Windows).

## Status

**Phase 1 — Windows parity + safety fixes.** The Windows port reaches
feature parity with the PowerShell script and adds the audit-flagged
safety improvements (BitLocker preflight, bcdedit GUID rollback, double
confirm before Clear-Disk, MBR/GPT fallthrough handling).

**Phase 2 — Linux port + manual partition editor** (planned).

## Build

### Linux (host build)

```bash
sudo apt install qt6-base-dev qt6-tools-dev cmake ninja-build g++ \
                 parted e2fsprogs btrfs-progs ntfs-3g

cd qt-ulli
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
./build/ulli-qt
```

### Windows (MSVC 2022 + Qt 6.7+)

```cmd
:: from a "x64 Native Tools Command Prompt for VS 2022"
cd qt-ulli
cmake -S . -B build -G Ninja ^
      -DCMAKE_PREFIX_PATH=C:/Qt/6.7.0/msvc2019_64 ^
      -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Release layout

`distros.json` lives next to the binary in both release artifacts:

```
windows/ulli-qt.zip
  ulli-qt.exe
  Qt6Core.dll, Qt6Gui.dll, ...     (windeployqt output)
  parted.exe, mke2fs.exe, ...     (e2fsprogs + btrfs-progs for Windows)
  distros.json

ulli-x86_64.AppImage               (Linux)
  └─ distros.json + Qt + bundled tools
```

## License

GPLv3. See `LICENSE` in the repo root.
