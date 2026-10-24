# Mock-based, non-destructive unit tests for UEFI boot-readiness logic in windows/ulli-windows.ps1.
# Every destructive/firmware command (bcdedit, Restart-Computer, New-UefiBootEntry) is replaced by a
# test double, so no real disk, firmware entry, or restart is performed. Safe to run on any host.
#
# Usage: pwsh -NoProfile -File windows/ulli-windows.boot-readiness-tests.ps1
#
# Covers:
#   1. Successful boot configuration with auto-restart enabled -> readiness true, restart called.
#   2. Failed boot configuration with auto-restart enabled -> readiness false, no restart, no success claim.
#   3. Successful boot configuration with auto-restart disabled -> readiness true, no restart.
#   4. Successful first run followed by failed second run -> readiness does not leak (reset at start).
#   5. RefindPlus entry failure followed by successful standard-entry fallback -> readiness true.
#   6. Both boot-entry strategies fail -> readiness false, manual recovery instructions in log.
#   7. Status bar / log messages agree with actual boot-readiness state.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if (-not $env:TEMP) { $env:TEMP = '/tmp' }

$script:testsPassed = 0
$script:testsFailed = 0
$script:failures = New-Object System.Collections.Generic.List[object]
$script:mockLogEntries = New-Object System.Collections.Generic.List[object]

# ---------- Test doubles ----------
$script:mockNewUefiBootEntryResult = $true
$script:mockRestartCalled = $false

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
}

function Restart-Computer {
    [CmdletBinding()]
    param([switch]$Force)
    $script:mockRestartCalled = $true
}

# Controllable double for the production New-UefiBootEntry function.
function New-UefiBootEntry {
    [CmdletBinding()]
    param([string]$DistroName, [string]$DevicePartition, [string]$EfiPath)
    return $script:mockNewUefiBootEntryResult
}

# ---------- Boot-readiness decision logic (mirrors Start-Installation's boot-config block) ----------
function Invoke-BootReadinessScenario {
    [CmdletBinding()]
    param(
        [bool]$AutoRestart,
        [bool]$UseRefind,
        [string]$RefindPlusDriveLetter = $null,
        [bool]$RefindEntrySucceeds = $true,
        [bool]$StandardEntrySucceeds = $true,
        [switch]$SkipReadinessReset   # simulates the OLD (buggy) behavior without the reset
    )

    $script:mockNewUefiBootEntryResult = $true
    $script:mockRestartCalled = $false
    $script:mockLogEntries.Clear()

    # --- The fix: reset readiness at the start of every installation ---
    if (-not $SkipReadinessReset) {
        $script:uefiBootReady = $false
    }

    # --- Boot configuration (independent of the auto-restart checkbox) ---
    if ($UseRefind -and $RefindPlusDriveLetter) {
        Write-LogMessage "Creating UEFI boot entry for RefindPlus..."
        $script:mockNewUefiBootEntryResult = $RefindEntrySucceeds
        $bootCreated = New-UefiBootEntry -DistroName "RefindPlus - ULLI" `
            -DevicePartition "${RefindPlusDriveLetter}:" -EfiPath "\EFI\refindplus\x64_RefindPlus_REL.efi"

        if ($bootCreated) {
            Write-LogMessage "RefindPlus UEFI boot entry created."
            $script:uefiBootReady = $true
        } else {
            Write-LogMessage "Could not create RefindPlus boot entry automatically" -IsError
            Write-LogMessage "RefindPlus entry creation failed; falling back to standard UEFI boot entry." -IsError
            $UseRefind = $false
        }
    }

    if (-not $UseRefind) {
        Write-LogMessage "Creating standard UEFI boot entry..."
        $script:mockNewUefiBootEntryResult = $StandardEntrySucceeds
        $bootCreated = New-UefiBootEntry -DistroName "TestDistro" `
            -DevicePartition "C:" -EfiPath "\EFI\BOOT\BOOTx64.EFI"

        if ($bootCreated) {
            Write-LogMessage "UEFI boot priority set successfully!"
            $script:uefiBootReady = $true
        } else {
            Write-LogMessage "Could not create UEFI boot entry automatically" -IsError
            Write-LogMessage "You will need to set boot priority manually in UEFI/BIOS settings" -IsError
        }
    }

    # --- Auto-restart only when enabled AND boot configuration succeeded ---
    if ($AutoRestart -and $script:uefiBootReady) {
        Write-LogMessage "Preparing for automatic restart..."
        Restart-Computer -Force
    } elseif ($AutoRestart -and -not $script:uefiBootReady) {
        Write-LogMessage "Automatic restart skipped: UEFI boot configuration was not successful." -IsError
        Write-LogMessage "Restart manually and configure boot priority in UEFI/BIOS settings when ready." -IsError
    }

    return [PSCustomObject]@{
        UefiBootReady   = [bool]$script:uefiBootReady
        RestartCalled   = $script:mockRestartCalled
        LogEntries      = $script:mockLogEntries
        UseRefind       = $UseRefind
    }
}

