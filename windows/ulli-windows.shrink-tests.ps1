# Mock-based, non-destructive unit tests for Shrink-UllPartition in windows/ulli-windows.ps1.
# Every destructive disk command (Get-Partition, Resize-Partition, diskpart) is replaced by a
# test double, so no real disk is touched. Safe to run on any PowerShell host.
#
# Usage: pwsh -NoProfile -File windows/ulli-windows.shrink-tests.ps1
#
# Covers audit cases:
#   1. Automatic shrink succeeds through the Storage API.
#   2. Storage API fails and DiskPart fallback succeeds.
#   3. Both resize methods fail.
#   4. Actual reduction within the 10 MiB tolerance accepted.
#   5. Exact lower tolerance boundary (requested - 10 MiB) accepted.
#   6. Exact upper tolerance boundary (requested + 10 MiB) accepted.
#   7. Actual reduction > 10 MiB below requested rejected.
#   8. Actual reduction > 10 MiB above requested rejected.
#   9. Verification cannot re-find the original partition rejected (no DiskPart retry).
#  10. Zero/negative shrink amount rejected (no-shrink path safety).
#  11. Shrink below the filesystem-supported minimum rejected.
#  12. DiskPart fallback succeeds but post-shrink verification fails; no second DiskPart attempt.
#  13. Partition-number mismatch at verification rejected.
#  14. Unexpected disk identity at initial query rejected.
#  15. Missing / invalid partition identity rejected; no stale drive-letter fallback.
#  16. Missing DiskPart command handled safely; temp script cleaned up.
#  17. Failure to write the temporary script handled safely; temp script cleaned up.
#  18. Temporary-script cleaned up after DiskPart success.
#  19. DiskPart exit code zero without the expected success output rejected.
#  20. Null or invalid partition-size values rejected.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if (-not $env:TEMP) { $env:TEMP = '/tmp' }

$script:testsPassed = 0
$script:testsFailed = 0
$script:failures = New-Object System.Collections.Generic.List[object]
$script:logEntries = New-Object System.Collections.Generic.List[object]

function Write-LogMessage {
    [CmdletBinding()]
    param([string]$Message, [switch]$IsError)
    $script:logEntries.Add([PSCustomObject]@{ Message = $Message; IsError = [bool]$IsError })
}

function Get-LogMessage {
    [CmdletBinding()]
    param([switch]$IsError)
    $script:logEntries | Where-Object { $_.IsError -eq [bool]$IsError } | ForEach-Object { $_.Message }
}

# ---------- Configurable fake disk state ----------
$script:fakePartSize = [int64](100GB)      # current size of the target partition
$script:fakePartNumber = 1
$script:fakeDiskNumber = 0
$script:fakeDriveLetter = 'C'
$script:sizeMin = [int64](10GB)            # Get-PartitionSupportedSize.SizeMin
$script:sizeMax = [int64](200GB)
$script:resizeShouldFail = $false
$script:resizeResultSize = $null           # if set, Resize-Partition forces the resulting size
$script:resizeRequestedSize = $null
$script:getPartitionShouldFail = $false
$script:getPartitionShouldReturnNull = $false
$script:getPartitionVerificationShouldReturnNull = $false
$script:getPartitionVerificationPartitionNumberOverride = $null
$script:getPartitionVerificationDiskNumberOverride = $null
$script:getPartitionSupportedSizeNull = $false
$script:diskpartExitCode = 0
$script:diskpartOutputSuccess = $true      # whether diskpart output contains "successfully"
$script:diskpartCommandMissing = $false
$script:diskpartInvoked = $false
$script:diskpartCallCount = 0
$script:lastDiskpartScriptPath = $null

