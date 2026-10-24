# Mock-based, non-destructive unit tests for UEFI boot-readiness logic in windows/ulli-windows.ps1.
# Every destructive/firmware command (bcdedit, Restart-Computer) is replaced by a test double, so no
# real disk, firmware entry, or restart is performed. Safe to run on any PowerShell host.
#
# Usage: pwsh -NoProfile -File windows/ulli-windows.boot-readiness-tests.ps1
#
# Tests exercise the PRODUCTION boot-configuration helpers (extracted from ulli-windows.ps1), not a
# separate simulation of the logic:
#   1. New-UefiBootEntry: success, /copy failure, unparseable /copy output (orphan uncertainty),
#      device/path/set failure, firmware-default failure, default verification failure.
#   2. Test-UelliFirmwareDefault: default matches / does not match.
#   3. Test-UelliEntryDevice / Select-UelliExistingBootEntry: exact match, ambiguous (multiple),
#      no match, device mismatch -> never selects a broad substring match.
#   4. Configure-UelliBootEntry: RefindPlus path, RefindPlus->standard fallback, existing entry,
#      new entry, total failure; readiness is reset each call (no leak).
#   5. Restart eligibility: only when auto-restart AND readiness are both true.
#   6. Conditional status text: complete vs boot-configuration-incomplete.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if (-not $env:TEMP) { $env:TEMP = '/tmp' }

$script:testsPassed = 0
$script:testsFailed = 0
$script:failures = New-Object System.Collections.Generic.List[object]
$script:mockLogEntries = New-Object System.Collections.Generic.List[object]
$script:mockStatuses = New-Object System.Collections.Generic.List[string]
$script:mockRestartCalled = $false

# ---------- Test doubles ----------
function Write-LogMessage {
    [CmdletBinding()]
    param([string]$Message, [switch]$IsError)
    $script:mockLogEntries.Add([PSCustomObject]@{ Message = $Message; IsError = [bool]$IsError })
}

function Get-LogMessage {
    [CmdletBinding()]
    param([switch]$IsError)
    $script:mockLogEntries | Where-Object { $_.IsError -eq [bool]$IsError } | ForEach-Object { $_.Message }
}

function Set-Status {
    [CmdletBinding()]
    param([string]$Status)
    $script:mockStatuses.Add($Status)
}

function Restart-Computer {
    [CmdletBinding()]
    param([switch]$Force)
    $script:mockRestartCalled = $true
}

# ---------- Fake bcdedit.exe (configurable via environment variables) ----------
# UB_COPY_OUTPUT -> full /copy "{bootmgr}" response line (realistic: explanatory
# text plus the new entry GUID on ONE line, e.g.
#   "The entry was successfully copied to {11111111-1111-1111-1111-111111111111}.")
# UB_COPY_EXIT   -> /copy exit code
# UB_SET_EXIT / UB_DEFAULT_SET_EXIT / UB_ORDER_EXIT -> /set /displayorder exit codes
# UB_ENUM_DEFAULT                                 -> default GUID in /enum {fwbootmgr}
function New-FakeBcdEdit {
    param([string]$Dir)
    if (-not (Test-Path $Dir)) { New-Item -ItemType Directory -Path $Dir | Out-Null }
    $fakeExe = Join-Path $Dir "bcdedit.exe"
    Set-Content -Path $fakeExe -Value @'
#!/bin/bash
if [ "$1" = "/copy" ]; then
    printf '%s\n' "${UB_COPY_OUTPUT:-The entry was successfully copied to {11111111-1111-1111-1111-111111111111}.}"
    exit "${UB_COPY_EXIT:-0}"
elif [ "$1" = "/set" ]; then
    if [ "$2" = "{fwbootmgr}" ] && [ "$3" = "default" ]; then
        exit "${UB_DEFAULT_SET_EXIT:-0}"
    fi
    exit "${UB_SET_EXIT:-0}"
elif [ "$1" = "/deletevalue" ]; then
    exit 0
elif [ "$1" = "/displayorder" ]; then
    exit "${UB_ORDER_EXIT:-0}"
elif [ "$1" = "/delete" ]; then
    exit 0
elif [ "$1" = "/enum" ] && [ "$2" = "{fwbootmgr}" ]; then
    printf 'Windows Boot Manager\r\n--------------------\r\nidentifier              {fwbootmgr}\r\ndefault                 %s\r\n' "${UB_ENUM_DEFAULT:-\{11111111-1111-1111-1111-111111111111\}}"
    exit 0
fi
exit 0
'@ -Encoding UTF8
    chmod 755 $fakeExe
    return $fakeExe
}

# Fake bare `bcdedit` (no .exe) used to simulate `bcdedit /enum firmware` output.
# Get-FirmwareEntriesFromBcdedit invokes `bcdedit` directly (not via Start-Process),
# so a bare executable named `bcdedit` must be discoverable on PATH.
function New-FakeBcdEditFirmware {
    param([string]$Dir)
    if (-not (Test-Path $Dir)) { New-Item -ItemType Directory -Path $Dir | Out-Null }
    $fakeExe = Join-Path $Dir "bcdedit"
    Set-Content -Path $fakeExe -Value @'
#!/bin/bash
if [ "$1" = "/enum" ] && [ "$2" = "firmware" ]; then
    printf '%s\n' "${UB_FIRMWARE_OUTPUT:-}"
    exit "${UB_FIRMWARE_EXIT:-0}"
fi
exit 0
'@ -Encoding UTF8
    chmod 755 $fakeExe
    return $fakeExe
}

$script:fakeBcdDir = Join-Path $env:TEMP "ulli_boot_readiness_bcdedit"
$script:fakeBcdExe = New-FakeBcdEdit -Dir $script:fakeBcdDir
$script:fakeBcdFirmware = New-FakeBcdEditFirmware -Dir $script:fakeBcdDir
$script:oldPath = $env:PATH
$sep = [System.IO.Path]::PathSeparator
$env:PATH = "$script:fakeBcdDir$sep$env:PATH"

