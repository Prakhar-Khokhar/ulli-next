// Linux platform backend.
#include "platform/Platform.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QtGlobal>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace ulli::platform {

bool isElevated() {
    return ::geteuid() == 0;
}

std::filesystem::path executableDir() {
    // QCoreApplication isn't created yet at very early startup; fall back
    // to /proc/self/exe.
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return p.parent_path();
    }
    if (QCoreApplication::instance()) {
        return std::filesystem::path(
            QCoreApplication::applicationDirPath().toStdString());
    }
    return std::filesystem::current_path();
}

std::filesystem::path cacheDir() {
    QString dir;
    if (QCoreApplication::instance()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    } else {
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        std::string base = xdg && *xdg
            ? std::string(xdg)
            : (std::string(std::getenv("HOME")) + "/.cache");
        dir = QString::fromStdString(base + "/ulli-qt");
    }
    std::error_code ec;
    std::filesystem::create_directories(dir.toStdString(), ec);
    return dir.toStdString();
}

std::filesystem::path toolPath(const std::string& tool) {
    // On Linux, the AppImage ships the tools in usr/bin/ next to the
    // binary. For dev runs, fall back to PATH lookup.
    auto candidate = executableDir() / "usr" / "bin" / tool;
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) {
        return candidate;
    }
    return tool;  // let QProcess find it via PATH
}

}  // namespace ulli::platform