function Get-Partition {
    [CmdletBinding()]
    param([string]$DriveLetter, [int]$DiskNumber, [int]$PartitionNumber)
    if ($script:getPartitionShouldFail) { throw "Get-Partition mock failure" }
    if ($script:getPartitionShouldReturnNull) { return $null }
    $dl = $script:fakeDriveLetter
    $dn = $script:fakeDiskNumber
    $pn = $script:fakePartNumber
    $sz = $script:fakePartSize
    # Verification re-queries by DiskNumber + PartitionNumber; apply identity overrides
    # to simulate the returned object differing from the requested identity.
    if ($PSBoundParameters.ContainsKey('PartitionNumber')) {
        if ($script:getPartitionVerificationShouldReturnNull) { return $null }
        if ($null -ne $script:getPartitionVerificationPartitionNumberOverride) {
            $pn = $script:getPartitionVerificationPartitionNumberOverride
        }
        if ($null -ne $script:getPartitionVerificationDiskNumberOverride) {
            $dn = $script:getPartitionVerificationDiskNumberOverride
        }
        if ($PSBoundParameters.ContainsKey('DriveLetter') -and $DriveLetter -ne $dl) { return $null }
        return [PSCustomObject]@{
            DiskNumber      = $dn
            PartitionNumber = $pn
            Size            = $sz
            Offset          = [int64](1MB)
            DriveLetter     = $dl
            Type            = 'Basic'
        }
    }
    # Initial lookup by drive letter.
    if ($PSBoundParameters.ContainsKey('DriveLetter') -and $DriveLetter -ne $dl) { return $null }
    if ($PSBoundParameters.ContainsKey('DiskNumber') -and $DiskNumber -ne $dn) { return $null }
    return [PSCustomObject]@{
        DiskNumber      = $dn
        PartitionNumber = $pn
        Size            = $sz
        Offset          = [int64](1MB)
        DriveLetter     = $dl
        Type            = 'Basic'
    }
}

function Get-PartitionSupportedSize {
    [CmdletBinding()]
    param([string]$DriveLetter)
    if ($script:getPartitionSupportedSizeNull) { return $null }
    [PSCustomObject]@{ SizeMin = $script:sizeMin; SizeMax = $script:sizeMax }
}

function Resize-Partition {
    [CmdletBinding()]
    param([string]$DriveLetter, [int64]$Size)
    if ($script:resizeShouldFail) { throw "Storage API mock failure: module could not be loaded" }
    $script:resizeRequestedSize = $Size
    if ($null -ne $script:resizeResultSize) {
        $script:fakePartSize = $script:resizeResultSize   # simulate actual size differing from request
    } else {
        $script:fakePartSize = $Size
    }
}

function diskpart {
    [CmdletBinding()]
    param([string]$s, [string]$ScriptPath)
    $script:diskpartInvoked = $true
    $script:diskpartCallCount++
    $script:lastDiskpartScriptPath = $ScriptPath
    if ($script:diskpartCommandMissing) {
        throw "The term 'diskpart' is not recognized as the name of a cmdlet"
    }
    # Simulate diskpart applying the shrink described in the script file.
    $content = Get-Content -Path $ScriptPath -Raw -ErrorAction SilentlyContinue
    if ($content -match 'shrink desired=(\d+)') {
        $sizeMB = [int64]$matches[1]
        $script:fakePartSize = $script:fakePartSize - ($sizeMB * 1MB)
    }
    # Simulate the exit code real diskpart reports via $LASTEXITCODE.
    $script:LASTEXITCODE = $script:diskpartExitCode
    if ($script:diskpartOutputSuccess) {
        Write-Output "Volume $($script:fakeDriveLetter): successfully shrunk"
    } else {
        Write-Output "DiskPart encountered an error: the operation failed"
    }
}

# ---------- Load the production Shrink-UllPartition from the source file ----------
$sourcePath = Join-Path $PSScriptRoot "ulli-windows.ps1"
$source = Get-Content -Path $sourcePath -Raw
$marker = 'function Shrink-UllPartition {'
$idx = $source.IndexOf($marker)
if ($idx -lt 0) { throw "Could not find '$marker' in $sourcePath" }
$i = $idx + $marker.Length          # char right after the opening '{'
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
if ($depth -ne 0) { throw "Brace mismatch while extracting Shrink-UllPartition (depth $depth)" }
$funcBody = $sb.ToString()
# Dot-scope so the function sees the script-scoped test doubles above.
. ([scriptblock]::Create($funcBody))

# ---------- Test harness ----------
function Reset-State {
    $script:logEntries.Clear()
    $script:fakePartSize = [int64](100GB)
    $script:fakePartNumber = 1
    $script:fakeDiskNumber = 0
    $script:fakeDriveLetter = 'C'
    $script:sizeMin = [int64](10GB)
    $script:sizeMax = [int64](200GB)
    $script:resizeShouldFail = $false
    $script:resizeResultSize = $null
    $script:resizeRequestedSize = $null
    $script:getPartitionShouldFail = $false
    $script:getPartitionShouldReturnNull = $false
    $script:getPartitionVerificationShouldReturnNull = $false
    $script:getPartitionVerificationPartitionNumberOverride = $null
    $script:getPartitionVerificationDiskNumberOverride = $null
    $script:getPartitionSupportedSizeNull = $false
    $script:diskpartExitCode = 0
    $script:diskpartOutputSuccess = $true
    $script:diskpartCommandMissing = $false
    $script:diskpartInvoked = $false
    $script:diskpartCallCount = 0
    $script:lastDiskpartScriptPath = $null
}