function Reset-BcdEditEnv {
    $env:UB_COPY_EXIT = '0'
    $env:UB_COPY_OUTPUT = 'The entry was successfully copied to {11111111-1111-1111-1111-111111111111}.'
    $env:UB_SET_EXIT = '0'
    $env:UB_DEFAULT_SET_EXIT = '0'
    $env:UB_ORDER_EXIT = '0'
    $env:UB_ENUM_DEFAULT = '{11111111-1111-1111-1111-111111111111}'
    $env:UB_FIRMWARE_EXIT = '0'
    $env:UB_FIRMWARE_OUTPUT = @'
Windows Boot Manager
--------------------
identifier {fwbootmgr}
description Windows Boot Manager
device partition=\Device\HarddiskVolume1
path \EFI\Microsoft\Boot\bootmgfw.efi

Firmware Application (101fffff)
----------------------------------------------------------------------
identifier {11111111-1111-1111-1111-111111111111}
description Linux Mint 22.3
device partition=C:
path \EFI\BOOT\BOOTx64.EFI

Firmware Application (101fffff)
----------------------------------------------------------------------
identifier {22222222-2222-2222-2222-222222222222}
description Ubuntu 26.04
device partition=\Device\HarddiskVolume3
path \EFI\ubuntu\shimx64.efi
'@
}

# ---------- Load the production helpers from the source file ----------
$sourcePath = Join-Path $PSScriptRoot "ulli-windows.ps1"
$source = Get-Content -Path $sourcePath -Raw

function Extract-FunctionBody {
    param([string]$Source, [string]$Marker)
    $idx = $Source.IndexOf($Marker)
    if ($idx -lt 0) { throw "Could not find '$Marker' in $sourcePath" }
    $i = $idx + $Marker.Length
    $depth = 1
    $sb = [System.Text.StringBuilder]::new()
    [void]$sb.Append($marker)
    $len = $Source.Length
    while ($i -lt $len) {
        $c = $Source[$i]
        [void]$sb.Append($c)
        if ($c -eq '{') { $depth++ }
        elseif ($c -eq '}') { $depth-- }
        $i++
        if ($depth -eq 0) { break }
    }
    if ($depth -ne 0) { throw "Brace mismatch extracting '$Marker'" }
    return $sb.ToString()
}

$functionMarkers = @(
    'function New-UefiBootEntry {',
    'function Test-UelliFirmwareDefault {',
    'function Test-UelliEntryDevice {',
    'function Select-UelliExistingBootEntry {',
    'function Get-FirmwareEntriesFromBcdedit {',
    'function Set-FirmwareDefault {',
    'function Configure-UelliBootEntry {',
    'function Get-UelliBootStatusText {',
    'function Test-UelliBootRestartEligible {',
    'function Start-Installation {'
)

foreach ($marker in $functionMarkers) {
    $body = Extract-FunctionBody -Source $source -Marker $marker
    . ([scriptblock]::Create($body))
}

# ---------- Test harness ----------
function Reset-Mocks {
    $script:mockLogEntries.Clear()
    $script:mockStatuses.Clear()
    $script:mockRestartCalled = $false
    $script:uefiBootReady = $false
    Reset-BcdEditEnv
}

function Assert-True {
    [CmdletBinding()]
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        $script:failures.Add($Message)
        throw $Message
    }
}

function Assert-False {
    [CmdletBinding()]
    param([bool]$Condition, [string]$Message)
    if ($Condition) {
        $script:failures.Add($Message)
        throw $Message
    }
}

function Assert-LogContains {
    [CmdletBinding()]
    param([string]$Pattern, [switch]$IsError, [string]$Message = "Log should contain '$Pattern'")
    $matches = Get-LogMessage -IsError:$IsError | Where-Object { $_ -like "*$Pattern*" }
    Assert-True ($matches.Count -gt 0) $Message
}

function Assert-LogNotContains {
    [CmdletBinding()]
    param([string]$Pattern, [switch]$IsError, [string]$Message = "Log should NOT contain '$Pattern'")
    $matches = Get-LogMessage -IsError:$IsError | Where-Object { $_ -like "*$Pattern*" }
    Assert-True ($matches.Count -eq 0) $Message
}

function Run-Test {
    [CmdletBinding()]
    param([string]$Name, [scriptblock]$Body)
    try {
        & $Body
        $script:testsPassed++
        Write-Host "PASS: $Name"
    } catch {
        $script:testsFailed++
        $script:failures.Add([PSCustomObject]@{ Name = $Name; Error = $_ })
        Write-Host "FAIL: $Name : $_"
    }
}

# ---------- 1. New-UefiBootEntry: success (production code with fake bcdedit) ----------
Run-Test "New-UefiBootEntry: bcdedit success returns the new GUID" {
    Reset-Mocks
    $env:UB_COPY_OUTPUT = 'The entry was successfully copied to {22222222-2222-2222-2222-222222222222}.'
    $env:UB_ENUM_DEFAULT = '{22222222-2222-2222-2222-222222222222}'
    $guid = New-UefiBootEntry -DistroName "TestDistro" -DevicePartition "C:" -EfiPath "\EFI\BOOT\BOOTx64.EFI"
    Assert-True ($guid -eq '{22222222-2222-2222-2222-222222222222}') "expected GUID, got $guid"
    Assert-LogContains "set as the firmware default"
}

# ── 2. /copy failure ─────────────────────────────────────────────────────────
Run-Test "New-UefiBootEntry: bcdedit /copy failure returns null" {
    Reset-Mocks
    $env:UB_COPY_EXIT = '1'
    $guid = New-UefiBootEntry -DistroName "TestDistro" -DevicePartition "C:" -EfiPath "\EFI\BOOT\BOOTx64.EFI"
    Assert-True ($null -eq $guid) "expected null, got $guid"
    Assert-LogContains "bcdedit /copy failed" -IsError
}

