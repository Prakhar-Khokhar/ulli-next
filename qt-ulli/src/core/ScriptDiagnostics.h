// core/ScriptDiagnostics.h
//
// Diagnostics for the helper scripts (PowerShell, diskpart) that the
// platform backend runs.
//
// A failing script can report its problem on either stream: the scripts
// in DiskOps::createLayout() write a JSON error object to standard output
// and exit with 1, while the errors of PowerShell itself go to standard
// error. Reporting only one of the streams loses the useful part of the
// message, so both are collected here.
//
// The helper is platform independent on purpose: the formatting of a
// script failure can be tested without a Windows host.

#pragma once

#include <QString>

#include <string>

namespace ulli::core {

// Joins the output of a failed script process into a single diagnostic
// fragment.
//
// A stream that carries no text is left out, so the message never shows
// an empty label, and every stream appears exactly once.
inline std::string joinScriptOutput(const QString& standardError,
                                    const QString& standardOutput) {
    const QString err = standardError.trimmed();
    const QString out = standardOutput.trimmed();
    if (err.isEmpty() && out.isEmpty()) {
        return "(no output)";
    }
    if (err.isEmpty()) {
        return "stdout: " + out.toStdString();
    }
    if (out.isEmpty()) {
        return "stderr: " + err.toStdString();
    }
    return "stderr: " + err.toStdString() + " | stdout: " + out.toStdString();
}

}  // namespace ulli::core
