// Windows platform backend (skeleton — Phase 1 fills in the real impl).
#include "platform/Platform.h"

#include <QCoreApplication>
#include <QStandardPaths>

#include <filesystem>
#include <windows.h>

namespace ulli::platform {

bool isElevated() {
    BOOL elevated = FALSE;
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD len = 0;
    if (::GetTokenInformation(token, TokenElevation, &elevation,
                             sizeof(elevation), &len)) {
        elevated = elevation.TokenIsElevated;
    }
    ::CloseHandle(token);
    return elevated != FALSE;
}

std::filesystem::path executableDir() {
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path cacheDir() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    std::error_code ec;
    std::filesystem::create_directories(dir.toStdString(), ec);
    return dir.toStdString();
}

std::filesystem::path toolPath(const std::string& tool) {
    // On Windows, the .zip ships the tools next to ulli-qt.exe.
    return executableDir() / (tool + ".exe");
}

}  // namespace ulli::platform