# ── 3. /copy succeeds but output is unparseable (orphan uncertainty) ─────────
Run-Test "New-UefiBootEntry: unparseable /copy output logs uncertainty, returns null" {
    Reset-Mocks
    $env:UB_COPY_OUTPUT = 'The operation completed successfully.'
    $guid = New-UefiBootEntry -DistroName "TestDistro" -DevicePartition "C:" -EfiPath "\EFI\BOOT\BOOTx64.EFI"
    Assert-True ($null -eq $guid) "expected null on unparseable output, got $guid"
    Assert-LogContains "no entry GUID could be parsed" -IsError
    Assert-LogContains "will not be deleted or modified" -IsError
}

# ── 3b. /copy output with TWO GUIDs is ambiguous -> null, never delete ───────
Run-Test "New-UefiBootEntry: ambiguous /copy output (two GUIDs) returns null" {
    Reset-Mocks
    $env:UB_COPY_OUTPUT = 'The entry was successfully copied to {11111111-1111-1111-1111-111111111111} and {22222222-2222-2222-2222-222222222222}.'
    $guid = New-UefiBootEntry -DistroName "TestDistro" -DevicePartition "C:" -EfiPath "\EFI\BOOT\BOOTx64.EFI"
    Assert-True ($null -eq $guid) "expected null on ambiguous output, got $guid"
    Assert-LogContains "did not contain exactly one entry GUID" -IsError
    # No cleanup log for an entry we cannot claim as our own.
    Assert-LogNotContains "Created new entry"
}

# ── 3c. /copy output with a malformed identifier -> null ─────────────────────
Run-Test "New-UefiBootEntry: malformed /copy identifier returns null" {
    Reset-Mocks
    $env:UB_COPY_OUTPUT = 'The entry was successfully copied to not-a-guid-at-all.'
    $guid = New-UefiBootEntry -DistroName "TestDistro" -DevicePartition "C:" -EfiPath "\EFI\BOOT\BOOTx64.EFI"
    Assert-True ($null -eq $guid) "expected null on malformed output, got $guid"
    Assert-LogContains "no entry GUID could be parsed" -IsError
}

# ── 4. device/set/path failure -> entry cleaned up, null returned ────────────
Run-Test "New-UefiBootEntry: device set failure cleans up and returns null" {
    Reset-Mocks
    $env:UB_SET_EXIT = '1'
    $guid = New-UefiBootEntry -DistroName "TestDistro" -DevicePartition "C:" -EfiPath "\EFI\BOOT\BOOTx64.EFI"
    Assert-True ($null -eq $guid) "expected null, got $guid"
    Assert-LogContains "Failed to set device" -IsError
}

# ── 5. firmware default set failure -> entry cleaned up, null returned ───────
Run-Test "New-UefiBootEntry: firmware default set failure cleans up and returns null" {
    Reset-Mocks
    $env:UB_DEFAULT_SET_EXIT = '1'
    $guid = New-UefiBootEntry -DistroName "TestDistro" -DevicePartition "C:" -EfiPath "\EFI\BOOT\BOOTx64.EFI"
    Assert-True ($null -eq $guid) "expected null, got $guid"
    Assert-LogContains "Failed to set firmware default" -IsError
}

# ── 6. default verification failure -> entry cleaned up, null returned ───────
Run-Test "New-UefiBootEntry: firmware default verification failure returns null" {
    Reset-Mocks
    $env:UB_COPY_GUID = '{33333333-3333-3333-3333-333333333333}'
    # /enum {fwbootmgr} reports a DIFFERENT default -> verification fails
    $env:UB_ENUM_DEFAULT = '{44444444-4444-4444-4444-444444444444}'
    $guid = New-UefiBootEntry -DistroName "TestDistro" -DevicePartition "C:" -EfiPath "\EFI\BOOT\BOOTx64.EFI"
    Assert-True ($null -eq $guid) "expected null when default not verified, got $guid"
    Assert-LogContains "Firmware default verification failed" -IsError
}

# ---------- 7. Test-UelliFirmwareDefault ─────────────────────────────────────
Run-Test "Test-UelliFirmwareDefault: matching default returns true" {
    Reset-Mocks
    $env:UB_ENUM_DEFAULT = '{55555555-5555-5555-5555-555555555555}'
    Assert-True ((Test-UelliFirmwareDefault -Guid '{55555555-5555-5555-5555-555555555555}') -eq $true)
}
Run-Test "Test-UelliFirmwareDefault: different default returns false" {
    Reset-Mocks
    $env:UB_ENUM_DEFAULT = '{66666666-6666-6666-6666-666666666666}'
    Assert-True ((Test-UelliFirmwareDefault -Guid '{55555555-5555-5555-5555-555555555555}') -eq $false)
}
Run-Test "Test-UelliFirmwareDefault: malformed default output returns false" {
    Reset-Mocks
    $env:UB_ENUM_DEFAULT = 'not-a-guid'
    Assert-True ((Test-UelliFirmwareDefault -Guid '{55555555-5555-5555-5555-555555555555}') -eq $false)
}
Run-Test "Test-UelliFirmwareDefault: empty default output returns false" {
    Reset-Mocks
    $env:UB_ENUM_DEFAULT = ''
    Assert-True ((Test-UelliFirmwareDefault -Guid '{55555555-5555-5555-5555-555555555555}') -eq $false)
}
Run-Test "Set-FirmwareDefault: thrown exception (bcdedit unavailable) returns false" {
    Reset-Mocks
    # Temporarily remove the fake bcdedit from PATH so Start-Process throws;
    # Set-FirmwareDefault must catch it and return false (never leave readiness true).
    $env:PATH = $script:oldPath
    $result = Set-FirmwareDefault -Guid '{11111111-1111-1111-1111-111111111111}'
    $env:PATH = "$script:fakeBcdDir$sep$env:PATH"
    Assert-True ($result -eq $false) "expected false when bcdedit throws, got $result"
}

