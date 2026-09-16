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

std::vector<std::string> parseDisplayOrder(const QString& fwbootmgrOutput) {
    std::vector<std::string> order;
    static const QRegularExpression rx(R"(\{([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})\})");
    auto it = rx.globalMatch(fwbootmgrOutput);
    while (it.hasNext()) {
        auto match = it.next();
        order.push_back(match.captured(1).toStdString());
    }
    return order;
}

std::optional<std::string> parseDefault(const QString& fwbootmgrOutput) {
    static const QRegularExpression rx(R"(default\s+\{([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})\})");
    auto m = rx.match(fwbootmgrOutput);
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

core::Result<void> BcdStore::createDirectBootEntry(const core::InstallPlan& plan, char driveLetter) {
    const std::string distroName = "ULLI - " + plan.distroKey;
    
    // Capture original boot state for potential rollback
    auto originalState = captureBootState();
    
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
    // Since plan is const, we need to cast away const for bcdGuid (mutable)
    const_cast<core::InstallPlan&>(plan).bcdGuid = guid.value();

    // Strip inherited props (default, displayorder, etc).
    const QStringList kInherited = {
        "default", "displayorder", "toolsdisplayorder", "timeout",
        "resumeobject", "inherit", "locale"
    };
    for (const QString& prop : kInherited) {
        runBcdEdit({"/deletevalue", g, prop});  // best-effort
    }

    // Set device / path / description.
    const QString device = QString("%1:").arg(driveLetter);
    if (auto r = runBcdEdit({"/set", g, "device", "partition=" + device}); !r) {
        restoreBootState(originalState);
        return core::makeError(r.error().kind(), r.error().message());
    }
    if (auto r = runBcdEdit({"/set", g, "path", "\\EFI\\BOOT\\BOOTx64.EFI"}); !r) {
        restoreBootState(originalState);
        return core::makeError(r.error().kind(), r.error().message());
    }
    if (auto r = runBcdEdit({"/set", g, "description",
                            QString::fromStdString("\"" + distroName + "\"")}); !r) {
        restoreBootState(originalState);
        return core::makeError(r.error().kind(), r.error().message());
    }

    // Add to displayorder (addfirst) but do NOT change default unless explicitly requested
    // We add to displayorder so it appears in the boot menu, but preserve original default
    if (auto r = runBcdEdit({"/set", "{fwbootmgr}", "displayorder", g, "/addfirst"}); !r) {
        restoreBootState(originalState);
        return core::makeError(r.error().kind(), r.error().message());
    }

    // Verify the entry was created
    QProcess verifyProc;
    verifyProc.start("bcdedit.exe", {"/enum", "{fwbootmgr}"});
    if (verifyProc.waitForFinished(5000)) {
        const QString out = QString::fromLocal8Bit(verifyProc.readAll());
        if (!out.contains(g)) {
            restoreBootState(originalState);
            return core::makeError(core::Error::Kind::Platform,
                "Created boot entry not found in fwbootmgr displayorder");
        }
    }

    return core::makeOk();
}

BcdStore::BootState BcdStore::captureBootState() {
    BootState state;
    QProcess proc;
    proc.start("bcdedit.exe", {"/enum", "{fwbootmgr}"});
    if (proc.waitForFinished(5000)) {
        const QString out = QString::fromLocal8Bit(proc.readAll());
        state.originalDisplayOrder = parseDisplayOrder(out);
        auto defaultGuid = parseDefault(out);
        if (defaultGuid.has_value()) {
            state.originalDefault = defaultGuid.value();
        }
        state.hasState = true;
    }
    return state;
}

void BcdStore::restoreBootState(const BootState& state) {
    if (!state.hasState) return;

    // Restore displayorder
    if (!state.originalDisplayOrder.empty()) {
        QStringList args = {"/set", "{fwbootmgr}", "displayorder"};
        for (const auto& guid : state.originalDisplayOrder) {
            args << QString::fromStdString("{" + guid + "}");
        }
        QProcess::execute("bcdedit.exe", args);
    }

    // Restore default
    if (!state.originalDefault.empty()) {
        QProcess::execute("bcdedit.exe", {"/set", "{fwbootmgr}", "default", 
            QString::fromStdString("{" + state.originalDefault + "}")});
    }
}

void BcdStore::deleteEntry(const core::InstallPlan& plan) {
    if (!plan.bcdGuid.has_value()) return;
    const QString g = QString::fromStdString("{" + plan.bcdGuid.value() + "}");

    // Check if this entry is in fwbootmgr displayorder before deleting
    QProcess checkProc;
    checkProc.start("bcdedit.exe", {"/enum", "{fwbootmgr}"});
    if (checkProc.waitForFinished(5000)) {
        const QString out = QString::fromLocal8Bit(checkProc.readAll());
        if (out.contains(g)) {
            // This entry is in fwbootmgr - restore default to Windows bootmgr
            QProcess::execute("bcdedit.exe", {"/set", "{fwbootmgr}", "default", "{bootmgr}"});
        }
    }

    // Delete the entry
    QProcess::execute("bcdedit.exe", {"/delete", g});

    plan.bcdGuid.reset();
}

}  // namespace ulli::platform::windows