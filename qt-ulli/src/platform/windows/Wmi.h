// platform/windows/Wmi.h
//
// Tiny PowerShell/CIM bridge. Runs a fixed script in a temp .ps1 file,
// captures the JSON output, parses it, and returns a structured result.
// All disk enumeration on Windows goes through this — the platform
// layer never parses ad-hoc CLI output, only well-formed JSON returned
// by a known script.
//
// The scripts are kept here as raw string literals so they are version-
// controlled alongside the C++ that calls them.

#pragma once

#include "core/Result.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <filesystem>
#include <string>

namespace ulli::platform::windows {

class Wmi {
public:
    // Run a named script (from the embed table) and parse the JSON
    // it writes to stdout. Returns the parsed document or a Result
    // error if PowerShell is missing, the script fails, or the output
    // is not valid JSON.
    static core::Result<QJsonDocument> runScript(const char* scriptName);

    // Convenience: returns the embedded script body as a string so the
    // caller can write it to a temp .ps1 file before invoking
    // powershell.exe.
    static std::string scriptBody(const char* scriptName);

private:
    static std::filesystem::path writeTempScript(const std::string& body);
};

}  // namespace ulli::platform::windows