# ---------- 8. Test-UelliEntryDevice ─────────────────────────────────────────
Run-Test "Test-UelliEntryDevice: partition=D matches drive D" {
    $entry = @{ Device = 'partition=D:' }
    Assert-True ((Test-UelliEntryDevice -Entry $entry -BootDriveLetter 'D') -eq $true)
}
Run-Test "Test-UelliEntryDevice: partition=E does not match drive D" {
    $entry = @{ Device = 'partition=E:' }
    Assert-True ((Test-UelliEntryDevice -Entry $entry -BootDriveLetter 'D') -eq $false)
}
Run-Test "Test-UelliEntryDevice: missing device returns false" {
    $entry = @{ ID = '{abc}' }
    Assert-True ((Test-UelliEntryDevice -Entry $entry -BootDriveLetter 'D') -eq $false)
}
Run-Test "Test-UelliEntryDevice: HarddiskVolume resolving to matching drive letter matches" {
    $entry = @{ ID = '{h}'; Description = 'X'; Device = 'partition=\Device\HarddiskVolume2' }
    $resolver = { param([string]$DevicePath) 'D' }
    Assert-True ((Test-UelliEntryDevice -Entry $entry -BootDriveLetter 'D' -ResolveDriveLetter $resolver) -eq $true)
}
Run-Test "Test-UelliEntryDevice: HarddiskVolume resolving to a different drive letter does not match" {
    $entry = @{ ID = '{h}'; Description = 'X'; Device = 'partition=\Device\HarddiskVolume2' }
    $resolver = { param([string]$DevicePath) 'E' }
    Assert-True ((Test-UelliEntryDevice -Entry $entry -BootDriveLetter 'D' -ResolveDriveLetter $resolver) -eq $false)
}
Run-Test "Test-UelliEntryDevice: HarddiskVolume that cannot be resolved is unmatched (fail closed)" {
    $entry = @{ ID = '{h}'; Description = 'X'; Device = 'partition=\Device\HarddiskVolume2' }
    $resolver = { param([string]$DevicePath) $null }
    Assert-True ((Test-UelliEntryDevice -Entry $entry -BootDriveLetter 'D' -ResolveDriveLetter $resolver) -eq $false)
}
Run-Test "Test-UelliEntryDevice: unsupported device representation is unmatched (fail closed)" {
    foreach ($device in @('partition=\Device\HarddiskVolume2\extra', 'partition=\Device\HarddiskVolume', 'removable=USB', '')) {
        $entry = @{ ID = '{x}'; Device = $device }
        Assert-True ((Test-UelliEntryDevice -Entry $entry -BootDriveLetter 'D') -eq $false) "device '$device' should not match"
    }
}

# ---------- 9. Select-UelliExistingBootEntry: no broad substring selection ---
Run-Test "Select-UelliExistingBootEntry: exact single match with device -> reused" {
    Reset-Mocks
    $entries = @(
        @{ ID = '{a}'; Description = 'Linux Mint 22.3'; Device = 'partition=D:'; Path = '\EFI\BOOT\BOOTx64.EFI' }
    )
    $sel = Select-UelliExistingBootEntry -BootEntries $entries -DistroName 'Linux Mint 22.3' -BootDriveLetter 'D' -EfiPath '\EFI\BOOT\BOOTx64.EFI'
    Assert-True ($sel.ID -eq '{a}') "expected {a}, got $($sel.ID)"
}
Run-Test "Select-UelliExistingBootEntry: multiple exact matches -> ambiguous, null" {
    Reset-Mocks
    $entries = @(
        @{ ID = '{a}'; Description = 'Linux Mint 22.3'; Device = 'partition=D:' },
        @{ ID = '{b}'; Description = 'Linux Mint 22.3'; Device = 'partition=D:' }
    )
    $sel = Select-UelliExistingBootEntry -BootEntries $entries -DistroName 'Linux Mint 22.3' -BootDriveLetter 'D'
    Assert-True ($null -eq $sel) "expected null for ambiguous entries, got $($sel.ID)"
    Assert-LogContains "refusing to modify any of them" -IsError
}
Run-Test "Select-UelliExistingBootEntry: no exact match -> null (no substring fallback)" {
    Reset-Mocks
    $entries = @(
        @{ ID = '{a}'; Description = 'Some Other Distro'; Device = 'partition=D:' }
    )
    $sel = Select-UelliExistingBootEntry -BootEntries $entries -DistroName 'Linux Mint 22.3' -BootDriveLetter 'D'
    Assert-True ($null -eq $sel) "expected null when no exact match, got $($sel.ID)"
}
Run-Test "Select-UelliExistingBootEntry: exact match but device mismatch -> null" {
    Reset-Mocks
    $entries = @(
        @{ ID = '{a}'; Description = 'Linux Mint 22.3'; Device = 'partition=E:' }
    )
    $sel = Select-UelliExistingBootEntry -BootEntries $entries -DistroName 'Linux Mint 22.3' -BootDriveLetter 'D' -EfiPath '\EFI\BOOT\BOOTx64.EFI'
    Assert-True ($null -eq $sel) "expected null for device mismatch, got $($sel.ID)"
    Assert-LogContains "treating as ambiguous" -IsError
}
Run-Test "Select-UelliExistingBootEntry: exact match with matching path -> reused" {
    Reset-Mocks
    $entries = @(
        @{ ID = '{a}'; Description = 'Linux Mint 22.3'; Device = 'partition=D:'; Path = '\EFI\BOOT\BOOTx64.EFI' }
    )
    $sel = Select-UelliExistingBootEntry -BootEntries $entries -DistroName 'Linux Mint 22.3' -BootDriveLetter 'D' -EfiPath '\EFI\BOOT\BOOTx64.EFI'
    Assert-True ($sel.ID -eq '{a}') "expected {a}, got $($sel.ID)"
}
Run-Test "Select-UelliExistingBootEntry: exact match with stale/different path -> not reused" {
    Reset-Mocks
    $entries = @(
        @{ ID = '{a}'; Description = 'Linux Mint 22.3'; Device = 'partition=D:'; Path = '\EFI\ubuntu\shimx64.efi' }
    )
    $sel = Select-UelliExistingBootEntry -BootEntries $entries -DistroName 'Linux Mint 22.3' -BootDriveLetter 'D' -EfiPath '\EFI\BOOT\BOOTx64.EFI'
    Assert-True ($null -eq $sel) "expected null for path mismatch, got $($sel.ID)"
    Assert-LogContains "does not match the intended loader path" -IsError
}
Run-Test "Select-UelliExistingBootEntry: exact match with no path recorded -> not reused" {
    Reset-Mocks
    $entries = @(
        @{ ID = '{a}'; Description = 'Linux Mint 22.3'; Device = 'partition=D:' }
    )
    $sel = Select-UelliExistingBootEntry -BootEntries $entries -DistroName 'Linux Mint 22.3' -BootDriveLetter 'D' -EfiPath '\EFI\BOOT\BOOTx64.EFI'
    Assert-True ($null -eq $sel) "expected null for missing path, got $($sel.ID)"
    Assert-LogContains "no EFI path recorded" -IsError
}

