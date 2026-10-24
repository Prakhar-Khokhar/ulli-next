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
# UB_COPY_EXIT / UB_COPY_OUTPUT / UB_COPY_GUID   -> /copy "{bootmgr}" response
# UB_SET_EXIT / UB_DEFAULT_SET_EXIT / UB_ORDER_EXIT -> /set /displayorder exit codes
# UB_ENUM_DEFAULT                                 -> default GUID in /enum {fwbootmgr}
function New-FakeBcdEdit {
    param([string]$Dir)
    if (-not (Test-Path $Dir)) { New-Item -ItemType Directory -Path $Dir | Out-Null }
    $fakeExe = Join-Path $Dir "bcdedit.exe"
    Set-Content -Path $fakeExe -Value @'
#!/bin/bash
if [ "$1" = "/copy" ]; then
    printf '%s\n' "${UB_COPY_OUTPUT:-The operation completed successfully.}"
    printf '%s\n' "${UB_COPY_GUID:-{11111111-1111-1111-1111-111111111111}}"
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

$script:fakeBcdDir = Join-Path $env:TEMP "ulli_boot_readiness_bcdedit"
$script:fakeBcdExe = New-FakeBcdEdit -Dir $script:fakeBcdDir
$script:oldPath = $env:PATH
$sep = [System.IO.Path]::PathSeparator
$env:PATH = "$script:fakeBcdDir$sep$env:PATH"

function Reset-BcdEditEnv {
    $env:UB_COPY_EXIT = '0'
    $env:UB_COPY_OUTPUT = 'The operation completed successfully.'
    $env:UB_COPY_GUID = '{11111111-1111-1111-1111-111111111111}'
    $env:UB_SET_EXIT = '0'
    $env:UB_DEFAULT_SET_EXIT = '0'
    $env:UB_ORDER_EXIT = '0'
    $env:UB_ENUM_DEFAULT = '{11111111-1111-1111-1111-111111111111}'
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
    'function Test-UelliBootRestartEligible {'
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
    $env:UB_COPY_GUID = '{22222222-2222-2222-2222-222222222222}'
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
    $env:UB_COPY_GUID = 'no-guid-here'
    $guid = New-UefiBootEntry -DistroName "TestDistro" -DevicePartition "C:" -EfiPath "\EFI\BOOT\BOOTx64.EFI"
    Assert-True ($null -eq $guid) "expected null on unparseable output, got $guid"
    Assert-LogContains "no entry GUID could be parsed" -IsError
    Assert-LogContains "will not be deleted or modified" -IsError
}

# ── 4. device/path set failure -> entry cleaned up, null returned ────────────
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

# ---------- 9. Select-UelliExistingBootEntry: no broad substring selection ---
Run-Test "Select-UelliExistingBootEntry: exact single match with device -> reused" {
    Reset-Mocks
    $entries = @(
        @{ ID = '{a}'; Description = 'Linux Mint 22.3'; Device = 'partition=D:' }
    )
    $sel = Select-UelliExistingBootEntry -BootEntries $entries -DistroName 'Linux Mint 22.3' -BootDriveLetter 'D'
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
    $sel = Select-UelliExistingBootEntry -BootEntries $entries -DistroName 'Linux Mint 22.3' -BootDriveLetter 'D'
    Assert-True ($null -eq $sel) "expected null for device mismatch, got $($sel.ID)"
    Assert-LogContains "treating as ambiguous" -IsError
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
        @{ ID = '{x}'; Description = 'Linux Mint 22.3'; Device = 'partition=D:' }
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
    Assert-True ((Get-UelliBootStatusText -BootReady $true) -eq "Installation complete!")
    Assert-True ((Get-UelliBootStatusText -BootReady $false) -eq "Installation finished - boot configuration incomplete")
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
