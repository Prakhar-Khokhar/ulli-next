// ─── Platform abstraction ────────────────────────────────────────────────────
// platform/Platform.h
//
// Single header that picks the correct platform-specific backend. All
// callers in core/ and ui/ go through this header — they never include
// platform/windows/*.h or platform/linux/*.h directly. This keeps the
// platform backend swappable and makes mocking easy in tests.

#pragma once

#if defined(_WIN32)
    #include "platform/windows/DiskOps.h"
    #include "platform/windows/BcdStore.h"
    #include "platform/windows/EspOps.h"
    #include "platform/windows/BitLocker.h"

    namespace ulli::platform {
        using DiskOps = windows::DiskOps;
        using BcdStore = windows::BcdStore;
        using EspOps = windows::EspOps;
        using BitLocker = windows::BitLocker;
    }
#elif defined(__linux__)
    #include "platform/linux/DiskOps.h"

    namespace ulli::platform {
        using DiskOps = linux::DiskOps;
    }
#else
    #error "Unsupported platform. ULLI supports Windows and Linux only."
#endif

namespace ulli::platform {
    // True if the current process is running with administrator/root
    // privileges. All disk operations require elevation; the UI must
    // gate installation on this.
    bool isElevated();

    // Path to the directory containing the running executable. Used to
    // locate distros.json (release layout).
    std::filesystem::path executableDir();

    // Path to a writable per-user directory for the install log, ISO
    // cache, and rEFInd download.
    std::filesystem::path cacheDir();

    // Bundled third-party tool paths (parted, mkfs.fat, mke2fs, etc.).
    // On Linux, these are inside the AppImage. On Windows, they sit
    // next to ulli-qt.exe in the .zip.
    std::filesystem::path toolPath(const std::string& tool);
}