# ---------- 10. Configure-UelliBootEntry (production helper with injectables) --
$script:mockNewUefiBootEntryGuid = $null

function Mock-NewUefiBootEntry {
    param([string]$d, [string]$dp, [string]$ep)
    return $script:mockNewUefiBootEntryGuid
}

function Mock-GetFirmwareEntries {
    param()
    return $script:mockFirmwareEntries
}

function Mock-SetFirmwareDefault {
    param([string]$g)
    return $script:mockSetFirmwareDefaultResult
}

Run-Test "Configure-UelliBootEntry: RefindPlus success -> readiness true" {
    Reset-Mocks
    $script:mockNewUefiBootEntryGuid = '{77777777-7777-7777-7777-777777777777}'
    $result = Configure-UelliBootEntry `
        -DistroName "Linux Mint 22.3" `
        -BootDriveLetter 'D' `
        -EfiPath "\EFI\BOOT\BOOTx64.EFI" `
        -UseRefind `
        -RefindDriveLetter 'E' `
        -NewUefiBootEntry (${function:Mock-NewUefiBootEntry})
    Assert-True ($result.Ready -eq $true) "readiness should be true, got $($result.Ready)"
    Assert-True ($result.UseRefind -eq $true) "UseRefind should remain true"
    Assert-LogContains "RefindPlus UEFI boot entry created and set as the firmware default"
}
Run-Test "Configure-UelliBootEntry: RefindPlus failure falls back to standard success" {
    Reset-Mocks
    # RefindPlus entry creation fails -> falls back to standard path
    $script:mockNewUefiBootEntryGuid = $null
    $script:mockFirmwareEntries = @()
    # Second New-UefiBootEntry call (standard) succeeds
    $callCount = 0
    $script:mockNewUefiBootEntryGuid = $null
    # Use a counter-based mock: first call (RefindPlus) null, second (standard) GUID
    $script:refindFallbackCalls = 0
    function Counter-NewUefiBootEntry {
        param([string]$d, [string]$dp, [string]$ep)
        $script:refindFallbackCalls++
        if ($script:refindFallbackCalls -eq 1) { return $null }
        return '{88888888-8888-8888-8888-888888888888}'
    }
    $result = Configure-UelliBootEntry `
        -DistroName "Linux Mint 22.3" `
        -BootDriveLetter 'D' `
        -EfiPath "\EFI\BOOT\BOOTx64.EFI" `
        -UseRefind `
        -RefindDriveLetter 'E' `
        -NewUefiBootEntry (${function:Counter-NewUefiBootEntry}) `
        -GetFirmwareEntries (${function:Mock-GetFirmwareEntries})
    Assert-True ($result.Ready -eq $true) "readiness should be true after fallback, got $($result.Ready)"
    Assert-True ($result.UseRefind -eq $false) "UseRefind cleared after RefindPlus failure"
    Assert-LogContains "falling back to standard UEFI boot entry" -IsError
    Assert-LogContains "New UEFI boot entry created and set as the firmware default"
}
Run-Test "Configure-UelliBootEntry: existing entry selected and default set -> readiness true" {
    Reset-Mocks
    $script:mockNewUefiBootEntryGuid = $null
    $script:mockFirmwareEntries = @(
        @{ ID = '{x}'; Description = 'Linux Mint 22.3'; Device = 'partition=D:'; Path = '\EFI\BOOT\BOOTx64.EFI' }
    )
    $script:mockSetFirmwareDefaultResult = $true
    $result = Configure-UelliBootEntry `
        -DistroName "Linux Mint 22.3" `
        -BootDriveLetter 'D' `
        -EfiPath "\EFI\BOOT\BOOTx64.EFI" `
        -NewUefiBootEntry (${function:Mock-NewUefiBootEntry}) `
        -GetFirmwareEntries (${function:Mock-GetFirmwareEntries}) `
        -SetFirmwareDefault (${function:Mock-SetFirmwareDefault})
    Assert-True ($result.Ready -eq $true) "readiness should be true for existing entry, got $($result.Ready)"
    Assert-LogContains "UEFI boot default set to existing entry"
}
Run-Test "Configure-UelliBootEntry: existing entry with stale path creates a new entry instead" {
    Reset-Mocks
    $script:mockNewUefiBootEntryGuid = '{99999999-9999-9999-9999-999999999999}'
    $script:mockFirmwareEntries = @(
        @{ ID = '{x}'; Description = 'Linux Mint 22.3'; Device = 'partition=D:'; Path = '\EFI\ubuntu\shimx64.efi' }
    )
    $script:mockSetFirmwareDefaultResult = $true
    $result = Configure-UelliBootEntry `
        -DistroName "Linux Mint 22.3" `
        -BootDriveLetter 'D' `
        -EfiPath "\EFI\BOOT\BOOTx64.EFI" `
        -NewUefiBootEntry (${function:Mock-NewUefiBootEntry}) `
        -GetFirmwareEntries (${function:Mock-GetFirmwareEntries}) `
        -SetFirmwareDefault (${function:Mock-SetFirmwareDefault})
    Assert-True ($result.Ready -eq $true) "readiness should be true after creating new entry, got $($result.Ready)"
    Assert-LogContains "New UEFI boot entry created and set as the firmware default"
    Assert-LogContains "does not match the intended loader path" -IsError
}
Run-Test "Configure-UelliBootEntry: ambiguous existing entries -> creates new entry" {
    Reset-Mocks
    $script:refindFallbackCalls = 0
    $script:mockNewUefiBootEntryGuid = '{99999999-9999-9999-9999-999999999999}'
    $script:mockFirmwareEntries = @(
        @{ ID = '{a}'; Description = 'Linux Mint 22.3'; Device = 'partition=D:' },
        @{ ID = '{b}'; Description = 'Linux Mint 22.3'; Device = 'partition=D:' }
    )
    $result = Configure-UelliBootEntry `
        -DistroName "Linux Mint 22.3" `
        -BootDriveLetter 'D' `
        -EfiPath "\EFI\BOOT\BOOTx64.EFI" `
        -NewUefiBootEntry (${function:Mock-NewUefiBootEntry}) `
        -GetFirmwareEntries (${function:Mock-GetFirmwareEntries})
    Assert-True ($result.Ready -eq $true) "readiness should be true after creating new entry, got $($result.Ready)"
    Assert-LogContains "New UEFI boot entry created and set as the firmware default"
    Assert-LogContains "refusing to modify any of them" -IsError
}
Run-Test "Configure-UelliBootEntry: both strategies fail -> readiness false" {
    Reset-Mocks
    $script:mockNewUefiBootEntryGuid = $null
    $script:mockFirmwareEntries = @()
    $script:mockSetFirmwareDefaultResult = $false
    $result = Configure-UelliBootEntry `
        -DistroName "Linux Mint 22.3" `
        -BootDriveLetter 'D' `
        -EfiPath "\EFI\BOOT\BOOTx64.EFI" `
        -NewUefiBootEntry (${function:Mock-NewUefiBootEntry}) `
        -GetFirmwareEntries (${function:Mock-GetFirmwareEntries}) `
        -SetFirmwareDefault (${function:Mock-SetFirmwareDefault})
    Assert-True ($result.Ready -eq $false) "readiness should be false when both fail, got $($result.Ready)"
    Assert-LogContains "Could not create UEFI boot entry automatically" -IsError
}
Run-Test "Configure-UelliBootEntry: readiness does not leak between runs" {
    Reset-Mocks
    $script:mockNewUefiBootEntryGuid = '{aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa}'
    $r1 = Configure-UelliBootEntry `
        -DistroName "Linux Mint 22.3" -BootDriveLetter 'D' -EfiPath "\EFI\BOOT\BOOTx64.EFI" `
        -NewUefiBootEntry (${function:Mock-NewUefiBootEntry})
    Assert-True ($r1.Ready -eq $true) "first run readiness should be true"
    # Second run: entry creation fails; readiness must reset to false (no leak).
    $script:mockNewUefiBootEntryGuid = $null
    $r2 = Configure-UelliBootEntry `
        -DistroName "Linux Mint 22.3" -BootDriveLetter 'D' -EfiPath "\EFI\BOOT\BOOTx64.EFI" `
        -NewUefiBootEntry (${function:Mock-NewUefiBootEntry})
    Assert-True ($r2.Ready -eq $false) "second run readiness should be false, no leak from first run"
}

