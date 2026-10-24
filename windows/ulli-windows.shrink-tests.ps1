# Mock-based, non-destructive unit tests for Shrink-UllPartition in windows/ulli-windows.ps1.
# Every destructive disk command (Get-Partition, Resize-Partition, diskpart) is replaced by a
# test double, so no real disk is touched. Safe to run on any PowerShell host.
#
# Usage: pwsh -NoProfile -File windows/ulli-windows.shrink-tests.ps1
#
# Covers audit cases:
#   1. Automatic shrink succeeds through the Storage API.
#   2. Storage API fails and DiskPart succeeds.
#   3. Both resize methods fail.
#   4. Actual reduction within the 10 MiB tolerance.
#   5. Actual reduction > 10 MiB below the requested reduction (rejected).
#   6. Actual reduction > 10 MiB above the requested reduction (rejected).
#   7. Verification cannot re-find the original partition (rejected).
#   8. Zero/negative shrink amount rejected (no-shrink path safety).
#   9. Shrink below the filesystem-supported minimum rejected.
#  10. Failed verification returns $false without re-invoking DiskPart.

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
$script:resizeShouldFail = $false
$script:resizeResultSize = $null           # if set, Resize-Partition forces the resulting size
$script:resizeRequestedSize = $null
$script:getPartitionShouldFail = $false
$script:getPartitionVerificationShouldReturnNull = $false
$script:diskpartExitCode = 0
$script:diskpartInvoked = $false

function Get-Partition {
    [CmdletBinding()]
    param([string]$DriveLetter, [int]$DiskNumber, [int]$PartitionNumber)
    if ($script:getPartitionShouldFail) { throw "Get-Partition mock failure" }
    # Verification re-queries by DiskNumber + PartitionNumber.
    if ($PSBoundParameters.ContainsKey('PartitionNumber') -and $script:getPartitionVerificationShouldReturnNull) {
        return $null
    }
    if ($script:getPartitionShouldReturnNull) { return $null }
    $p = [PSCustomObject]@{
        DiskNumber      = $script:fakeDiskNumber
        PartitionNumber = $script:fakePartNumber
        Size            = $script:fakePartSize
        Offset          = [int64](1MB)
        DriveLetter     = $script:fakeDriveLetter
        Type            = 'Basic'
    }
    if ($PSBoundParameters.ContainsKey('DriveLetter') -and $DriveLetter -ne $script:fakeDriveLetter) { return $null }
    if ($PSBoundParameters.ContainsKey('DiskNumber') -and $DiskNumber -ne $script:fakeDiskNumber) { return $null }
    if ($PSBoundParameters.ContainsKey('PartitionNumber') -and $PartitionNumber -ne $script:fakePartNumber) { return $null }
    return $p
}

function Get-PartitionSupportedSize {
    [CmdletBinding()]
    param([string]$DriveLetter)
    [PSCustomObject]@{ SizeMin = $script:sizeMin; SizeMax = [int64](200GB) }
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
    # Simulate diskpart applying the shrink described in the script file.
    $content = Get-Content -Path $ScriptPath -Raw -ErrorAction SilentlyContinue
    if ($content -match 'shrink desired=(\d+)') {
        $sizeMB = [int64]$matches[1]
        $script:fakePartSize = $script:fakePartSize - ($sizeMB * 1MB)
    }
    if ($script:diskpartExitCode -eq 0) { & /bin/true 2>$null } else { & /bin/false 2>$null }
    Write-Output "Volume $($script:fakeDriveLetter): successfully shrunk"
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
    $script:resizeShouldFail = $false
    $script:resizeResultSize = $null
    $script:resizeRequestedSize = $null
    $script:getPartitionShouldFail = $false
    $script:getPartitionShouldReturnNull = $false
    $script:getPartitionVerificationShouldReturnNull = $false
    $script:diskpartExitCode = 0
    $script:diskpartInvoked = $false
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
}

# ── 2. Storage API fails, DiskPart fallback succeeds ─────────────────────────
Run-Test "2. Storage API fails, DiskPart fallback succeeds" {
    Reset-State
    $script:resizeShouldFail = $true
    $script:diskpartExitCode = 0
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $true) "expected true, got $r"
    # DiskPart shrink desired = 8 * 1024 = 8192 MiB = 8 GB
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
    # Storage reports a resulting size 3 MiB larger than requested (undersize by 3 MiB).
    $script:resizeResultSize = [int64](92GB + 3MB)
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $true) "expected true, got $r"
    $actualReduction = [int64](100GB) - $script:fakePartSize
    Assert-True ($actualReduction -ge ([int64](8GB) - [int64](10MB))) "reduction $actualReduction below lower bound"
    Assert-True ($actualReduction -le ([int64](8GB) + [int64](10MB))) "reduction $actualReduction above upper bound"
}

# ── 5. Actual reduction > 10 MiB below requested (rejected) ──────────────────
Run-Test "5. Actual reduction more than 10 MiB below requested rejected" {
    Reset-State
    $script:resizeResultSize = [int64](92GB + 15MB)   # reduction = 8 GB - 15 MiB
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "Post-shrink verification failed" -IsError
}

# ── 6. Actual reduction > 10 MiB above requested (rejected) ──────────────────
Run-Test "6. Actual reduction more than 10 MiB above requested rejected" {
    Reset-State
    $script:resizeResultSize = [int64](92GB - 15MB)   # reduction = 8 GB + 15 MiB
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "Post-shrink verification failed" -IsError
    Assert-True ($script:diskpartInvoked -eq $false) "diskpart must not be re-invoked after verification failure"
}

# ── 7. Verification cannot re-find the original partition (rejected) ─────────
Run-Test "7. Verification cannot re-find original partition rejected" {
    Reset-State
    $script:getPartitionVerificationShouldReturnNull = $true
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "could not re-query partition" -IsError
}

# ── 8. Zero/negative shrink amount rejected ──────────────────────────────────
Run-Test "8. Zero or negative shrink amount rejected" {
    Reset-State
    $r1 = Invoke-ShrinkTest -ShrinkAmountGB 0
    Assert-True ($r1 -eq $false) "zero shrink should be false, got $r1"
    $r2 = Invoke-ShrinkTest -ShrinkAmountGB -1
    Assert-True ($r2 -eq $false) "negative shrink should be false, got $r2"
    Assert-LogContains "must be greater than zero" -IsError
}

# ── 9. Shrink below the filesystem-supported minimum rejected ────────────────
Run-Test "9. Shrink below supported minimum rejected" {
    Reset-State
    # currentSize 100 GB, SizeMin 10 GB; shrink 95 GB -> newSize 5 GB < 10 GB
    $r = Invoke-ShrinkTest -ShrinkAmountGB 95
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-LogContains "below the supported minimum" -IsError
}

# ── 10. Failed verification returns false without re-invoking DiskPart ───────
Run-Test "10. Verification failure returns false without re-invoking DiskPart" {
    Reset-State
    $script:getPartitionVerificationShouldReturnNull = $true
    $script:diskpartExitCode = 0
    $r = Invoke-ShrinkTest -ShrinkAmountGB 8
    Assert-True ($r -eq $false) "expected false, got $r"
    Assert-True ($script:diskpartInvoked -eq $false) "diskpart re-invoked after verification failure"
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