function Assert-True {
    [CmdletBinding()]
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
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

function Invoke-ShrinkTest {
    [CmdletBinding()]
    param([string]$DriveLetter = 'C', [double]$ShrinkAmountGB = 8, [int]$TargetDiskNumber = 0)
    # Callers must have run Reset-State and configured flags first; this only invokes the SUT.
    Shrink-UllPartition -DriveLetter $DriveLetter -ShrinkAmountGB $ShrinkAmountGB -TargetDiskNumber $TargetDiskNumber
}

# ── 1. Storage API shrink succeeds, exact reduction ──────────────────────────
Run-Test "1. Storage API shrink succeeds (exact reduction, verified)" {
    Reset-State
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $true) "expected true, got $r"
    Assert-True ($script:resizeRequestedSize -eq [int64](92GB)) "requested size $($script:resizeRequestedSize)"
    Assert-True ($script:fakePartSize -eq [int64](92GB)) "actual size $($script:fakePartSize)"
    Assert-True ($script:diskpartInvoked -eq $false) "diskpart should not run when Storage API succeeds"
    Assert-LogContains "Post-shrink verified"
    Assert-LogContains "Disk 0 Partition 1"
}

# ── 2. Storage API fails, DiskPart fallback succeeds ─────────────────────────
Run-Test "2. Storage API fails, DiskPart fallback succeeds" {
    Reset-State
    $script:resizeShouldFail = $true
    $script:diskpartExitCode = 0
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $true) "expected true, got $r"
    Assert-True ($script:fakePartSize -eq [int64](92GB)) "actual size after diskpart $($script:fakePartSize)"
    Assert-True ($script:diskpartInvoked -eq $true) "diskpart should run when Storage API fails"
    Assert-LogContains "Trying diskpart method"
    Assert-LogContains "Post-shrink verified"
}

# ── 3. Both resize methods fail ──────────────────────────────────────────────
Run-Test "3. Both resize methods fail" {
    Reset-State
    $script:resizeShouldFail = $true
    $script:diskpartExitCode = 1
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "Failed to shrink" -IsError
}

# ── 4. Actual reduction within the 10 MiB tolerance (undersize by 3 MiB) ─────
Run-Test "4. Actual reduction within tolerance (undersize by 3 MiB) accepted" {
    Reset-State
    $script:resizeResultSize = [int64](92GB + 3MB)
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $true) "expected true, got $r"
    $actualReduction = [int64](100GB) - $script:fakePartSize
    Assert-True ($actualReduction -ge ([int64](8GB) - [int64](10MB))) "reduction $actualReduction below lower bound"
    Assert-True ($actualReduction -le ([int64](8GB) + [int64](10MB))) "reduction $actualReduction above upper bound"
}

# ── 5. Exact lower boundary: reduction == requested - 10 MiB accepted ────────
Run-Test "5. Exact lower tolerance boundary (requested - 10 MiB) accepted" {
    Reset-State
    $script:resizeResultSize = [int64](92GB + 10MB)   # reduction = 8 GB - 10 MiB
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $true) "expected true at lower boundary, got $r"
}

# ── 6. Exact upper boundary: reduction == requested + 10 MiB accepted ────────
Run-Test "6. Exact upper tolerance boundary (requested + 10 MiB) accepted" {
    Reset-State
    $script:resizeResultSize = [int64](92GB - 10MB)   # reduction = 8 GB + 10 MiB
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $true) "expected true at upper boundary, got $r"
}

# ── 7. Actual reduction > 10 MiB below requested rejected ────────────────────
Run-Test "7. Actual reduction more than 10 MiB below requested rejected" {
    Reset-State
    $script:resizeResultSize = [int64](92GB + 15MB)   # reduction = 8 GB - 15 MiB
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "Post-shrink verification failed" -IsError
    Assert-LogContains "signed discrepancy" -IsError
}

# ── 8. Actual reduction > 10 MiB above requested rejected ────────────────────
Run-Test "8. Actual reduction more than 10 MiB above requested rejected" {
    Reset-State
    $script:resizeResultSize = [int64](92GB - 15MB)   # reduction = 8 GB + 15 MiB
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "Post-shrink verification failed" -IsError
    Assert-True ($script:diskpartInvoked -eq $false) "diskpart must not run on the Storage API path"
}

# ── 9. Verification cannot re-find the original partition rejected ───────────
Run-Test "9. Verification cannot re-find original partition rejected" {
    Reset-State
    $script:getPartitionVerificationShouldReturnNull = $true
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "could not re-query partition" -IsError
}