# ---------- 11. Restart eligibility ──────────────────────────────────────────
Run-Test "Restart eligible only when auto-restart AND readiness are true" {
    Assert-True ((Test-UelliBootRestartEligible -AutoRestart $true -BootReady $true) -eq $true)
    Assert-True ((Test-UelliBootRestartEligible -AutoRestart $true -BootReady $false) -eq $false)
    Assert-True ((Test-UelliBootRestartEligible -AutoRestart $false -BootReady $true) -eq $false)
    Assert-True ((Test-UelliBootRestartEligible -AutoRestart $false -BootReady $false) -eq $false)
}

# ---------- 12. Conditional status text ──────────────────────────────────────
Run-Test "Status text: complete when ready, incomplete when not" {
    Assert-True ((Get-UelliBootStatusText -BootReady $true) -eq "Installation complete - boot default configured")
    Assert-True ((Get-UelliBootStatusText -BootReady $false) -eq "Installation finished - boot configuration incomplete")
}

# ---------- 3c-ext. /copy output with a non-canonical GUID ({123}) -> null ───
Run-Test "New-UefiBootEntry: non-canonical GUID {123} is rejected" {
    Reset-Mocks
    $env:UB_COPY_OUTPUT = 'The entry was successfully copied to {123}.'
    $guid = New-UefiBootEntry -DistroName "TestDistro" -DevicePartition "C:" -EfiPath "\EFI\BOOT\BOOTx64.EFI"
    Assert-True ($null -eq $guid) "expected null for non-canonical GUID, got $guid"
    Assert-LogContains "no entry GUID could be parsed" -IsError
}