# ---------- Test harness ----------
function Reset-Mocks {
    $script:mockNewUefiBootEntryResult = $true
    $script:mockRestartCalled = $false
    $script:mockLogEntries.Clear()
    $script:uefiBootReady = $false
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

# ── 1. Successful boot configuration with auto-restart enabled ──────────────────────
Run-Test "1. Successful boot config with auto-restart enabled -> readiness true, restart called" {
    Reset-Mocks
    $result = Invoke-BootReadinessScenario -AutoRestart $true -UseRefind $false -StandardEntrySucceeds $true
    Assert-True ($result.UefiBootReady -eq $true) "readiness should be true, got $($result.UefiBootReady)"
    Assert-True ($result.RestartCalled -eq $true) "restart should be called when auto-restart and ready"
    Assert-LogContains "UEFI boot priority set successfully!"
    Assert-LogContains "Preparing for automatic restart"
}

# ── 2. Failed boot config with auto-restart enabled: no restart, no success claim ──
Run-Test "2. Failed boot config with auto-restart enabled -> readiness false, no restart" {
    Reset-Mocks
    $result = Invoke-BootReadinessScenario -AutoRestart $true -UseRefind $false -StandardEntrySucceeds $false
    Assert-True ($result.UefiBootReady -eq $false) "readiness should be false, got $($result.UefiBootReady)"
    Assert-True ($result.RestartCalled -eq $false) "restart must NOT be called when boot config failed"
    Assert-LogContains "Could not create UEFI boot entry automatically" -IsError
    Assert-LogNotContains "UEFI boot priority has been configured."
    Assert-LogContains "Automatic restart skipped" -IsError
}

# ── 3. Successful boot config with auto-restart disabled: config occurs, no restart ─
Run-Test "3. Successful boot config with auto-restart disabled -> readiness true, no restart" {
    Reset-Mocks
    $result = Invoke-BootReadinessScenario -AutoRestart $false -UseRefind $false -StandardEntrySucceeds $true
    Assert-True ($result.UefiBootReady -eq $true) "readiness should be true even without auto-restart"
    Assert-True ($result.RestartCalled -eq $false) "restart must NOT be called when auto-restart disabled"
    Assert-LogContains "UEFI boot priority set successfully!"
}

# ── 4. Successful first run, failed second run: readiness does not leak ─────────────
Run-Test "4. First run success then second run failure -> readiness does not leak" {
    Reset-Mocks

    # First run: success
    $r1 = Invoke-BootReadinessScenario -AutoRestart $false -UseRefind $false -StandardEntrySucceeds $true
    Assert-True ($r1.UefiBootReady -eq $true) "first run readiness should be true"

    # Second run: failure (without reset, readiness would leak from first run)
    $r2 = Invoke-BootReadinessScenario -AutoRestart $false -UseRefind $false -StandardEntrySucceeds $false
    Assert-True ($r2.UefiBootReady -eq $false) "second run readiness should be false, no leak from first run"
    Assert-True ($r2.RestartCalled -eq $false) "no restart on second failed run"
}

# ── 4b. Without the readiness reset, stale success leaks (verifies the reset matters) ─
Run-Test "4b. Without readiness reset, stale success leaks into next run" {
    Reset-Mocks

    # First run: success
    $r1 = Invoke-BootReadinessScenario -AutoRestart $false -UseRefind $false -StandardEntrySucceeds $true
    Assert-True ($r1.UefiBootReady -eq $true)

    # Second run: failure, but SKIP the reset to simulate the OLD buggy behavior
    $r2 = Invoke-BootReadinessScenario -AutoRestart $false -UseRefind $false -StandardEntrySucceeds $false -SkipReadinessReset
    Assert-True ($r2.UefiBootReady -eq $true) "WITHOUT reset, stale true leaks from first run"
}

# ── 5. RefindPlus failure followed by successful standard-entry fallback ────────────
Run-Test "5. RefindPlus failure then standard fallback success -> readiness true" {
    Reset-Mocks
    $result = Invoke-BootReadinessScenario -AutoRestart $false -UseRefind $true `
        -RefindPlusDriveLetter "D" -RefindEntrySucceeds $false -StandardEntrySucceeds $true
    Assert-True ($result.UefiBootReady -eq $true) "readiness should be true after fallback succeeds"
    Assert-LogContains "RefindPlus entry creation failed; falling back to standard UEFI boot entry." -IsError
    Assert-LogContains "UEFI boot priority set successfully!"
    Assert-True ($result.UseRefind -eq $false) "UseRefind should be cleared after fallback"
}

# ── 6. Both boot-entry strategies fail: readiness false, manual instructions ────────
Run-Test "6. Both strategies fail -> readiness false, manual recovery instructions" {
    Reset-Mocks
    $result = Invoke-BootReadinessScenario -AutoRestart $false -UseRefind $true `
        -RefindPlusDriveLetter "D" -RefindEntrySucceeds $false -StandardEntrySucceeds $false
    Assert-True ($result.UefiBootReady -eq $false) "readiness should be false when both fail"
    Assert-True ($result.RestartCalled -eq $false) "no restart when both fail"
    Assert-LogContains "Could not create RefindPlus boot entry automatically" -IsError
    Assert-LogContains "Could not create UEFI boot entry automatically" -IsError
    Assert-LogContains "set boot priority manually" -IsError
    Assert-LogNotContains "UEFI boot priority has been configured."
}

# ── 7. Messages agree with actual boot-readiness state ──────────────────────────────
Run-Test "7. Success messages only when readiness true; failure messages when false" {
    Reset-Mocks

    # Success case
    $rs = Invoke-BootReadinessScenario -AutoRestart $false -UseRefind $false -StandardEntrySucceeds $true
    Assert-True ($rs.UefiBootReady -eq $true)
    $allSuccessLog = ($rs.LogEntries | ForEach-Object { $_.Message }) -join "`n"
    Assert-True ($allSuccessLog -like "*UEFI boot priority set successfully!*") "success log should confirm boot priority set"

    # Failure case
    $rf = Invoke-BootReadinessScenario -AutoRestart $false -UseRefind $false -StandardEntrySucceeds $false
    Assert-True ($rf.UefiBootReady -eq $false)
    $allFailLog = ($rf.LogEntries | ForEach-Object { $_.Message }) -join "`n"
    Assert-True (($allFailLog -like "*set boot priority manually*") -or ($allFailLog -like "*NOT yet bootable*")) `
        "failure log should contain manual recovery instructions"
    Assert-True ($allFailLog -notlike "*UEFI boot priority has been configured*") `
        "failure log must not claim boot priority was configured"
}

# ── 8. Production New-UefiBootEntry with fake bcdedit.exe: success ─────────────────
Run-Test "8. New-UefiBootEntry production code: bcdedit success returns true" {
    # Create a fake bcdedit.exe that returns a GUID on /copy and exit 0 on /set
    $fakeDir = Join-Path $env:TEMP "ulli_boot_tests_bcdedit"
    if (-not (Test-Path $fakeDir)) { New-Item -ItemType Directory -Path $fakeDir | Out-Null }
    $fakeExe = Join-Path $fakeDir "bcdedit.exe"
    Set-Content -Path $fakeExe -Value @'
#!/bin/bash
if [[ "$1" == "/copy" ]]; then
    echo "The operation completed successfully."
    echo "{12345678-1234-1234-1234-123456789012}"
    exit 0
elif [[ "$1" == "/set" ]] || [[ "$1" == "/deletevalue" ]] || [[ "$1" == "/delete" ]]; then
    exit 0
fi
exit 0
'@ -Encoding UTF8
    chmod 755 $fakeExe
    $oldPath = $env:PATH
    $sep = [System.IO.Path]::PathSeparator
    $env:PATH = "$fakeDir$sep$oldPath"
    try {
        $sourcePath = Join-Path $PSScriptRoot "ulli-windows.ps1"
        $source = Get-Content -Path $sourcePath -Raw
        $marker = 'function New-UefiBootEntry {'
        $idx = $source.IndexOf($marker)
        if ($idx -lt 0) { throw "Could not find '$marker' in $sourcePath" }
        $i = $idx + $marker.Length
        $depth = 1
        $sb = [System.Text.StringBuilder]::new()
        [void]$sb.Append($marker)
        $len = $source.Length
        while ($i -lt $len) {
            $c = $source[$i]
            [void]$sb.Append($c)
            if ($c -eq '{') { $depth++ }
            elseif ($c -eq '}') { $depth-- }
            $i++
            if ($depth -eq 0) { break }
        }
        if ($depth -ne 0) { throw "Brace mismatch extracting New-UefiBootEntry" }
        $funcBody = $sb.ToString()
        function Write-LogMessage { param([string]$Message, [switch]$IsError) }
        . ([scriptblock]::Create($funcBody))
        $result = New-UefiBootEntry -DistroName "TestDistro" -DevicePartition "C:" -EfiPath "\EFI\BOOT\BOOTx64.EFI"
        Assert-True ($result -eq $true) "New-UefiBootEntry should return true on bcdedit success, got $result"
    } finally {
        $env:PATH = $oldPath
        if (Test-Path $fakeExe) { Remove-Item $fakeExe -Force }
        if (Test-Path $fakeDir) { Remove-Item $fakeDir -Force -ErrorAction SilentlyContinue }
    }
}

# ── 9. Production New-UefiBootEntry: bcdedit /copy failure returns false ───────────
Run-Test "9. New-UefiBootEntry production code: bcdedit /copy failure returns false" {
    $fakeDir = Join-Path $env:TEMP "ulli_boot_tests_bcdedit_fail"
    if (-not (Test-Path $fakeDir)) { New-Item -ItemType Directory -Path $fakeDir | Out-Null }
    $fakeExe = Join-Path $fakeDir "bcdedit.exe"
    Set-Content -Path $fakeExe -Value @'
#!/bin/bash
echo "The operation failed." >&2
exit 1
'@ -Encoding UTF8
    chmod 755 $fakeExe
    $oldPath = $env:PATH
    $sep = [System.IO.Path]::PathSeparator
    $env:PATH = "$fakeDir$sep$oldPath"
    try {
        $sourcePath = Join-Path $PSScriptRoot "ulli-windows.ps1"
        $source = Get-Content -Path $sourcePath -Raw
        $marker = 'function New-UefiBootEntry {'
        $idx = $source.IndexOf($marker)
        if ($idx -lt 0) { throw "Could not find '$marker' in $sourcePath" }
        $i = $idx + $marker.Length
        $depth = 1
        $sb = [System.Text.StringBuilder]::new()
        [void]$sb.Append($marker)
        $len = $source.Length
        while ($i -lt $len) {
            $c = $source[$i]
            [void]$sb.Append($c)
            if ($c -eq '{') { $depth++ }
            elseif ($c -eq '}') { $depth-- }
            $i++
            if ($depth -eq 0) { break }
        }
        if ($depth -ne 0) { throw "Brace mismatch extracting New-UefiBootEntry" }
        $funcBody = $sb.ToString()
        function Write-LogMessage { param([string]$Message, [switch]$IsError) }
        . ([scriptblock]::Create($funcBody))
        $result = New-UefiBootEntry -DistroName "TestDistro" -DevicePartition "C:" -EfiPath "\EFI\BOOT\BOOTx64.EFI"
        Assert-True ($result -eq $false) "New-UefiBootEntry should return false on bcdedit failure, got $result"
    } finally {
        $env:PATH = $oldPath
        if (Test-Path $fakeExe) { Remove-Item $fakeExe -Force }
        if (Test-Path $fakeDir) { Remove-Item $fakeDir -Force -ErrorAction SilentlyContinue }
    }
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
