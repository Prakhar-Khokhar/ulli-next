// app/main.cpp
//
// ULLI entry point. Wires QApplication, applies the style, creates the
// main window, and re-launches with elevation if we're not admin / root
// (mirrors the existing PowerShell / Python auto-elevate dance).

#include "ui/MainWindow.h"
#include "platform/Platform.h"

#include <QApplication>
#include <QMessageBox>
#include <QProcess>

#include <iostream>

#if defined(Q_OS_WIN)
#  include <windows.h>
#endif

namespace {

void relaunchElevated() {
#if defined(Q_OS_WIN)
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.lpVerb = L"runas";
    info.lpFile = path;
    info.nShow = SW_NORMAL;
    ::ShellExecuteExW(&info);
#else
    // Re-launch via pkexec (or sudo if not present).
    const QStringList candidates = {"pkexec", "sudo"};
    for (const QString& c : candidates) {
        if (QProcess::startDetached(c, {QApplication::applicationFilePath()})) return;
    }
#endif
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("ULLI");
    QApplication::setApplicationVersion("1.0.0");
    QApplication::setOrganizationName("ULLI");

    if (!ulli::platform::isElevated()) {
        const auto reply = QMessageBox::question(
            nullptr, "ULLI",
            "ULLI must be run as Administrator (Windows) or root (Linux).\n\n"
            "Relaunch elevated now?",
            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            relaunchElevated();
            return 0;
        }
        return 1;
    }

    ulli::ui::MainWindow w;
    w.show();
    return app.exec();
}