# ---------- Test-UelliEntryDevice: lowercase drive letters ---------------------
Run-Test "Test-UelliEntryDevice: lowercase partition=d matches drive D" {
    $entry = @{ Device = 'partition=d:' }
    Assert-True ((Test-UelliEntryDevice -Entry $entry -BootDriveLetter 'D') -eq $true)
}
Run-Test "Test-UelliEntryDevice: lowercase partition=e does not match drive D" {
    $entry = @{ Device = 'partition=e:' }
    Assert-True ((Test-UelliEntryDevice -Entry $entry -BootDriveLetter 'D') -eq $false)
}

# ---------- 13. Start-Installation: readiness reset on exception after readiness set --
# Mocks are defined at script scope so Start-Installation (called later in this
# same scope) resolves them. Functions that are also extracted/production helpers
# (Configure-UelliBootEntry, Get-UelliBootStatusText) are saved/restored around
# the single test so other tests in this file keep using the production bodies.
$script:customRadio = [PSCustomObject]@{ Checked = $true }
$script:deleteIsoCheck = [PSCustomObject]@{ Checked = $false }
$script:autoRestartCheck = [PSCustomObject]@{ Checked = $false }
$script:CustomIsoPath = 'C:\ULLI\isos\linuxmint-22.3-cinnamon-64bit.iso'
$script:IsoPath = $script:CustomIsoPath

function Get-SelectedDistro {
    [PSCustomObject]@{
        Name           = 'Linux Mint 22.3'
        IsoFilename    = 'linuxmint-22.3-cinnamon-64bit.iso'
        Keyword        = 'Mint'
        ValidationFile = 'casper\vmlinuz'
        IsHybrid       = $false
        ToRam          = $false
        ExpectedSize   = 'approximately 2.9 GB'
    }
}

function Show-DiskPlan {
    param([string]$DistroName)
    [PSCustomObject]@{
        Approved         = $true
        Strategy         = 'use_free_all'
        TargetDiskNumber = 1
        ShrinkDriveLetter = $null
        ShrinkAmountGB   = 0
        LinuxSizeGB      = 20
        BootPartSizeGB   = 7
        UseRefind        = $false
        LiveOnly         = $false
    }
}

function Get-DiskUnallocatedGB {
    param([int]$DiskNumber)
    100
}

function Test-Path {
    param($Path)
    $true
}

function Mount-DiskImage {
    param($ImagePath, $StorageType, [switch]$PassThru)
    [PSCustomObject]@{ DeviceNumber = 0 }
}

function Dismount-DiskImage {
    param($ImagePath)
}

function Get-Volume {
    param($DiskImage, $DriveLetter)
    if ($PSBoundParameters.ContainsKey('DiskImage')) {
        [PSCustomObject]@{ DriveLetter = 'E' }
    } else {
        [PSCustomObject]@{ SizeRemaining = [int64](50GB) }
    }
}

# robocopy stub: leave $LASTEXITCODE at 0 so the copy check passes.
function robocopy { }

function Get-ChildItem {
    param($Path, [switch]$Recurse, [switch]$Force)
}

function Set-UILocked { param([bool]$Locked) }

function Test-UelliBootRestartEligible {
    param([bool]$AutoRestart, [bool]$BootReady)
    ($AutoRestart -and $BootReady)
}

# Flipped to $true by the mocked Configure-UelliBootEntry once boot
# configuration for this run has completed. Get-Partition calls before that
# point (the gap scan) succeed; the first call after (the post-install
# layout re-query) throws, reproducing an exception escaping after
# readiness was set to true.
$script:bootConfigDone = $false

function Get-Partition {
    param($DriveLetter, $DiskNumber, $PartitionNumber)
    if ($script:bootConfigDone) {
        throw "Get-Partition failure after boot configuration"
    }
    @(
        [PSCustomObject]@{ DiskNumber = 1; PartitionNumber = 1; Size = [int64](100GB); Offset = [int64](1MB); DriveLetter = 'C'; Type = 'Basic' }
        [PSCustomObject]@{ DiskNumber = 1; PartitionNumber = 2; Size = [int64](500MB); Offset = [int64](100GB); DriveLetter = $null; Type = 'System' }
    )
}

function Get-Disk {
    param([int]$Number)
    [PSCustomObject]@{ Number = 1; Size = [int64](200GB); OperationalStatus = 'Online'; PartitionStyle = 'GPT' }
}

function New-Partition {
    param([int]$DiskNumber, [int64]$Size, [int64]$Offset, [switch]$AssignDriveLetter)
    [PSCustomObject]@{ DiskNumber = 1; PartitionNumber = 3; Size = $Size; Offset = $Offset; DriveLetter = 'Z'; Type = 'Basic' }
}

function Format-Volume {
    param([string]$DriveLetter, [string]$FileSystem, [string]$NewFileSystemLabel, [switch]$Confirm)
}

function Start-Sleep { param([int]$Seconds) }

Run-Test "Start-Installation: readiness is reset to false after an installation failure" {
    Reset-Mocks
    # ShouldProcess gates the start of Start-Installation; in a non-interactive test
    # host we must opt out of confirmation prompts to reach the installation body.
    $savedConfirmPreference = $ConfirmPreference
    $ConfirmPreference = 'None'
    # Save the production helpers this test overrides, then restore them so the
    # remaining tests in this file are unaffected.
    $savedConfigure = ${function:Configure-UelliBootEntry}
    $savedStatusText = ${function:Get-UelliBootStatusText}
    try {
        function Configure-UelliBootEntry {
            param([string]$DistroName, [string]$BootDriveLetter, [string]$EfiPath,
                  [switch]$UseRefind, [string]$RefindDriveLetter,
                  [scriptblock]$NewUefiBootEntry, [scriptblock]$GetFirmwareEntries,
                  [scriptblock]$SetFirmwareDefault)
            $script:bootConfigDone = $true
            [PSCustomObject]@{ Ready = $true; UseRefind = $false }
        }
        function Get-UelliBootStatusText { param([bool]$BootReady) 'stub' }
        # Keep the robocopy copy-check below the failure threshold.
        $LASTEXITCODE = 0
        Start-Installation -Confirm:$false
        # Readiness must not be left true after an installation failure. On
        # Windows (where Get-Partition can be mocked) boot configuration is
        # reached and readiness is set to true before the exception escapes, so
        # the outer catch must reset it to false here. On this Linux host the
        # Storage cmdlets are unavailable so the installation fails earlier,
        # but readiness must still end up false.
        Assert-True ($script:uefiBootReady -eq $false) "readiness should be false after an installation failure, got $script:uefiBootReady"
        # Restart must not be attempted when the installation failed.
        Assert-True ($script:mockRestartCalled -eq $false) "restart should not be attempted after a failed installation"
    } finally {
        ${function:Configure-UelliBootEntry} = $savedConfigure
        ${function:Get-UelliBootStatusText} = $savedStatusText
        $ConfirmPreference = $savedConfirmPreference
    }
}