# ── 10. Zero/negative shrink amount rejected ─────────────────────────────────
Run-Test "10. Zero or negative shrink amount rejected" {
    Reset-State
    $r1 = Invoke-ShrinkTest -ShrinkAmountGB 0
    Assert-True ($r1 -eq $false) "zero shrink should be false, got $r1"
    $r2 = Invoke-ShrinkTest -ShrinkAmountGB -1
    Assert-True ($r2 -eq $false) "negative shrink should be false, got $r2"
    Assert-LogContains "must be greater than zero" -IsError
}

# ── 11. Shrink below the filesystem-supported minimum rejected ───────────────
Run-Test "11. Shrink below supported minimum rejected" {
    Reset-State
    # currentSize 100 GB, SizeMin 10 GB; shrink 95 GB -> newSize 5 GB < 10 GB
    $r = Invoke-ShrinkTest -ShrinkAmountGB 95
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "below the supported minimum" -IsError
}

# ── 12. DiskPart succeeds but post-shrink verification fails; no retry ───────
Run-Test "12. DiskPart succeeds but verification fails; no second DiskPart attempt" {
    Reset-State
    $script:resizeShouldFail = $true
    $script:diskpartExitCode = 0
    $script:getPartitionVerificationShouldReturnNull = $true
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-True ($script:diskpartCallCount -eq 1) "diskpart invoked $($script:diskpartCallCount) times, expected 1"
    Assert-LogContains "could not re-query partition" -IsError
}

# ── 13. Partition-number mismatch at verification rejected ───────────────────
Run-Test "13. Verification partition-number mismatch rejected" {
    Reset-State
    # Verification returns a partition whose number differs from the original.
    $script:getPartitionVerificationPartitionNumberOverride = 99
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "does not match the original Partition" -IsError
}

# ── 14. Unexpected disk identity at initial query rejected ───────────────────
Run-Test "14. Partition on wrong disk rejected" {
    Reset-State
    $script:fakeDiskNumber = 0
    $r = Invoke-ShrinkTest -TargetDiskNumber 1 -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "not on the approved target Disk" -IsError
}

# ── 15. Missing / invalid partition identity rejected; no drive-letter fallback ─
Run-Test "15. Missing partition identity rejected" {
    Reset-State
    $script:getPartitionShouldReturnNull = $true
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "returned no object" -IsError
}

Run-Test "15b. Invalid partition number rejected (no drive-letter-only fallback)" {
    Reset-State
    $script:fakePartNumber = 0
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "unable to establish a valid partition number" -IsError
}

# ── 16. Missing DiskPart command handled safely; temp script cleaned up ──────
Run-Test "16. Missing DiskPart command handled safely" {
    Reset-State
    $script:resizeShouldFail = $true
    $script:diskpartCommandMissing = $true
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "DiskPart fallback failed" -IsError
    # Temp script written by Out-File must be removed by the finally block.
    Assert-True ((Test-Path $script:lastDiskpartScriptPath) -eq $false) "temp script not cleaned up"
}

# ── 17. Failure to write the temporary script handled safely ─────────────────
Run-Test "17. Failure to write temporary script handled safely" {
    Reset-State
    $script:resizeShouldFail = $true
    $oldTemp = $env:TEMP
    try {
        $env:TEMP = '/proc/1/nope'
        $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    } finally {
        $env:TEMP = $oldTemp
    }
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "DiskPart fallback failed" -IsError
}

# ── 18. Temporary-script cleaned up after DiskPart success ───────────────────
Run-Test "18. Temp script cleaned up after DiskPart success" {
    Reset-State
    $script:resizeShouldFail = $true
    $script:diskpartExitCode = 0
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $true) "expected true, got $r"
    Assert-True ((Test-Path $script:lastDiskpartScriptPath) -eq $false) "temp script not cleaned up after success"
}

# ── 19. DiskPart exit code zero without expected success output ──────────────
Run-Test "19. DiskPart exit code zero without success output rejected" {
    Reset-State
    $script:resizeShouldFail = $true
    $script:diskpartExitCode = 0
    $script:diskpartOutputSuccess = $false
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "Failed to shrink" -IsError
}

# ── 20. Null / invalid partition-size values rejected ────────────────────────
Run-Test "20. Null current partition size rejected" {
    Reset-State
    $script:fakePartSize = $null
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "unable to determine the current partition size" -IsError
}

Run-Test "20b. Null supported minimum size rejected" {
    Reset-State
    $script:getPartitionSupportedSizeNull = $true
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "no size range returned" -IsError
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
