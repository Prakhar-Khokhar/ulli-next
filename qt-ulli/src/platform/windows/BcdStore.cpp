// platform/windows/BcdStore.cpp
#include "platform/windows/BcdStore.h"

#include <QProcess>
#include <QRegularExpression>

#include <iostream>

namespace ulli::platform::windows {

namespace {

core::Result<std::string> runBcdEdit(const QStringList& args) {
    QProcess proc;
    proc.start("bcdedit.exe", args);
    if (!proc.waitForFinished(15000)) {
        return core::makeError<std::string>(core::Error::Kind::Platform,
            "bcdedit timed out");
    }
    const QString out = QString::fromLocal8Bit(proc.readAll());
    const int code = proc.exitCode();
    if (code != 0) {
        return core::makeError<std::string>(core::Error::Kind::Platform,
            "bcdedit " + args.join(' ').toStdString() +
            " failed (exit " + std::to_string(code) + "):\n" + out.toStdString());
    }
    return core::makeOk(out.toStdString());
}

std::optional<std::string> extractGuid(const QString& bcdeditOutput) {
    static const QRegularExpression rx(
        R"(\{([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})\})");
    auto m = rx.match(bcdeditOutput);
    if (m.hasMatch()) {
        return m.captured(1).toStdString();
    }
    return std::nullopt;
}

}  // namespace

core::Result<void> BcdStore::createLinuxEntry(core::InstallPlan& plan) {
    const std::string distroName = "ULLI - " + plan.distroKey;
    auto copyR = runBcdEdit({"/copy", "{bootmgr}",
                             "/d", QString::fromStdString("\"" + distroName + "\"")});
    if (!copyR) return core::makeError(copyR.error().kind(), copyR.error().message());

    auto guid = extractGuid(QString::fromStdString(copyR.value()));
    if (!guid.has_value()) {
        return core::makeError(core::Error::Kind::Platform,
            "bcdedit /copy did not return a GUID");
    }
    const QString g = QString::fromStdString("{" + guid.value() + "}");

    // Cache GUID for rollback before any further /set call.
    plan.bcdGuid = guid.value();

    // Strip inherited props (default, displayorder, etc).
    const QStringList kInherited = {
        "default", "displayorder", "toolsdisplayorder", "timeout",
        "resumeobject", "inherit", "locale"
    };
    for (const QString& prop : kInherited) {
        runBcdEdit({"/deletevalue", g, prop});  // best-effort
    }

    // Set device / path / description.
    const QString device = plan.bootMode == core::BootMode::Refind
                               ? QString::fromStdString(
                                     plan.refindPartitionMount
                                         ? plan.refindPartitionMount->string()
                                         : "C:")
                               : QString::fromStdString(
                                     plan.bootPartitionMount
                                         ? plan.bootPartitionMount->string()
                                         : "C:");
    if (auto r = runBcdEdit({"/set", g, "device", "partition=" + device}); !r)
        return core::makeError(r.error().kind(), r.error().message());
    if (auto r = runBcdEdit({"/set", g, "path", "\\EFI\\BOOT\\BOOTx64.EFI"}); !r)
        return core::makeError(r.error().kind(), r.error().message());
    if (auto r = runBcdEdit({"/set", g, "description",
                            QString::fromStdString("\"" + distroName + "\"")}); !r)
        return core::makeError(r.error().kind(), r.error().message());
    if (auto r = runBcdEdit({"/set", "{fwbootmgr}", "displayorder", g, "/addfirst"}); !r)
        return core::makeError(r.error().kind(), r.error().message());
    if (auto r = runBcdEdit({"/set", "{fwbootmgr}", "default", g}); !r)
        return core::makeError(r.error().kind(), r.error().message());

    return core::makeOk();
}

void BcdStore::deleteEntry(const core::InstallPlan& plan) {
    if (!plan.bcdGuid.has_value()) return;
    const QString g = QString::fromStdString("{" + plan.bcdGuid.value() + "}");
    QProcess::execute("bcdedit.exe", {"/delete", g});
    // Reset fwbootmgr default to the original Windows boot entry.
    // Best-effort: bcdedit doesn't fail gracefully here, so we ignore
    // the exit code.
    QProcess::execute("bcdedit.exe", {"/set", "{fwbootmgr}", "default", "{bootmgr}"});
    plan.bcdGuid.reset();
}

}  // namespace ulli::platform::windows