# ---------- 14. Get-FirmwareEntriesFromBcdedit -----------------------------------
Run-Test "Get-FirmwareEntriesFromBcdedit: parses representative firmware output" {
    Reset-Mocks
    $env:UB_FIRMWARE_OUTPUT = @'
Windows Boot Manager
--------------------
identifier {fwbootmgr}
description Windows Boot Manager
device partition=\Device\HarddiskVolume1
path \EFI\Microsoft\Boot\bootmgfw.efi

Firmware Application (101fffff)
----------------------------------------------------------------------
identifier {11111111-1111-1111-1111-111111111111}
description Linux Mint 22.3
device partition=C:
path \EFI\BOOT\BOOTx64.EFI

Firmware Application (101fffff)
----------------------------------------------------------------------
identifier {22222222-2222-2222-2222-222222222222}
description Ubuntu 26.04
device partition=\Device\HarddiskVolume3
path \EFI\ubuntu\shimx64.efi
'@
    $entries = @(Get-FirmwareEntriesFromBcdedit)
    Assert-True ($entries.Count -eq 2) "expected 2 firmware entries, got $($entries.Count)"
    Assert-True ($entries[0].ID -eq '{11111111-1111-1111-1111-111111111111}')
    Assert-True ($entries[0].Description -eq 'Linux Mint 22.3')
    Assert-True ($entries[0].Device -eq 'partition=C:')
    Assert-True ($entries[0].Path -eq '\EFI\BOOT\BOOTx64.EFI')
    Assert-True ($entries[1].ID -eq '{22222222-2222-2222-2222-222222222222}')
    Assert-True ($entries[1].Description -eq 'Ubuntu 26.04')
    Assert-True ($entries[1].Device -eq 'partition=\Device\HarddiskVolume3')
    Assert-True ($entries[1].Path -eq '\EFI\ubuntu\shimx64.efi')
}

Run-Test "Get-FirmwareEntriesFromBcdedit: missing device and path fields" {
    Reset-Mocks
    $env:UB_FIRMWARE_OUTPUT = @'
Firmware Application (101fffff)
----------------------------------------------------------------------
identifier {33333333-3333-3333-3333-333333333333}
description Entry without device or path
'@
    $entries = @(Get-FirmwareEntriesFromBcdedit)
    Assert-True ($entries.Count -eq 1)
    Assert-True ($entries[0].ID -eq '{33333333-3333-3333-3333-333333333333}')
    Assert-True ($null -eq $entries[0].Device -or $entries[0].Device -eq '') "missing device should be empty"
    Assert-True ($null -eq $entries[0].Path -or $entries[0].Path -eq '') "missing path should be empty"
}

Run-Test "Get-FirmwareEntriesFromBcdedit: no firmware applications returns empty" {
    Reset-Mocks
    $env:UB_FIRMWARE_OUTPUT = @'
Windows Boot Manager
--------------------
identifier {fwbootmgr}
description Windows Boot Manager
'@
    $entries = @(Get-FirmwareEntriesFromBcdedit)
    Assert-True ($entries.Count -eq 0) "expected empty when no firmware application blocks, got $($entries.Count)"
}

Run-Test "Get-FirmwareEntriesFromBcdedit: bcdedit unavailable returns empty" {
    Reset-Mocks
    # Hide the fake bcdedit so `bcdedit` is not found; the parser must fail
    # safely and return an empty list rather than throw.
    $env:PATH = $script:oldPath
    try {
        $entries = @(Get-FirmwareEntriesFromBcdedit)
        Assert-True ($entries.Count -eq 0) "expected empty when bcdedit is unavailable, got $($entries.Count)"
        Assert-LogContains "Failed to enumerate firmware boot entries" -IsError
    } finally {
        $env:PATH = "$script:fakeBcdDir$sep$env:PATH"
    }
}

Run-Test "Get-FirmwareEntriesFromBcdedit: malformed field values are captured as-is" {
    Reset-Mocks
    $env:UB_FIRMWARE_OUTPUT = @'
Firmware Application (101fffff)
----------------------------------------------------------------------
identifier not-a-guid-at-all
description Malformed entry
device partition=\Device\HarddiskVolume9
path \EFI\BOOT\BOOTx64.EFI
'@
    $entries = @(Get-FirmwareEntriesFromBcdedit)
    Assert-True ($entries.Count -eq 1)
    Assert-True ($entries[0].ID -eq 'not-a-guid-at-all') "malformed identifier captured as-is, got $($entries[0].ID)"
}

# ---------- Summary ----------
Write-Host ""
Write-Host "Results: $script:testsPassed passed, $script:testsFailed failed"
if ($script:testsFailed -gt 0) {
    Write-Host "Failures:"
    foreach ($f in $script:failures) { Write-Host "  $($f.Name) : $($f.Error)" }
    exit 1
}
exit 0
