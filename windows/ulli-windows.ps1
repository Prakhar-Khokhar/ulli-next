# Linux Installer for Windows 10/11 UEFI Systems - Enhanced Edition with Auto-Restart
# PowerShell GUI Version - Fixed unit conversions for proper partition placement
# Run as Administrator: powershell -ExecutionPolicy Bypass -File linux_installer.ps1
# Distributions: Linux Mint 22.3 "Zena" (Cinnamon Edition), CachyOS Desktop, Ubuntu 24.04.4 LTS, Kubuntu 24.04.4 LTS, Debian Live 13.6.0 KDE, Fedora 43 KDE
# Optional rEFInd boot manager on a dedicated FAT32 partition with ext4 driver
#
# ─── Table of Contents ───────────────────────────────────────────────────────
#   1. Bootstrap       (auto-elevate, assemblies)
#   2. Global state    ($script: variables, constants, log file)
#   3. Distro catalog  (Load-DistroCatalog + hard-coded fallback)
#   4. UI constants    (screen size, form dimensions)
#   5. Main form       (controls, layout, event wiring)
#   6. Helpers         (Write-Log, Set-Status, Get-SelectedDistro, Get-PartitionLabel,
#                       Format-AfterLayout, Invoke-PartitionShrink, Set-UILocked)
#   7. UEFI boot       (New-UefiBootEntry, bcdedit wrappers)
#   8. Disk info       (Update-DiskInfo, Get-DiskLayoutText, Get-DiskUnallocatedGB)
#   9. Plan dialog     (Show-DiskPlan, strategy selection)
#  10. ISO / rEFInd    (Test-IsoChecksum, Save-LinuxIso, Save-Refind, Install-Refind,
#                       New-RefindPartition)
#  11. Install state   (Start-Installation - top-level state machine)
#  12. Entry point     ($form.ShowDialog())

#Requires -Version 5.1

# ─── 1. Bootstrap ─────────────────────────────────────────────────────────────
if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    try {
        Start-Process powershell.exe -ArgumentList @(
            "-ExecutionPolicy", "Bypass",
            "-File", "`"$PSCommandPath`""
        ) -Verb RunAs
    } catch {
        Write-Host "ERROR: Administrator privileges are required to run ULLI." -ForegroundColor Red
        Write-Host "Please right-click the script and select 'Run as Administrator'."
        Read-Host "Press Enter to exit"
    }
    exit
}

# Add required assemblies
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

# ─── 2. Global state ─────────────────────────────────────────────────────────
# Catalog / partition / rEFInd tunables
$script:MinPartitionSizeGB = 7
$script:MinLinuxSizeGB = 20
$script:RefindUrl = "https://sourceforge.net/projects/refind/files/0.14.2/refind-bin-0.14.2.zip/download"
$script:RefindFilename = "refind-bin-0.14.2.zip"
$script:RefindSizeMB = 100  # 100 MB FAT32 partition for rEFInd

# Named constants for the rest of the script. Pulled out so that the magic
# numbers in the plan dialog and install pipeline have a single source of truth.
$script:Const = [ordered]@{
    # Free-space headroom (GB) when validating that a strategy can fit.
    ShrinkAllCHeadroomGB       = 10   # required extra free space on C: for shrink_all
    UseFreeBootCHeadroomGB     = 10   # required extra free space on C: for use_free_boot
    OtherDriveShrinkHeadroomGB = 5    # required extra free space on shrunk non-C: volume
    GapSlackGB                 = 1    # required slack in a gap for a partition to fit
    # Restart countdown
    RestartCountdownSeconds    = 30
    # Defaults
    DefaultLinuxSizeGB         = 30
    MaxLinuxSizeFallbackGB     = 10000
    # UI pump intervals (ms) during long downloads.
    UiPumpIntervalMs           = 500
    # Sleep durations (seconds) used after partition operations.
    SleepAfterPartitionSec     = 2
    SleepForDriveLetterSec     = 3
    # Layout constants
    FormMinWidth               = 720
    FormMinHeight              = 480
    FormDefaultWidth           = 720
    FormCompactHeight          = 645
    FormTallHeight             = 665
    CompactHeightThreshold     = 900
}

# ─── 3. Distro catalog ───────────────────────────────────────────────────────
# Load the distro catalog from windows/distros.json. If the file is missing or
# malformed, fall back to the built-in catalog so the installer still works.
$script:Distros = Get-DistroCatalog

$script:IsoPath = ""
$script:CustomIsoPath = ""
$script:IsRunning = $false
$script:MaxAvailableGB = $script:Const.MaxLinuxSizeFallbackGB

# ─── Log file sink ──────────────────────────────────────────────────────────
# Per-run log file in %TEMP% for post-install debugging and user bug reports.
$script:LogFile = Join-Path $env:TEMP "ulli-install-$(Get-Date -Format 'yyyyMMdd-HHmmss').log"
$script:LogStream = $null
try {
    $script:LogStream = New-Object System.IO.StreamWriter($script:LogFile, $true)
    $script:LogStream.AutoFlush = $true
} catch {
    # Non-fatal: fall back to in-memory logging only.
    $script:LogStream = $null
}

# ─── 4. UI constants ─────────────────────────────────────────────────────────
# Detect screen resolution and adapt window size
$primaryScreen = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$scrH = $primaryScreen.Height
if ($scrH -le $script:Const.CompactHeightThreshold) {
    $formW = $script:Const.FormDefaultWidth
    $formH = [Math]::Min($scrH - 60, $script:Const.FormCompactHeight)
} else {
    $formW = $script:Const.FormDefaultWidth
    $formH = $script:Const.FormTallHeight
}

# Create main form
$form = New-Object System.Windows.Forms.Form
$form.Text = "USB-less Linux Installer for Windows"
$form.Size = New-Object System.Drawing.Size($formW, $formH)
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "Sizable"
$form.MinimumSize = New-Object System.Drawing.Size($script:Const.FormMinWidth, $script:Const.FormMinHeight)
$form.AutoScroll = $true
$form.Icon = [System.Drawing.SystemIcons]::Application

# Create fonts
$headerFont = New-Object System.Drawing.Font("Segoe UI", 16, [System.Drawing.FontStyle]::Bold)
$normalFont = New-Object System.Drawing.Font("Segoe UI", 9)
$boldFont = New-Object System.Drawing.Font("Segoe UI", 9, [System.Drawing.FontStyle]::Bold)

# Header label
$headerLabel = New-Object System.Windows.Forms.Label
$headerLabel.Text = "USB-less Linux Installer for Windows"
$headerLabel.Font = $headerFont
$headerLabel.ForeColor = [System.Drawing.Color]::FromArgb(135, 185, 74)
$headerLabel.Location = New-Object System.Drawing.Point(10, 10)
$headerLabel.Size = New-Object System.Drawing.Size(680, 30)
$headerLabel.TextAlign = "MiddleCenter"
$form.Controls.Add($headerLabel)

# Sub-header label
$subHeaderLabel = New-Object System.Windows.Forms.Label
$subHeaderLabel.Text = "Mint 22.3, Ubuntu 24.04.4, Kubuntu 24.04.4, Debian 13.3.0, or Fedora 43  |  No USB required"
$subHeaderLabel.Font = $normalFont
$subHeaderLabel.ForeColor = [System.Drawing.Color]::DimGray
$subHeaderLabel.Location = New-Object System.Drawing.Point(10, 42)
$subHeaderLabel.Size = New-Object System.Drawing.Size(680, 18)
$subHeaderLabel.TextAlign = "MiddleCenter"
$form.Controls.Add($subHeaderLabel)

# Status label
$statusLabel = New-Object System.Windows.Forms.Label
$statusLabel.Text = "Ready to install"
$statusLabel.Font = $normalFont
$statusLabel.Location = New-Object System.Drawing.Point(10, 62)
$statusLabel.Size = New-Object System.Drawing.Size(680, 20)
$statusLabel.TextAlign = "MiddleCenter"
$form.Controls.Add($statusLabel)

# Progress bar
$progressBar = New-Object System.Windows.Forms.ProgressBar
$progressBar.Location = New-Object System.Drawing.Point(10, 84)
$progressBar.Size = New-Object System.Drawing.Size(680, 14)
$progressBar.Style = "Continuous"
$form.Controls.Add($progressBar)

# ISO source group
$isoGroup = New-Object System.Windows.Forms.GroupBox
$isoGroup.Text = "Distribution"
$isoGroup.Font = $normalFont
$isoGroup.Location = New-Object System.Drawing.Point(10, 104)
$isoGroup.Size = New-Object System.Drawing.Size(680, 100)
$form.Controls.Add($isoGroup)

# ─── 5. Main form ────────────────────────────────────────────────────────────
# Distro dropdown list
$script:DistroKeys = @($script:Distros.Keys)
$distroCombo = New-Object System.Windows.Forms.ComboBox
$distroCombo.Font = $boldFont
$distroCombo.Location = New-Object System.Drawing.Point(10, 22)
$distroCombo.Size = New-Object System.Drawing.Size(650, 24)
$distroCombo.ForeColor = [System.Drawing.Color]::Black
$distroCombo.DropDownStyle = [System.Windows.Forms.ComboBoxStyle]::DropDownList
foreach ($distroId in $script:DistroKeys) {
    $distroCombo.Items.Add($script:Distros[$distroId].RadioLabel) | Out-Null
}
$distroCombo.SelectedIndex = 0
$isoGroup.Controls.Add($distroCombo)

# Show informative warning when Fedora is selected because Anaconda treats the Live partition's disk as installer media
$distroCombo.Add_SelectedIndexChanged({
    try {
        $idx = $distroCombo.SelectedIndex
        if ($idx -ge 0 -and $idx -lt $script:DistroKeys.Count) {
            $selected = $script:Distros[$script:DistroKeys[$idx]]
            if ($selected.Keyword -eq "Fedora") {
                $msg = "Important: Fedora's installer (Anaconda) treats the disk that contains the Live Linux partition as the installer media and will refuse to install onto that same disk.\n\nIf you intend to install Fedora, use a separate physical disk (select the other disk in ULLI) or use Fedora as a live-only environment."
                [System.Windows.Forms.MessageBox]::Show($msg, "Fedora installation warning", [System.Windows.Forms.MessageBoxButtons]::OK, [System.Windows.Forms.MessageBoxIcon]::Warning) | Out-Null
            }
        }
    } catch {
        # Don't let UI warnings break the app; log and continue
        Write-Log "Error showing Fedora warning: $_" -Error
    }
})

# Custom ISO checkbox
$customRadio = New-Object System.Windows.Forms.CheckBox
$customRadio.Text = "Use existing ISO file:"
$customRadio.Font = $normalFont
$customRadio.Location = New-Object System.Drawing.Point(10, 56)
$customRadio.Size = New-Object System.Drawing.Size(160, 20)
$isoGroup.Controls.Add($customRadio)

# Custom ISO path textbox
$customIsoTextbox = New-Object System.Windows.Forms.TextBox
$customIsoTextbox.Font = $normalFont
$customIsoTextbox.Location = New-Object System.Drawing.Point(172, 54)
$customIsoTextbox.Size = New-Object System.Drawing.Size(388, 24)
$customIsoTextbox.ReadOnly = $true
$customIsoTextbox.Enabled = $false
$isoGroup.Controls.Add($customIsoTextbox)

# Browse button
$browseButton = New-Object System.Windows.Forms.Button
$browseButton.Text = "Browse..."
$browseButton.Font = $normalFont
$browseButton.Location = New-Object System.Drawing.Point(568, 53)
$browseButton.Size = New-Object System.Drawing.Size(100, 26)
$browseButton.Enabled = $false
$isoGroup.Controls.Add($browseButton)

# Disk info group
$diskGroup = New-Object System.Windows.Forms.GroupBox
$diskGroup.Text = "Disk Information"
$diskGroup.Font = $normalFont
$diskGroup.Location = New-Object System.Drawing.Point(10, 214)
$diskGroup.Size = New-Object System.Drawing.Size(680, 130)
$form.Controls.Add($diskGroup)

# Disk info text
$diskInfoText = New-Object System.Windows.Forms.Label
$diskInfoText.Font = $normalFont
$diskInfoText.Location = New-Object System.Drawing.Point(10, 20)
$diskInfoText.Size = New-Object System.Drawing.Size(660, 100)
$diskGroup.Controls.Add($diskInfoText)

# Log group
$logGroup = New-Object System.Windows.Forms.GroupBox
$logGroup.Text = "Installation Log"
$logGroup.Font = $normalFont
$logGroup.Location = New-Object System.Drawing.Point(10, 353)
$logGroup.Size = New-Object System.Drawing.Size(680, 90)
$form.Controls.Add($logGroup)

# Log text box
$logBox = New-Object System.Windows.Forms.TextBox
$logBox.Multiline = $true
$logBox.ScrollBars = "Vertical"
$logBox.ReadOnly = $true
$logBox.Font = New-Object System.Drawing.Font("Consolas", 9)
$logBox.Location = New-Object System.Drawing.Point(10, 20)
$logBox.Size = New-Object System.Drawing.Size(530, 60)
$logGroup.Controls.Add($logBox)

# Copy log to clipboard
$copyLogButton = New-Object System.Windows.Forms.Button
$copyLogButton.Text = "Copy Log"
$copyLogButton.Font = $normalFont
$copyLogButton.Location = New-Object System.Drawing.Point(548, 20)
$copyLogButton.Size = New-Object System.Drawing.Size(122, 28)
$logGroup.Controls.Add($copyLogButton)

# Open log folder in Explorer
$openLogButton = New-Object System.Windows.Forms.Button
$openLogButton.Text = "Open Folder"
$openLogButton.Font = $normalFont
$openLogButton.Location = New-Object System.Drawing.Point(548, 52)
$openLogButton.Size = New-Object System.Drawing.Size(122, 28)
$logGroup.Controls.Add($openLogButton)

# Delete ISO checkbox
$deleteIsoCheck = New-Object System.Windows.Forms.CheckBox
$deleteIsoCheck.Text = "Delete ISO file after installation"
$deleteIsoCheck.Font = $normalFont
$deleteIsoCheck.Location = New-Object System.Drawing.Point(10, 453)
$deleteIsoCheck.Size = New-Object System.Drawing.Size(300, 25)
$form.Controls.Add($deleteIsoCheck)

# Auto-restart checkbox
$autoRestartCheck = New-Object System.Windows.Forms.CheckBox
$autoRestartCheck.Text = "Automatically restart and configure UEFI boot"
$autoRestartCheck.Font = $normalFont
$autoRestartCheck.Location = New-Object System.Drawing.Point(10, 478)
$autoRestartCheck.Size = New-Object System.Drawing.Size(300, 25)
$autoRestartCheck.Checked = $true
$form.Controls.Add($autoRestartCheck)

# rEFInd checkbox
$refindCheck = New-Object System.Windows.Forms.CheckBox
$refindCheck.Text = "Install rEFInd boot manager  -  requires disabling Secure Boot"
$refindCheck.Font = $normalFont
$refindCheck.Location = New-Object System.Drawing.Point(10, 503)
$refindCheck.Size = New-Object System.Drawing.Size(680, 25)
$refindCheck.Checked = $false
$form.Controls.Add($refindCheck)

# Start button
$startButton = New-Object System.Windows.Forms.Button
$startButton.Text = "Start Installation"
$startButton.Font = $boldFont
$startButton.Location = New-Object System.Drawing.Point(390, 535)
$startButton.Size = New-Object System.Drawing.Size(140, 35)
$startButton.BackColor = [System.Drawing.Color]::FromArgb(135, 185, 74)
$startButton.ForeColor = [System.Drawing.Color]::White
$startButton.FlatStyle = "Flat"
$form.Controls.Add($startButton)

# Exit button
$exitButton = New-Object System.Windows.Forms.Button
$exitButton.Text = "Exit"
$exitButton.Font = $normalFont
$exitButton.Location = New-Object System.Drawing.Point(540, 535)
$exitButton.Size = New-Object System.Drawing.Size(140, 35)
$form.Controls.Add($exitButton)

# ─── 6. Helpers ──────────────────────────────────────────────────────────────
# HELPER FUNCTIONS
# ────────────────────────────────────────────────────────────────────────────

# Write-Log shadows a built-in PowerShell 7+ cmdlet of the same name. The
# installer targets PowerShell 5.1 (Windows 10/11 default) where the built-in
# does not exist, so the override is intentional.
function Write-Log {
    [CmdletBinding()]
    param(
        [Parameter(Position=0)]
        [string]$Message,
        [switch]$IsError,
        [ValidateSet('Info','Warn','Error')]
        [string]$Severity = 'Info'
    )

    $timestamp = Get-Date -Format "HH:mm:ss"
    $fullMessage = "[$timestamp] $Message"

    $logBox.AppendText("$fullMessage`r`n")
    $logBox.SelectionStart = $logBox.TextLength
    $logBox.ScrollToCaret()

    # Back-compat: $IsError implies Severity=Error unless caller overrode it.
    $effectiveSeverity = if ($PSBoundParameters.ContainsKey('Severity')) { $Severity } elseif ($IsError) { 'Error' } else { 'Info' }
    $hostColor = switch ($effectiveSeverity) {
        'Error' { 'Red' }
        'Warn'  { 'Yellow' }
        default { 'Cyan' }
    }
    Write-Host $fullMessage -ForegroundColor $hostColor

    # Mirror to log file (best-effort: never let a logging failure crash the run).
    if ($script:LogStream) {
        try {
            $script:LogStream.WriteLine("[$effectiveSeverity] $fullMessage")
        } catch {
            # Drop the stream silently on any IO error.
            try { $script:LogStream.Dispose() } catch {}
            $script:LogStream = $null
        }
    }
}

function Set-Status {
    param([string]$Status)
    $statusLabel.Text = $Status
    $form.Refresh()
}

# ─── Distro catalog loader (called from section 3) ──────────────────────────
function Get-DistroCatalog {
    # 1. Try distros.json next to this script (release layout: the .json sits
    #    in the same directory as the .ps1).
    $candidates = @(
        (Join-Path $PSScriptRoot "distros.json"),
        (Join-Path (Split-Path -Parent $PSScriptRoot) "distros.json")
    )
    foreach ($catalogPath in $candidates) {
        if (Test-Path -LiteralPath $catalogPath) {
            try {
                $raw = Get-Content -LiteralPath $catalogPath -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
                $ordered = [ordered]@{}
                foreach ($prop in $raw.PSObject.Properties) {
                    $e = $prop.Value
                    # Translate shared-schema keys to Windows-specific names.
                    $ordered[$prop.Name] = @{
                        Name           = if ($e.PSObject.Properties.Name -contains 'Name') { $e.Name } else { $e.label }
                        RadioLabel     = $e.label
                        ExpectedSize   = if ($e.PSObject.Properties.Name -contains 'ExpectedSize') { $e.ExpectedSize } else { '' }
                        Mirrors        = @($e.mirrors)
                        Checksum       = $e.sha256
                        IsoFilename    = $e.filename
                        DownloadPage   = $e.download_page
                        DownloadMsg    = $e.download_msg
                        Keyword        = $e.keyword
                        ValidationFile = $e.validation_file
                        IsHybrid       = [bool]$e.is_hybrid
                    }
                }
                if ($ordered.Count -gt 0) {
                    Write-Host "Loaded $($ordered.Count) distros from $catalogPath"
                    return $ordered
                }
            } catch {
                Write-Host "WARN: failed to parse $catalogPath ($($_.Exception.Message)). Trying next location."
            }
        }
    }
    # 2. Built-in fallback (mirror of distros.json - keep in sync).
    return Get-FallbackDistroCatalog
}

function Get-FallbackDistroCatalog {
    return [ordered]@{
        mint = @{
            Name          = "Linux Mint 22.3"
            RadioLabel    = 'Linux Mint 22.3 "Zena" - Cinnamon Edition (approx. 2.9 GB)'
            ExpectedSize  = "approximately 2.9 GB"
            Mirrors       = @(
                "https://mirrors.kernel.org/linuxmint/stable/22.3/linuxmint-22.3-cinnamon-64bit.iso",
                "https://mirror.csclub.uwaterloo.ca/linuxmint/stable/22.3/linuxmint-22.3-cinnamon-64bit.iso",
                "https://mirrors.seas.harvard.edu/linuxmint/stable/22.3/linuxmint-22.3-cinnamon-64bit.iso",
                "https://mirror.arizona.edu/linuxmint/stable/22.3/linuxmint-22.3-cinnamon-64bit.iso"
            )
            Checksum       = "a081ab202cfda17f6924128dbd2de8b63518ac0531bcfe3f1a1b88097c459bd4"
            IsoFilename    = "linuxmint-22.3-cinnamon-64bit.iso"
            DownloadPage   = "https://linuxmint.com/edition.php?id=326"
            DownloadMsg    = "Please download Linux Mint 22.3 Cinnamon (64-bit) and save it as:"
            Keyword        = "Mint"
            ValidationFile = "casper\vmlinuz"
            IsHybrid       = $false
        }
        cachyos = @{
            Name          = "CachyOS Desktop"
            RadioLabel    = "CachyOS Desktop (approx. 3 GB)"
            ExpectedSize  = "approximately 3 GB"
            Mirrors       = @(
                "https://cdn77.cachyos.org/ISO/desktop/260809/cachyos-desktop-linux-260809.iso"
            )
            Checksum       = "959f6577f45e25ee9fd8c220fd221b08e4ea79412c7315c0f922dd6d86d5e33c"
            IsoFilename    = "cachyos-desktop-linux-260809.iso"
            DownloadPage   = "https://cachyos.org/download/"
            DownloadMsg    = "Please download CachyOS Desktop and save it as:"
            Keyword        = "CachyOS"
            ValidationFile = "arch\boot\x86_64\vmlinuz-linux-cachyos"
            IsHybrid       = $true
        }
        ubuntu = @{
            Name          = "Ubuntu 24.04.4 LTS"
            RadioLabel    = "Ubuntu 24.04.4 LTS - GNOME Edition (approx. 6.2GB)"
            ExpectedSize  = "approximately 6.2 GB"
            Mirrors       = @(
                "https://releases.ubuntu.com/24.04.4/ubuntu-24.04.4-desktop-amd64.iso",
                "https://mirror.cs.uchicago.edu/ubuntu-releases/24.04.4/ubuntu-24.04.4-desktop-amd64.iso",
                "https://mirrors.mit.edu/ubuntu-releases/24.04.4/ubuntu-24.04.4-desktop-amd64.iso",
                "https://ubuntu.osuosl.org/ubuntu-releases/24.04.4/ubuntu-24.04.4-desktop-amd64.iso"
            )
            Checksum       = "3a4c9877b483ab46d7c3fbe165a0db275e1ae3cfe56a5657e5a47c2f99a99d1e"
            IsoFilename    = "ubuntu-24.04.4-desktop-amd64.iso"
            DownloadPage   = "https://ubuntu.com/download/desktop"
            DownloadMsg    = "Please download Ubuntu 24.04.4 LTS (64-bit) and save it as:"
            Keyword        = "Ubuntu"
            ValidationFile = "casper\vmlinuz"
            IsHybrid       = $false
        }
        kubuntu = @{
            Name          = "Kubuntu 24.04.4 LTS"
            RadioLabel    = "Kubuntu 24.04.4 LTS - KDE Plasma 5 Edition (approx. 4.5 GB)"
            ExpectedSize  = "approximately 4.5 GB"
            Mirrors       = @(
                "https://cdimage.ubuntu.com/kubuntu/releases/24.04.4/release/kubuntu-24.04.4-desktop-amd64.iso",
                "https://mirror.netzwerge.de/ubuntu-dvd/kubuntu/releases/24.04/release/kubuntu-24.04.4-desktop-amd64.iso",
                "https://ftpmirror.your.org/pub/ubuntu/cdimage/kubuntu/releases/24.04/release/kubuntu-24.04.4-desktop-amd64.iso",
                "https://www.mirrorservice.org/sites/cdimage.ubuntu.com/cdimage/kubuntu/releases/24.04/release/kubuntu-24.04.4-desktop-amd64.iso"
            )
            Checksum       = "02cda2568cb96c090b0438a31a7d2e7b07357fde16217c215e7c3f45263bcc49"
            IsoFilename    = "kubuntu-24.04.4-desktop-amd64.iso"
            DownloadPage   = "https://kubuntu.org/getkubuntu/"
            DownloadMsg    = "Please download Kubuntu 24.04.4 LTS (64-bit) and save it as:"
            Keyword        = "Kubuntu"
            ValidationFile = "casper\vmlinuz"
            IsHybrid       = $false
        }
        debian = @{
            Name          = "Debian Live 13.6.0 KDE"
            RadioLabel    = "Debian Live 13.6.0 - KDE Edition (approx. 3.9 GB)"
            ExpectedSize  = "approximately 3.9 GB"
            Mirrors       = @(
                "https://cdimage.debian.org/debian-cd/current-live/amd64/iso-hybrid/debian-live-13.6.0-amd64-kde.iso",
                "https://mirrors.edge.kernel.org/debian-cd/current-live/amd64/iso-hybrid/debian-live-13.6.0-amd64-kde.iso",
                "https://mirror.csclub.uwaterloo.ca/debian-cd/current-live/amd64/iso-hybrid/debian-live-13.6.0-amd64-kde.iso"
            )
            Checksum       = "426984f7edf034f4cd49f6218e706a6086588359d34fa0328676451b4a679639"
            IsoFilename    = "debian-live-13.6.0-amd64-kde.iso"
            DownloadPage   = "https://www.debian.org/CD/live/"
            DownloadMsg    = "Please download Debian Live 13.6.0 KDE (amd64) and save it as:"
            Keyword        = "Debian"
            ValidationFile = "live\vmlinuz"
            IsHybrid       = $true
        }
        fedora = @{
            Name          = "Fedora 43 KDE"
            RadioLabel    = "Fedora 43 - KDE Plasma Desktop (approx. 3.0 GB)"
            ExpectedSize  = "approximately 3.0 GB"
            Mirrors       = @(
                "https://mirror.telepoint.bg/fedora/releases/43/KDE/x86_64/iso/Fedora-KDE-Desktop-Live-43-1.6.x86_64.iso",
                "https://mirrors.netix.net/fedora/linux/releases/43/KDE/x86_64/iso/Fedora-KDE-Desktop-Live-43-1.6.x86_64.iso"
            )
            Checksum       = "181fe3e265fb5850c929f5afb7bdca91bb433b570ef39ece4a7076187435fdab"
            IsoFilename    = "Fedora-KDE-Desktop-Live-43-1.6.x86_64.iso"
            DownloadPage   = "https://fedoraproject.org/kde/download/"
            DownloadMsg    = "Please download Fedora 43 KDE Plasma Desktop (x86_64) and save it as:"
            Keyword        = "Fedora"
            ValidationFile = "LiveOS\squashfs.img"
            IsHybrid       = $true
        }
    }
}

function Get-SelectedDistro {
    if ($customRadio.Checked) {
        $isoSource = if ($script:CustomIsoPath) { $script:CustomIsoPath } else { $script:IsoPath }
        if (-not $isoSource) {
            return $null
        }
        return @{
            Name        = [System.IO.Path]::GetFileNameWithoutExtension($isoSource)
            IsoFilename = [System.IO.Path]::GetFileName($isoSource)
            Custom      = $true
        }
    }
    else {
        $idx = $distroCombo.SelectedIndex
        if ($idx -ge 0 -and $idx -lt $script:DistroKeys.Count) {
            return $script:Distros[$script:DistroKeys[$idx]]
        }
    }
    return $script:Distros["mint"]
}

function Get-PartitionLabel {
    param($Part)
    if ($Part.DriveLetter -eq 'C') {
        return "C: (Windows/NTFS)    "
    } elseif ($Part.DriveLetter) {
        return "$($Part.DriveLetter): drive               "
    } elseif ($Part.Type -eq "Recovery" -or $Part.GptType -match "de94bba4") {
        return "Recovery             "
    } elseif ($Part.IsSystem) {
        return "EFI System (ESP)     "
    } elseif ($Part.GptType -match "e3c9e316") {
        return "Microsoft Reserved   "
    } else {
        return "Partition            "
    }
}

function Format-AfterLayout {
    param(
        [array]$Partitions,
        [string]$DistroName,
        [int]$BootPartSizeGB,
        [int]$LinuxSizeGB = 0,
        [string]$ShrinkLetter = $null,
        [double]$NewShrinkSizeGB = 0,
        [switch]$ShrinkLinuxOnly,
        [switch]$AppendLinuxAndBoot,
        [double]$RemainingFreeGB = 0,
        [switch]$ShowUnchanged,
        [switch]$NoChanges,
        [switch]$UseRefind
    )
    $lines = @()
    foreach ($part in $Partitions) {
        $sGB = [math]::Round($part.Size / 1GB, 2)

        if ($ShrinkLetter -and $part.DriveLetter -eq $ShrinkLetter) {
            $label = Get-PartitionLabel -Part $part
            $lines += "  $label $NewShrinkSizeGB GB  (shrunk)"
            $lines += "  [Unallocated - Linux]  $LinuxSizeGB GB  <-- for Linux installer"
            if (-not $ShrinkLinuxOnly) {
                $lines += "  LINUX_LIVE (FAT32)     $BootPartSizeGB GB  <-- $DistroName live boot"
            }
            if ($UseRefind) {
                $lines += "  REFIND (FAT32)         0.1 GB  <-- rEFInd boot manager"
            }
            continue
        }

        $label = Get-PartitionLabel -Part $part
        $suffix = if ($ShowUnchanged -and $part.DriveLetter) { "  (unchanged)" }
                  elseif ($NoChanges -and $part.DriveLetter) { "  (unchanged)" }
                  else { "" }
        $lines += "  $label $sGB GB$suffix"
    }

    if ($AppendLinuxAndBoot) {
        if ($RemainingFreeGB -gt 0) {
            $lines += "  [Unallocated - Linux]  $RemainingFreeGB GB  <-- for Linux installer"
        }
        $lines += "  LINUX_LIVE (FAT32)     $BootPartSizeGB GB  <-- $DistroName live boot"
        if ($UseRefind) {
            $lines += "  REFIND (FAT32)         0.1 GB  <-- rEFInd boot manager"
        }
    }

    if ($NoChanges) {
        $lines += ""
        $lines += "  (No changes - disk cannot be used as-is)"
    }

    return $lines
}

function Invoke-PartitionShrink {
    param(
        [string]$DriveLetter,
        [double]$ShrinkAmountGB
    )
    try {
        $currentSize = (Get-Partition -DriveLetter $DriveLetter).Size
        $newSize = $currentSize - ($ShrinkAmountGB * 1GB)
        Resize-Partition -DriveLetter $DriveLetter -Size $newSize -ErrorAction Stop
        Write-Log "${DriveLetter}: partition shrunk successfully!"
        return $true
    }
    catch {
        Write-Log "Trying diskpart method..."
        $sizeMB = [int]($ShrinkAmountGB * 1024)
        $diskpartScript = @"
select volume $DriveLetter
shrink desired=$sizeMB
exit
"@
        $scriptPath = Join-Path $env:TEMP "shrink_script.txt"
        $diskpartScript | Out-File -FilePath $scriptPath -Encoding ASCII

        $result = diskpart /s $scriptPath
        Remove-Item $scriptPath -Force

        if ($result -match "successfully") {
            Write-Log "${DriveLetter}: partition shrunk successfully!"
            return $true
        } else {
            $hint = if ($DriveLetter -eq 'C') {
                "You may need to: 1) Run disk cleanup 2) Disable hibernation (powercfg -h off) 3) Reboot"
            } else {
                "You may need to: 1) Run disk cleanup 2) Defragment the drive 3) Reboot"
            }
            Write-Log "Failed to shrink ${DriveLetter}: partition!" -Error
            Write-Log $hint -Error
            return $false
        }
    }
}

# Log-button click handlers (UI)
$copyLogButton.Add_Click({
    try {
        if ($script:LogFile -and (Test-Path $script:LogFile)) {
            $content = Get-Content -Path $script:LogFile -Raw -ErrorAction Stop
            [System.Windows.Forms.Clipboard]::SetText($content)
            $copyLogButton.Text = "Copied!"
            $copyLogButton.Enabled = $false
            $copyTimer = New-Object System.Windows.Forms.Timer
            $copyTimer.Interval = 1500
            $copyTimer.Add_Tick({
                $copyLogButton.Text = "Copy Log"
                $copyLogButton.Enabled = $true
                $copyTimer.Stop()
                $copyTimer.Dispose()
            })
            $copyTimer.Start()
        } else {
            [System.Windows.Forms.MessageBox]::Show(
                "Log file not available yet.`nIt is created when the installer starts logging to disk.",
                "ULLI", "OK", "Information") | Out-Null
        }
    } catch {
        [System.Windows.Forms.MessageBox]::Show(
            "Failed to copy log: $_",
            "ULLI", "OK", "Error") | Out-Null
    }
})

$openLogButton.Add_Click({
    try {
        # Ensure the file exists so Explorer highlights it.
        if ($script:LogFile -and -not (Test-Path $script:LogFile)) {
            New-Item -ItemType File -Path $script:LogFile -Force | Out-Null
        }
        Start-Process explorer.exe "/select,`"$($script:LogFile)`""
    } catch {
        Start-Process explorer.exe $env:TEMP
    }
})

# Flush & close the log stream when the form closes.
$form.Add_FormClosed({
    if ($script:LogStream) {
        try { $script:LogStream.Flush() } catch {}
        try { $script:LogStream.Dispose() } catch {}
        $script:LogStream = $null
    }
})

# ─── 7. UEFI boot ────────────────────────────────────────────────────────────
function New-UefiBootEntry {
    param(
        [string]$DistroName,
        [string]$DevicePartition,
        [string]$EfiPath
    )
    $bootCreated = $false
    try {
        $copyOutput = & bcdedit.exe /copy "{bootmgr}" /d "`"$DistroName`"" 2>&1
        $copyOutputStr = $copyOutput -join " "

        if ($copyOutputStr -match '\{[0-9a-fA-F-]+\}') {
            $newGuid = $matches[0]
            Write-Log "Created new entry: $newGuid"

            $inheritedProps = @("default", "displayorder", "toolsdisplayorder", "timeout", "resumeobject", "inherit", "locale")
            foreach ($prop in $inheritedProps) {
                Start-Process "bcdedit.exe" -ArgumentList "/deletevalue", $newGuid, $prop -Wait -NoNewWindow -ErrorAction SilentlyContinue 2>$null | Out-Null
            }

            Write-Log "Setting device=partition=$DevicePartition path=$EfiPath"

            $r1 = Start-Process "bcdedit.exe" -ArgumentList "/set", $newGuid, "device", "partition=$DevicePartition" -Wait -PassThru -NoNewWindow
            $r2 = Start-Process "bcdedit.exe" -ArgumentList "/set", $newGuid, "path", $EfiPath -Wait -PassThru -NoNewWindow
            Start-Process "bcdedit.exe" -ArgumentList "/set", $newGuid, "description", "`"$DistroName`"" -Wait -NoNewWindow -ErrorAction SilentlyContinue | Out-Null
            $r3 = Start-Process "bcdedit.exe" -ArgumentList "/set", "{fwbootmgr}", "displayorder", $newGuid, "/addfirst" -Wait -PassThru -NoNewWindow
            $r4 = Start-Process "bcdedit.exe" -ArgumentList "/set", "{fwbootmgr}", "default", $newGuid -Wait -PassThru -NoNewWindow

            if ($r1.ExitCode -eq 0 -and $r2.ExitCode -eq 0 -and $r3.ExitCode -eq 0 -and $r4.ExitCode -eq 0) {
                Write-Log "UEFI boot entry created and set as default!"
                $bootCreated = $true
            } else {
                Write-Log "Some bcdedit commands failed (exit codes: device=$($r1.ExitCode), path=$($r2.ExitCode), displayorder=$($r3.ExitCode), default=$($r4.ExitCode))" -Error
                Start-Process "bcdedit.exe" -ArgumentList "/delete", $newGuid -Wait -NoNewWindow -ErrorAction SilentlyContinue
            }
        } else {
            Write-Log "bcdedit /copy did not return a GUID: $copyOutputStr" -Error
        }
    }
    catch {
        Write-Log "Failed to create boot entry: $_" -Error
    }
    return $bootCreated
}

function Set-UILocked {
    param([bool]$Locked)
    $enabled = -not $Locked
    $startButton.Enabled = $enabled
    $exitButton.Enabled = $enabled
    $deleteIsoCheck.Enabled = $enabled
    $autoRestartCheck.Enabled = $enabled
    $customRadio.Enabled = $enabled
    $browseButton.Enabled = $enabled
    $distroCombo.Enabled = $enabled
    $refindCheck.Enabled = $enabled
}

# ─── 8. Disk info ────────────────────────────────────────────────────────────
function Update-DiskInfo {
    try {
        $cDrive = Get-Partition -DriveLetter C -ErrorAction Stop | Select-Object -First 1
        $volume = Get-Volume -DriveLetter C -ErrorAction Stop

        $partitionNumber = if ($cDrive.PartitionNumber) {
            $cDrive.PartitionNumber
        } else {
            (Get-Partition -DiskNumber $cDrive.DiskNumber | Where-Object { $_.DriveLetter -eq 'C' }).PartitionNumber
        }

        $diskInfo = @"
C: Drive Information:
Total Size: $([math]::Round($volume.Size / 1GB, 2)) GB
Free Space: $([math]::Round($volume.SizeRemaining / 1GB, 2)) GB
File System: $($volume.FileSystem)
Disk Number: $($cDrive.DiskNumber)
Partition Number: $partitionNumber
"@

        $diskInfoText.Text = $diskInfo

        $script:CDriveInfo = @{
            DiskNumber = $cDrive.DiskNumber
            PartitionNumber = $partitionNumber
            FreeGB = [math]::Round($volume.SizeRemaining / 1GB, 2)
            TotalGB = [math]::Round($volume.Size / 1GB, 2)
        }

        # Max available stored for the disk plan dialog
        $script:MaxAvailableGB = [math]::Floor($script:CDriveInfo.FreeGB - $script:MinPartitionSizeGB - 10)
    }
    catch {
        Write-Log "Error getting disk information: $_" -Error
        $diskInfoText.Text = "Error retrieving disk information"
    }
}

function Get-DiskLayoutText {
    param(
        [int]$DiskNumber
    )
    $disk = Get-Disk -Number $DiskNumber
    $partitions = Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue | Sort-Object Offset
    $lines = @()

    if (-not $partitions -or $partitions.Count -eq 0) {
        $lines += "  [Entire disk is unallocated]  $([math]::Round($disk.Size / 1GB, 2)) GB"
        return $lines
    }

    $previousEnd = [int64]0
    foreach ($part in $partitions) {
        if ($part.Offset -gt ($previousEnd + 1MB)) {
            $gapGB = [math]::Round(($part.Offset - $previousEnd) / 1GB, 2)
            if ($gapGB -gt 0.01) {
                $lines += "  [Unallocated]             $gapGB GB"
            }
        }

        $sizeGB = [math]::Round($part.Size / 1GB, 2)
        $label = Get-PartitionLabel -Part $part

        $freeNote = ""
        if ($part.DriveLetter) {
            try {
                $vol = Get-Volume -DriveLetter $part.DriveLetter -ErrorAction Stop
                if ($vol.SizeRemaining) {
                    $freeNote = "  (Free: $([math]::Round($vol.SizeRemaining / 1GB, 2)) GB)"
                }
            } catch {}
        }

        $lines += "  $label $sizeGB GB$freeNote"
        $previousEnd = $part.Offset + $part.Size
    }
    # Trailing
    if ($disk.Size -gt ($previousEnd + 1MB)) {
        $trailGB = [math]::Round(($disk.Size - $previousEnd) / 1GB, 2)
        if ($trailGB -gt 0.01) {
            $lines += "  [Unallocated]             $trailGB GB"
        }
    }

    return $lines
}

function Get-DiskUnallocatedGB {
    param(
        [int]$DiskNumber,
        [int64]$AfterOffset = 0
    )
    $disk = Get-Disk -Number $DiskNumber
    $partitions = Get-Partition -DiskNumber $DiskNumber -ErrorAction SilentlyContinue | Sort-Object Offset

    $total = [int64]0
    $previousEnd = [int64]0

    if ($partitions) {
        foreach ($part in $partitions) {
            $gap = $part.Offset - $previousEnd
            if ($gap -gt 1MB -and $previousEnd -ge $AfterOffset) {
                $total += $gap
            }
            $previousEnd = $part.Offset + $part.Size
        }
    }
    $trailing = $disk.Size - $previousEnd
    if ($trailing -gt 1MB -and $previousEnd -ge $AfterOffset) {
        $total += $trailing
    }

    return [math]::Round($total / 1GB, 2)
}

# ─── 9. Plan dialog ──────────────────────────────────────────────────────────
function Show-DiskPlan {
    param(
        [string]$DistroName
    )

    $bootPartSizeGB = $script:MinPartitionSizeGB  # 7 GB

    # ---- Enumerate all suitable disks ----
    $allDisks = Get-Disk | Where-Object {
        $_.OperationalStatus -eq 'Online' -and $_.Size -gt 10GB
    } | Sort-Object Number

    $cDiskNumber = $script:CDriveInfo.DiskNumber

    # Build dropdown items: C: disk first, then others
    $diskItems = @()
    foreach ($d in $allDisks) {
        $dSizeGB = [math]::Round($d.Size / 1GB, 1)
        $dFreeGB = Get-DiskUnallocatedGB -DiskNumber $d.Number
        $busType = if ($d.BusType) { $d.BusType } else { "Unknown" }
        $model = if ($d.Model) { $d.Model.Trim() } else { "Disk" }

        $letters = (Get-Partition -DiskNumber $d.Number -ErrorAction SilentlyContinue |
            Where-Object { $_.DriveLetter } |
            ForEach-Object { "$($_.DriveLetter):" }) -join ", "
        $letterInfo = if ($letters) { " [$letters]" } else { "" }

        $isCDisk = ($d.Number -eq $cDiskNumber)
        $prefix = if ($isCDisk) { "Disk $($d.Number) (Windows)" } else { "Disk $($d.Number)" }

        $diskItems += [PSCustomObject]@{
            Number = $d.Number
            Label = "$prefix - $model - $dSizeGB GB ($busType)$letterInfo - Free: $dFreeGB GB"
            IsCDisk = $isCDisk
            TotalGB = $dSizeGB
            FreeGB = $dFreeGB
        }
    }

    # ---- Build the dialog ----
    $planForm = New-Object System.Windows.Forms.Form
    $planForm.Text = "Disk Plan - Review Before Proceeding"
    $planForm.Size = New-Object System.Drawing.Size(720, 780)
    $planForm.StartPosition = "CenterParent"
    $planForm.FormBorderStyle = "Sizable"
    $planForm.MinimumSize = New-Object System.Drawing.Size($script:Const.FormMinWidth, $script:Const.FormMinHeight)
    $planForm.MaximizeBox = $true
    $planForm.MinimizeBox = $false
    $planForm.AutoScroll = $true

    $planFont = New-Object System.Drawing.Font("Segoe UI", 9)
    $planBoldFont = New-Object System.Drawing.Font("Segoe UI", 9, [System.Drawing.FontStyle]::Bold)
    $planHeaderFont = New-Object System.Drawing.Font("Segoe UI", 12, [System.Drawing.FontStyle]::Bold)
    $planMonoFont = New-Object System.Drawing.Font("Consolas", 9)

    $yPos = 12

    # Title
    $titleLabel = New-Object System.Windows.Forms.Label
    $titleLabel.Text = "Review Disk Changes for $DistroName"
    $titleLabel.Font = $planHeaderFont
    $titleLabel.Location = New-Object System.Drawing.Point(16, $yPos)
    $titleLabel.Size = New-Object System.Drawing.Size(670, 26)
    $planForm.Controls.Add($titleLabel)
    $yPos += 32

    # Warning banner
    $warningPanel = New-Object System.Windows.Forms.Panel
    $warningPanel.Location = New-Object System.Drawing.Point(16, $yPos)
    $warningPanel.Size = New-Object System.Drawing.Size(670, 42)
    $warningPanel.BackColor = [System.Drawing.Color]::FromArgb(255, 248, 220)
    $warningPanel.BorderStyle = "FixedSingle"
    $planForm.Controls.Add($warningPanel)

    $warningLabel = New-Object System.Windows.Forms.Label
    $warningLabel.Text = [char]0x26A0 + "  These changes modify your disk's partition table. Some options (like wipe and reformat) will DESTROY ALL DATA on the target disk. Make sure you have a backup of important files before proceeding."
    $warningLabel.Font = $planFont
    $warningLabel.Location = New-Object System.Drawing.Point(8, 4)
    $warningLabel.Size = New-Object System.Drawing.Size(650, 34)
    $warningPanel.Controls.Add($warningLabel)
    $yPos += 52

    # ---- TARGET DISK SELECTOR ----
    $diskSelectGroup = New-Object System.Windows.Forms.GroupBox
    $diskSelectGroup.Text = "Target Disk"
    $diskSelectGroup.Font = $planBoldFont
    $diskSelectGroup.Location = New-Object System.Drawing.Point(16, $yPos)
    $diskSelectGroup.Size = New-Object System.Drawing.Size(670, 56)
    $planForm.Controls.Add($diskSelectGroup)

    $diskCombo = New-Object System.Windows.Forms.ComboBox
    $diskCombo.Font = $planFont
    $diskCombo.DropDownStyle = "DropDownList"
    $diskCombo.Location = New-Object System.Drawing.Point(10, 22)
    $diskCombo.Size = New-Object System.Drawing.Size(648, 24)
    $diskSelectGroup.Controls.Add($diskCombo)

    foreach ($item in $diskItems) {
        $diskCombo.Items.Add($item.Label) | Out-Null
    }
    $cIndex = 0
    for ($i = 0; $i -lt $diskItems.Count; $i++) {
        if ($diskItems[$i].IsCDisk) { $cIndex = $i; break }
    }
    $diskCombo.SelectedIndex = $cIndex
    $yPos += 66

    # ---- LINUX PARTITION SIZE ----
    $sizeGroup = New-Object System.Windows.Forms.GroupBox
    $sizeGroup.Text = "Linux Partition Size"
    $sizeGroup.Font = $planBoldFont
    $sizeGroup.Location = New-Object System.Drawing.Point(16, $yPos)
    $sizeGroup.Size = New-Object System.Drawing.Size(670, 56)
    $planForm.Controls.Add($sizeGroup)

    $sizeLabel = New-Object System.Windows.Forms.Label
    $sizeLabel.Text = "Size for Linux (GB):"
    $sizeLabel.Font = $planFont
    $sizeLabel.Location = New-Object System.Drawing.Point(10, 24)
    $sizeLabel.Size = New-Object System.Drawing.Size(130, 20)
    $sizeGroup.Controls.Add($sizeLabel)

    $sizeNumeric = New-Object System.Windows.Forms.NumericUpDown
    $sizeNumeric.Font = $planFont
    $sizeNumeric.Location = New-Object System.Drawing.Point(145, 22)
    $sizeNumeric.Size = New-Object System.Drawing.Size(80, 24)
    $sizeNumeric.Minimum = $script:MinLinuxSizeGB
    $sizeNumeric.Maximum = if ($script:MaxAvailableGB -gt $script:MinLinuxSizeGB) { $script:MaxAvailableGB } else { $script:Const.MaxLinuxSizeFallbackGB }
    $sizeNumeric.Value = 30
    $sizeGroup.Controls.Add($sizeNumeric)

    $sizeHelpLabel = New-Object System.Windows.Forms.Label
    $sizeHelpLabel.Text = "Minimum: 20 GB, Recommended: 100+ GB"
    $sizeHelpLabel.Font = $planFont
    $sizeHelpLabel.Location = New-Object System.Drawing.Point(240, 24)
    $sizeHelpLabel.Size = New-Object System.Drawing.Size(400, 20)
    $sizeGroup.Controls.Add($sizeHelpLabel)
    $yPos += 66

    # ---- CURRENT LAYOUT ----
    $currentGroup = New-Object System.Windows.Forms.GroupBox
    $currentGroup.Text = "Current Disk Layout"
    $currentGroup.Font = $planBoldFont
    $currentGroup.Location = New-Object System.Drawing.Point(16, $yPos)
    $currentGroup.Size = New-Object System.Drawing.Size(670, 150)
    $planForm.Controls.Add($currentGroup)

    $currentText = New-Object System.Windows.Forms.TextBox
    $currentText.Multiline = $true
    $currentText.ReadOnly = $true
    $currentText.ScrollBars = "Vertical"
    $currentText.Font = $planMonoFont
    $currentText.Location = New-Object System.Drawing.Point(10, 20)
    $currentText.Size = New-Object System.Drawing.Size(648, 120)
    $currentText.BackColor = [System.Drawing.Color]::White
    $currentGroup.Controls.Add($currentText)
    $yPos += 160

    # ---- STRATEGY SELECTION ----
    $strategyGroup = New-Object System.Windows.Forms.GroupBox
    $strategyGroup.Text = "Installation Strategy"
    $strategyGroup.Font = $planBoldFont
    $strategyGroup.Location = New-Object System.Drawing.Point(16, $yPos)
    $strategyGroup.Size = New-Object System.Drawing.Size(670, 104)
    $planForm.Controls.Add($strategyGroup)

    $stratPanel = New-Object System.Windows.Forms.Panel
    $stratPanel.Location = New-Object System.Drawing.Point(10, 18)
    $stratPanel.Size = New-Object System.Drawing.Size(648, 80)
    $strategyGroup.Controls.Add($stratPanel)

    $radioShrink = New-Object System.Windows.Forms.RadioButton
    $radioShrink.Font = $planFont
    $radioShrink.Location = New-Object System.Drawing.Point(0, 0)
    $radioShrink.Size = New-Object System.Drawing.Size(640, 20)
    $radioShrink.Checked = $true
    $stratPanel.Controls.Add($radioShrink)

    $radioFreeAll = New-Object System.Windows.Forms.RadioButton
    $radioFreeAll.Font = $planFont
    $radioFreeAll.Location = New-Object System.Drawing.Point(0, 24)
    $radioFreeAll.Size = New-Object System.Drawing.Size(640, 20)
    $stratPanel.Controls.Add($radioFreeAll)

    $radioWipe = New-Object System.Windows.Forms.RadioButton
    $radioWipe.Font = $planFont
    $radioWipe.ForeColor = [System.Drawing.Color]::DarkRed
    $radioWipe.Location = New-Object System.Drawing.Point(0, 48)
    $radioWipe.Size = New-Object System.Drawing.Size(640, 20)
    $radioWipe.Visible = $false
    $stratPanel.Controls.Add($radioWipe)
    $yPos += 112

    # ---- PLANNED CHANGES ----
    $changesGroup = New-Object System.Windows.Forms.GroupBox
    $changesGroup.Text = "Planned Changes"
    $changesGroup.Font = $planBoldFont
    $changesGroup.Location = New-Object System.Drawing.Point(16, $yPos)
    $changesGroup.Size = New-Object System.Drawing.Size(670, 100)
    $planForm.Controls.Add($changesGroup)

    $changesText = New-Object System.Windows.Forms.TextBox
    $changesText.Multiline = $true
    $changesText.ReadOnly = $true
    $changesText.Font = $planMonoFont
    $changesText.Location = New-Object System.Drawing.Point(10, 20)
    $changesText.Size = New-Object System.Drawing.Size(648, 70)
    $changesText.BackColor = [System.Drawing.Color]::White
    $changesGroup.Controls.Add($changesText)
    $yPos += 110

    # ---- AFTER LAYOUT ----
    $afterGroup = New-Object System.Windows.Forms.GroupBox
    $afterGroup.Text = "Disk Layout After Changes"
    $afterGroup.Font = $planBoldFont
    $afterGroup.Location = New-Object System.Drawing.Point(16, $yPos)
    $afterGroup.Size = New-Object System.Drawing.Size(670, 130)
    $planForm.Controls.Add($afterGroup)

    $afterText = New-Object System.Windows.Forms.TextBox
    $afterText.Multiline = $true
    $afterText.ReadOnly = $true
    $afterText.ScrollBars = "Vertical"
    $afterText.Font = $planMonoFont
    $afterText.Location = New-Object System.Drawing.Point(10, 20)
    $afterText.Size = New-Object System.Drawing.Size(648, 100)
    $afterText.BackColor = [System.Drawing.Color]::White
    $afterGroup.Controls.Add($afterText)
    $yPos += 140

    # Track selected disk number and shrink info
    $script:DiskPlanStrategy = "shrink_all"
    $script:DiskPlanTargetDisk = $cDiskNumber
    $script:DiskPlanShrinkLetter = $null
    $script:DiskPlanShrinkAmount = 0

    # ---- Master update function ----
    $updateAll = {
        $selIndex = $diskCombo.SelectedIndex
        if ($selIndex -lt 0) { return }
        $selDisk = $diskItems[$selIndex]
        $selDiskNum = $selDisk.Number
        $isTargetCDisk = $selDisk.IsCDisk

        $LinuxSizeGB = [int]$sizeNumeric.Value
        $useRefind = $refindCheck.Checked
        $refindGB = if ($useRefind) { 0.1 } else { 0 }
        $totalNeededGB = $LinuxSizeGB + $bootPartSizeGB + $refindGB

        $script:DiskPlanTargetDisk = $selDiskNum

        # Update current layout text
        $layoutLines = Get-DiskLayoutText -DiskNumber $selDiskNum
        $diskObj = Get-Disk -Number $selDiskNum
        $dTotalGB = [math]::Round($diskObj.Size / 1GB, 2)
        $currentGroup.Text = "Current Disk Layout  (Disk $selDiskNum - $dTotalGB GB)"

        $totalFreeGB = Get-DiskUnallocatedGB -DiskNumber $selDiskNum
        if ($totalFreeGB -gt 0.01) {
            $layoutLines += ""
            $layoutLines += "  Total unallocated space: $totalFreeGB GB"
        }
        $currentText.Text = ($layoutLines -join "`r`n")

        if ($isTargetCDisk) {
            # ---- C: disk strategies ----
            $radioWipe.Visible = $false
            $radioWipe.Checked = $false
            $cPartition = Get-Partition -DriveLetter C
            $cSizeGB = [math]::Round($cPartition.Size / 1GB, 2)
            $cPartitionEnd = $cPartition.Offset + $cPartition.Size
            $usableFreeGB = Get-DiskUnallocatedGB -DiskNumber $selDiskNum -AfterOffset $cPartitionEnd

            $canFreeAll = ($usableFreeGB -ge ($totalNeededGB + 1))
            $canFreeBoot = ($usableFreeGB -ge ($bootPartSizeGB + $refindGB + 1))

            $refindNote = if ($useRefind) { " + rEFInd (0.1 GB)" } else { "" }
            $radioShrink.Text = "Shrink C: by $totalNeededGB GB for Linux ($LinuxSizeGB GB) + boot ($bootPartSizeGB GB)$refindNote"
            $radioShrink.Visible = $true
            $radioShrink.Enabled = $true

            if ($canFreeAll) {
                $radioFreeAll.Text = "Use existing unallocated space ($([math]::Round($usableFreeGB, 1)) GB available) - no shrink needed"
                $radioFreeAll.Visible = $true
                $radioFreeAll.Enabled = $true
            } elseif ($canFreeBoot) {
                $radioFreeAll.Text = "Use existing free space for 7 GB boot partition, shrink C: by $LinuxSizeGB GB for Linux only"
                $radioFreeAll.Visible = $true
                $radioFreeAll.Enabled = $true
            } else {
                $radioFreeAll.Visible = $false
                $radioFreeAll.Checked = $false
                if (-not $radioShrink.Checked) { $radioShrink.Checked = $true }
            }

            $strategyGroup.Visible = $true

            $strategy = "shrink_all"
            if ($radioFreeAll.Visible -and $radioFreeAll.Checked) {
                if ($canFreeAll) { $strategy = "use_free_all" }
                elseif ($canFreeBoot) { $strategy = "use_free_boot" }
            }
            $script:DiskPlanStrategy = $strategy

            $changeLines = @()
            $afterLines = @()
            $partitions = Get-Partition -DiskNumber $selDiskNum | Sort-Object Offset

            # Always resolve the selected distro first
            $distro = Get-SelectedDistro
            $distroName = if ($customRadio.Checked -and $script:CustomIsoPath) {
                [System.IO.Path]::GetFileNameWithoutExtension($script:CustomIsoPath)
            } elseif ($distro -and $distro.Name) {
                $distro.Name
            } else {
                $DistroName
            }
            switch ($strategy) {
                "shrink_all" {
                    $newCSizeGB = [math]::Round($cSizeGB - $totalNeededGB, 2)
                    $step = 1
                    $changeLines += "  $step. Shrink C: partition from $cSizeGB GB to $newCSizeGB GB  (-$totalNeededGB GB)"
                    $step++
                    $changeLines += "  $step. Create 7 GB FAT32 boot partition (LINUX_LIVE) with $distroName files"
                    if ($useRefind) {
                        $step++
                        $changeLines += "  $step. Create 100 MB FAT32 rEFInd partition"
                    }
                    $step++
                    $changeLines += "  $step. Leave $LinuxSizeGB GB unallocated for Linux installation"
                    $step++
                    if ($useRefind) {
                        $changeLines += "  $step. Install rEFInd boot manager and configure UEFI boot entry"
                    } else {
                        $changeLines += "  $step. Configure UEFI boot entry for $distroName"
                    }

                    $afterLines = Format-AfterLayout -Partitions $partitions -DistroName $distroName `
                        -BootPartSizeGB $bootPartSizeGB -LinuxSizeGB $LinuxSizeGB `
                        -ShrinkLetter 'C' -NewShrinkSizeGB $newCSizeGB -UseRefind:$useRefind
                }
                "use_free_all" {
                    $step = 1
                    $changeLines += "  $step. C: partition is NOT modified (stays at $cSizeGB GB)"
                    $step++
                    $changeLines += "  $step. Create 7 GB FAT32 boot partition (LINUX_LIVE) in existing free space"
                    if ($useRefind) {
                        $step++
                        $changeLines += "  $step. Create 100 MB FAT32 rEFInd partition"
                    }
                    $step++
                    $remainFreeGB = [math]::Round($usableFreeGB - $bootPartSizeGB - $refindGB, 1)
                    $changeLines += "  $step. Remaining ~$remainFreeGB GB stays unallocated for Linux"
                    $step++
                    if ($useRefind) {
                        $changeLines += "  $step. Install rEFInd boot manager and configure UEFI boot entry"
                    } else {
                        $changeLines += "  $step. Configure UEFI boot entry for $distroName"
                    }

                    $afterLines = Format-AfterLayout -Partitions $partitions -DistroName $distroName `
                        -BootPartSizeGB $bootPartSizeGB -ShowUnchanged -AppendLinuxAndBoot `
                        -RemainingFreeGB $remainFreeGB -UseRefind:$useRefind
                }
                "use_free_boot" {
                    $newCSizeGB = [math]::Round($cSizeGB - $LinuxSizeGB, 2)
                    $step = 1
                    $changeLines += "  $step. Shrink C: partition from $cSizeGB GB to $newCSizeGB GB  (-$LinuxSizeGB GB)"
                    $step++
                    $changeLines += "  $step. Create 7 GB FAT32 boot partition (LINUX_LIVE) in existing free space"
                    if ($useRefind) {
                        $step++
                        $changeLines += "  $step. Create 100 MB FAT32 rEFInd partition"
                    }
                    $step++
                    $changeLines += "  $step. Leave $LinuxSizeGB GB (from C: shrink) unallocated for Linux"
                    $step++
                    if ($useRefind) {
                        $changeLines += "  $step. Install rEFInd boot manager and configure UEFI boot entry"
                    } else {
                        $changeLines += "  $step. Configure UEFI boot entry for $distroName"
                    }

                    $afterLines = Format-AfterLayout -Partitions $partitions -DistroName $distroName `
                        -BootPartSizeGB $bootPartSizeGB -LinuxSizeGB $LinuxSizeGB `
                        -ShrinkLetter 'C' -NewShrinkSizeGB $newCSizeGB -ShrinkLinuxOnly
                    $afterLines += "  LINUX_LIVE (FAT32)     $bootPartSizeGB GB  <-- $distroName live boot"
                    if ($useRefind) {
                        $afterLines += "  REFIND (FAT32)         0.1 GB  <-- rEFInd boot manager"
                    }
                }
            }

            $changesText.Text = ($changeLines -join "`r`n")
            $afterText.Text = ($afterLines -join "`r`n")

        } else {
            # ---- Other disk ----
            $shrinkablePartitions = @()
            $partitions = Get-Partition -DiskNumber $selDiskNum -ErrorAction SilentlyContinue | Sort-Object Offset
            if ($partitions) {
                foreach ($part in $partitions) {
                    if ($part.DriveLetter) {
                        try {
                            $vol = Get-Volume -DriveLetter $part.DriveLetter -ErrorAction Stop
                            if ($vol.FileSystem -eq "NTFS" -and $vol.SizeRemaining -gt ($totalNeededGB * 1GB)) {
                                $shrinkablePartitions += [PSCustomObject]@{
                                    DriveLetter = $part.DriveLetter
                                    SizeGB = [math]::Round($part.Size / 1GB, 2)
                                    FreeGB = [math]::Round($vol.SizeRemaining / 1GB, 2)
                                    PartitionNumber = $part.PartitionNumber
                                }
                            }
                        } catch {}
                    }
                }
            }

            $diskFreeGB = $selDisk.FreeGB
            $hasFreeSpace = ($diskFreeGB -ge ($bootPartSizeGB + $refindGB + 1))
            $hasShrinkable = ($shrinkablePartitions.Count -gt 0)

            $nonNtfsPartitions = @()
            if ($partitions) {
                foreach ($part in $partitions) {
                    if ($part.DriveLetter) {
                        try {
                            $vol = Get-Volume -DriveLetter $part.DriveLetter -ErrorAction Stop
                            if ($vol.FileSystem -ne "NTFS" -and $vol.FileSystem) {
                                $nonNtfsPartitions += [PSCustomObject]@{
                                    DriveLetter = $part.DriveLetter
                                    FileSystem = $vol.FileSystem
                                    SizeGB = [math]::Round($part.Size / 1GB, 2)
                                }
                            }
                        } catch {}
                    }
                }
            }

            # Configure radio buttons for other-drive strategies
            if ($hasFreeSpace) {
                $radioShrink.Text = "Use existing unallocated space ($([math]::Round($diskFreeGB, 1)) GB) on Disk $selDiskNum"
                $radioShrink.Visible = $true
                $radioShrink.Enabled = $true
                if (-not $radioShrink.Checked -and -not $radioFreeAll.Checked -and -not $radioWipe.Checked) {
                    $radioShrink.Checked = $true
                }
            } else {
                $radioShrink.Visible = $false
                $radioShrink.Checked = $false
            }

            if ($hasShrinkable) {
                $bestShrink = $shrinkablePartitions | Sort-Object FreeGB -Descending | Select-Object -First 1
                $radioFreeAll.Text = "Shrink $($bestShrink.DriveLetter): ($($bestShrink.SizeGB) GB, $($bestShrink.FreeGB) GB free) on Disk $selDiskNum to make space"
                $radioFreeAll.Visible = $true
                $radioFreeAll.Enabled = $true
                if (-not $hasFreeSpace -and -not $radioWipe.Checked) {
                    $radioFreeAll.Checked = $true
                }
            } else {
                $radioFreeAll.Visible = $false
                $radioFreeAll.Checked = $false
            }

            # Always offer wipe & reformat for non-C: disks if disk is large enough
            $wipeMinGB = $bootPartSizeGB + $refindGB + 1
            $diskSizeOK = ($selDisk.TotalGB -ge $wipeMinGB)
            $radioWipe.Text = [char]0x26A0 + " Wipe & reformat entire disk ($($selDisk.TotalGB) GB) - ALL DATA ON DISK $selDiskNum WILL BE DESTROYED"
            $radioWipe.Visible = $true
            $radioWipe.Enabled = $diskSizeOK

            if (-not $hasFreeSpace -and -not $hasShrinkable) {
                if ($diskSizeOK) {
                    $radioShrink.Text = "No unallocated space or shrinkable partitions on Disk $selDiskNum"
                    $radioShrink.Visible = $true
                    $radioShrink.Enabled = $false
                    $radioShrink.Checked = $false

                    if ($nonNtfsPartitions.Count -gt 0) {
                        $fsTypes = ($nonNtfsPartitions | ForEach-Object { "$($_.DriveLetter): ($($_.FileSystem))" }) -join ", "
                        $radioFreeAll.Text = "Cannot shrink $fsTypes - only NTFS partitions can be resized by Windows"
                        $radioFreeAll.Visible = $true
                        $radioFreeAll.Enabled = $false
                        $radioFreeAll.Checked = $false
                    }

                    if (-not $radioWipe.Checked) {
                        $radioWipe.Checked = $true
                    }
                } else {
                    $radioShrink.Text = "No unallocated space or shrinkable partitions on Disk $selDiskNum"
                    $radioShrink.Visible = $true
                    $radioShrink.Enabled = $false
                    $radioShrink.Checked = $false
                    $radioFreeAll.Visible = $false

                    if ($nonNtfsPartitions.Count -gt 0) {
                        $fsTypes = ($nonNtfsPartitions | ForEach-Object { "$($_.DriveLetter): ($($_.FileSystem))" }) -join ", "
                        $radioFreeAll.Text = "Cannot shrink $fsTypes - only NTFS partitions can be resized by Windows"
                        $radioFreeAll.Visible = $true
                        $radioFreeAll.Enabled = $false
                        $radioFreeAll.Checked = $false
                    }
                }
            }

            $strategyGroup.Visible = $true

            $usingWipe = ($radioWipe.Visible -and $radioWipe.Checked)

            if ($usingWipe) {
                $usingShrink = $false
            } elseif ($hasShrinkable -and -not $hasFreeSpace -and -not $usingWipe) {
                $usingShrink = $true
            } elseif ($hasFreeSpace -and -not $hasShrinkable) {
                $usingShrink = $false
            } elseif ($hasFreeSpace -and $hasShrinkable) {
                $usingShrink = $radioFreeAll.Checked
            } else {
                $usingShrink = $false
            }

            if ($usingWipe) {
                $script:DiskPlanStrategy = "wipe_disk"
                $script:DiskPlanShrinkLetter = $null
                $script:DiskPlanShrinkAmount = 0
            } elseif ($usingShrink) {
                $script:DiskPlanStrategy = "other_drive_shrink"
                $script:DiskPlanShrinkLetter = $bestShrink.DriveLetter
                $script:DiskPlanShrinkAmount = $totalNeededGB
            } else {
                $script:DiskPlanStrategy = "other_drive"
                $script:DiskPlanShrinkLetter = $null
                $script:DiskPlanShrinkAmount = 0
            }

            $changeLines = @()
            $afterLines = @()

            if ($usingWipe) {
                $usableGB = [math]::Round($selDisk.TotalGB - $bootPartSizeGB - $refindGB, 1)

                $changeLines += "  ** WARNING: This will ERASE ALL DATA on this disk! **"
                $changeLines += ""
                $step = 1
                $changeLines += "  $step. C: partition is NOT modified (different disk)"
                $step++
                $changeLines += "  $step. Wipe Disk $selDiskNum and create a new GPT partition table"
                $step++
                $changeLines += "  $step. Create $bootPartSizeGB GB FAT32 boot partition (LINUX_LIVE)"
                if ($useRefind) {
                    $step++
                    $changeLines += "  $step. Create 100 MB FAT32 rEFInd partition"
                }
                $step++
                $changeLines += "  $step. Leave ~$usableGB GB unallocated for Linux installation"
                $step++
                if ($useRefind) {
                    $changeLines += "  $step. Install rEFInd boot manager and configure UEFI boot entry"
                } else {
                    $changeLines += "  $step. Install bootloader to Windows ESP and configure UEFI boot entry for $DistroName"
                }

                $afterLines += "  LINUX_LIVE (FAT32)     $bootPartSizeGB GB  <-- $DistroName live boot"
                if ($useRefind) {
                    $afterLines += "  REFIND (FAT32)         0.1 GB  <-- rEFInd boot manager"
                }
                $afterLines += "  [Unallocated - Linux]  ~$usableGB GB  <-- for Linux installer"
            } elseif ($usingShrink) {
                $shrinkTarget = $bestShrink
                $newPartSizeGB = [math]::Round($shrinkTarget.SizeGB - $totalNeededGB, 2)
                $step = 1
                $changeLines += "  $step. C: partition is NOT modified (different disk selected)"
                $step++
                $changeLines += "  $step. Shrink $($shrinkTarget.DriveLetter): from $($shrinkTarget.SizeGB) GB to $newPartSizeGB GB  (-$totalNeededGB GB)"
                $step++
                $changeLines += "  $step. Create 7 GB FAT32 boot partition (LINUX_LIVE) on Disk $selDiskNum"
                if ($useRefind) {
                    $step++
                    $changeLines += "  $step. Create 100 MB FAT32 rEFInd partition"
                }
                $step++
                $changeLines += "  $step. Leave $LinuxSizeGB GB unallocated for Linux installation"
                $step++
                if ($useRefind) {
                    $changeLines += "  $step. Install rEFInd boot manager and configure UEFI boot entry"
                } else {
                    $changeLines += "  $step. Configure UEFI boot entry for $DistroName"
                }

                if ($partitions) {
                    $afterLines = Format-AfterLayout -Partitions $partitions -DistroName $DistroName `
                        -BootPartSizeGB $bootPartSizeGB -LinuxSizeGB $LinuxSizeGB `
                        -ShrinkLetter $shrinkTarget.DriveLetter -NewShrinkSizeGB $newPartSizeGB -UseRefind:$useRefind
                }
            } else {
                if ($hasFreeSpace) {
                    $step = 1
                    $changeLines += "  $step. C: partition is NOT modified (different disk selected)"
                    $step++
                    $changeLines += "  $step. Create 7 GB FAT32 boot partition (LINUX_LIVE) on Disk $selDiskNum"
                    if ($useRefind) {
                        $step++
                        $changeLines += "  $step. Create 100 MB FAT32 rEFInd partition"
                    }
                    $step++
                    $changeLines += "  $step. Remaining unallocated space on Disk $selDiskNum available for Linux"
                    $step++
                    if ($useRefind) {
                        $changeLines += "  $step. Install rEFInd boot manager and configure UEFI boot entry"
                    } else {
                        $changeLines += "  $step. Configure UEFI boot entry for $DistroName"
                    }

                    $remainFreeGB = [math]::Round($diskFreeGB - $bootPartSizeGB - $refindGB, 1)
                    if ($partitions) {
                        $afterLines = Format-AfterLayout -Partitions $partitions -DistroName $DistroName `
                            -BootPartSizeGB $bootPartSizeGB -ShowUnchanged -AppendLinuxAndBoot `
                            -RemainingFreeGB $remainFreeGB -UseRefind:$useRefind
                    }
                } else {
                    $changeLines += "  Cannot proceed with this disk."
                    $changeLines += ""
                    if ($nonNtfsPartitions.Count -gt 0) {
                        foreach ($nfp in $nonNtfsPartitions) {
                            $changeLines += "  $($nfp.DriveLetter): is $($nfp.FileSystem) ($($nfp.SizeGB) GB) - cannot be shrunk by Windows."
                        }
                        $changeLines += ""
                        $changeLines += "  To use this disk, you would need to:"
                        $changeLines += "    - Back up your data from the drive"
                        $changeLines += "    - Shrink or delete the partition using Disk Management"
                        $changeLines += "    - Re-run ULLI (it will detect the free space)"
                    } else {
                        $changeLines += "  No unallocated space available on this disk."
                    }

                    if ($partitions) {
                        $afterLines = Format-AfterLayout -Partitions $partitions -DistroName $DistroName `
                            -BootPartSizeGB $bootPartSizeGB -NoChanges
                    }
                }
            }

            $changesText.Text = ($changeLines -join "`r`n")
            $afterText.Text = ($afterLines -join "`r`n")
        }
    }

    # Wire events
    $diskCombo.Add_SelectedIndexChanged({
        $radioShrink.Checked = $true
        & $updateAll
    })
    $sizeNumeric.Add_ValueChanged({ & $updateAll })
    $radioShrink.Add_CheckedChanged({ & $updateAll })
    $radioFreeAll.Add_CheckedChanged({ & $updateAll })
    $radioWipe.Add_CheckedChanged({ & $updateAll })

    # Initial update
    & $updateAll

    # ---- BUTTONS ----
    $confirmButton = New-Object System.Windows.Forms.Button
    $confirmButton.Text = "Confirm && Proceed"
    $confirmButton.Font = $planBoldFont
    $confirmButton.Size = New-Object System.Drawing.Size(160, 38)
    $confirmButton.Location = New-Object System.Drawing.Point(362, $yPos)
    $confirmButton.BackColor = [System.Drawing.Color]::FromArgb(135, 185, 74)
    $confirmButton.ForeColor = [System.Drawing.Color]::White
    $confirmButton.FlatStyle = "Flat"
    $planForm.Controls.Add($confirmButton)

    $cancelPlanButton = New-Object System.Windows.Forms.Button
    $cancelPlanButton.Text = "Cancel"
    $cancelPlanButton.Font = $planFont
    $cancelPlanButton.Size = New-Object System.Drawing.Size(120, 38)
    $cancelPlanButton.Location = New-Object System.Drawing.Point(532, $yPos)
    $planForm.Controls.Add($cancelPlanButton)

    $script:DiskPlanApproved = $false

    $confirmButton.Add_Click({
        $selIndex = $diskCombo.SelectedIndex
        $selDisk = $diskItems[$selIndex]

        if (-not $selDisk.IsCDisk) {
            $strat = $script:DiskPlanStrategy
            if ($strat -eq "other_drive") {
                $minFreeNeeded = $bootPartSizeGB + $refindGB + 1
                if ($selDisk.FreeGB -lt $minFreeNeeded) {
                    [System.Windows.Forms.MessageBox]::Show(
                        "Disk $($selDisk.Number) does not have enough unallocated space.`n`n" +
                        "Need at least $minFreeNeeded GB of free space, but only $($selDisk.FreeGB) GB available.`n`n" +
                        "Please select a different disk or choose to shrink a partition.",
                        "Insufficient Space on Target Disk",
                        [System.Windows.Forms.MessageBoxButtons]::OK,
                        [System.Windows.Forms.MessageBoxIcon]::Warning
                    )
                    return
                }
            } elseif ($strat -eq "other_drive_shrink") {
                $powerConfirm = [System.Windows.Forms.MessageBox]::Show(
                    "Keep your computer plugged in!`n`n" +
                    "A partition resize is about to begin. Power loss during this process " +
                    "could corrupt your partition table.`n`n" +
                    "Make sure your computer is connected to AC power before continuing.",
                    "Power Requirement Warning",
                    [System.Windows.Forms.MessageBoxButtons]::OKCancel,
                    [System.Windows.Forms.MessageBoxIcon]::Warning
                )
                if ($powerConfirm -ne [System.Windows.Forms.DialogResult]::OK) {
                    return
                }
            } elseif ($strat -eq "wipe_disk") {
                $powerConfirm = [System.Windows.Forms.MessageBox]::Show(
                    "Keep your computer plugged in!`n`n" +
                    "A partition resize is about to begin. Power loss during this process " +
                    "could corrupt your partition table.`n`n" +
                    "Make sure your computer is connected to AC power before continuing.",
                    "Power Requirement Warning",
                    [System.Windows.Forms.MessageBoxButtons]::OKCancel,
                    [System.Windows.Forms.MessageBoxIcon]::Warning
                )
                if ($powerConfirm -ne [System.Windows.Forms.DialogResult]::OK) {
                    return
                }
                
                $wipeConfirm = [System.Windows.Forms.MessageBox]::Show(
                    "WARNING: You are about to ERASE ALL DATA on Disk $($selDisk.Number)!`n`n" +
                    "This will:`n" +
                    "  - Destroy the partition table`n" +
                    "  - Delete ALL partitions and data`n" +
                    "  - Create a fresh GPT layout`n`n" +
                    "This action CANNOT be undone.`n`n" +
                    "Are you absolutely sure?",
                    "Confirm Disk Wipe",
                    [System.Windows.Forms.MessageBoxButtons]::YesNo,
                    [System.Windows.Forms.MessageBoxIcon]::Warning
                )
                if ($wipeConfirm -ne [System.Windows.Forms.DialogResult]::Yes) {
                    return
                }
            } elseif ($strat -eq "other_drive" -and -not ($selDisk.FreeGB -ge ($bootPartSizeGB + $refindGB + 1))) {
                [System.Windows.Forms.MessageBox]::Show(
                    "Disk $($selDisk.Number) cannot be used as-is.`n`n" +
                    "It has no unallocated space and no NTFS partitions that can be shrunk.`n" +
                    "You may need to shrink or remove a partition manually first.",
                    "Cannot Use Target Disk",
                    [System.Windows.Forms.MessageBoxButtons]::OK,
                    [System.Windows.Forms.MessageBoxIcon]::Warning
                )
                return
            } else {
                [System.Windows.Forms.MessageBox]::Show(
                    "Disk $($selDisk.Number) has no unallocated space and no shrinkable NTFS partitions.`n`n" +
                    "Please select a different disk.",
                    "Cannot Use Target Disk",
                    [System.Windows.Forms.MessageBoxButtons]::OK,
                    [System.Windows.Forms.MessageBoxIcon]::Warning
                )
                return
            }
        }

        $script:DiskPlanApproved = $true
        $planForm.Close()
    })

    $cancelPlanButton.Add_Click({
        $script:DiskPlanApproved = $false
        $planForm.Close()
    })

    $planScreenH = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea.Height
    $desiredH = $yPos + 52 + ($planForm.Size.Height - $planForm.ClientSize.Height)
    $cappedH = [Math]::Min($desiredH, $planScreenH - 40)
    $planForm.ClientSize = New-Object System.Drawing.Size(702, ($yPos + 52))
    if ($desiredH -gt ($planScreenH - 40)) {
        $planForm.Size = New-Object System.Drawing.Size($planForm.Size.Width, $cappedH)
    }

    $planForm.ShowDialog($form)

    return @{
        Approved = $script:DiskPlanApproved
        Strategy = $script:DiskPlanStrategy
        TargetDiskNumber = $script:DiskPlanTargetDisk
        ShrinkDriveLetter = $script:DiskPlanShrinkLetter
        ShrinkAmountGB = $script:DiskPlanShrinkAmount
        LinuxSizeGB = [int]$sizeNumeric.Value
        UseRefind = $refindCheck.Checked
    }
}

# ─── 10. ISO / rEFInd ────────────────────────────────────────────────────────
# CHECKSUM / DOWNLOAD / REFIND HELPERS
# ────────────────────────────────────────────────────────────────────────────

function Test-IsoChecksum {
    param(
        [string]$FilePath
    )

    Write-Log "Verifying ISO checksum..."
    Set-Status "Verifying ISO integrity..."

    try {
        $distro = Get-SelectedDistro
        $expectedHash = $distro.Checksum
        Write-Log "Expected SHA256: $expectedHash"

        Write-Log "Calculating SHA256 checksum of downloaded ISO (this may take a minute)..."
        $actualHash = (Get-FileHash -Path $FilePath -Algorithm SHA256 -ErrorAction Stop).Hash.ToLower()
        Write-Log "Actual SHA256:   $actualHash"

        if ($actualHash -eq $expectedHash) {
            Write-Log "[PASS] Checksum verification PASSED - ISO is authentic!"
            return $true
        } else {
            Write-Log "[FAIL] Checksum verification FAILED - ISO may be corrupted or tampered!" -Error

            $response = [System.Windows.Forms.MessageBox]::Show(
                "The ISO file checksum does not match the expected checksum!`n`n" +
                "Expected: $expectedHash`n" +
                "Actual:   $actualHash`n`n" +
                "This could mean the file is corrupted or has been tampered with.`n" +
                "Do you want to delete it and re-download?",
                "Checksum Verification Failed",
                [System.Windows.Forms.MessageBoxButtons]::YesNo,
                [System.Windows.Forms.MessageBoxIcon]::Warning
            )

            if ($response -eq [System.Windows.Forms.DialogResult]::Yes) {
                try {
                    Remove-Item $FilePath -Force
                    Write-Log "Corrupted ISO deleted"
                } catch {
                    Write-Log "Error deleting ISO: $_" -Error
                }
            }

            return $false
        }
    }
    catch {
        Write-Log "Error calculating checksum: $_" -Error

        $response = [System.Windows.Forms.MessageBox]::Show(
            "Unable to verify the ISO checksum. Error: $_`n`n" +
            "Do you want to continue anyway?",
            "Checksum Verification Error",
            [System.Windows.Forms.MessageBoxButtons]::YesNo,
            [System.Windows.Forms.MessageBoxIcon]::Question
        )

        return ($response -eq [System.Windows.Forms.DialogResult]::Yes)
    }
}

function Save-LinuxIso {
    param(
        [string]$Destination
    )

    $distro = Get-SelectedDistro
    $isoName = $distro.Name
    $expectedSize = $distro.ExpectedSize
    $mirrors = $distro.Mirrors

    Write-Log "Downloading $isoName ISO ($expectedSize)..."
    Write-Log "This may take a while depending on your internet speed..."

    foreach ($i in 0..($mirrors.Count - 1)) {
        $mirror = $mirrors[$i]
        Write-Log "Trying mirror $($i + 1)/$($mirrors.Count): $($mirror.Split('/')[2])"
        Set-Status "Connecting to mirror..."

        try {
            Add-Type -AssemblyName System.Net.Http

            $httpClient = New-Object System.Net.Http.HttpClient
            $httpClient.Timeout = [TimeSpan]::FromMinutes(60)

            $response = $httpClient.GetAsync($mirror, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).Result

            if ($response.IsSuccessStatusCode) {
                $totalBytes = $response.Content.Headers.ContentLength
                $totalMB = [math]::Round($totalBytes / 1MB, 1)
                Write-Log "File size: $totalMB MB"

                $fileStream = [System.IO.File]::Create($Destination)
                $downloadStream = $response.Content.ReadAsStreamAsync().Result

                $buffer = New-Object byte[] 81920
                $totalRead = 0
                $lastUpdate = [DateTime]::Now
                $updateInterval = [TimeSpan]::FromMilliseconds(500)

                Set-Status "Downloading..."

                while ($true) {
                    $bytesRead = $downloadStream.Read($buffer, 0, $buffer.Length)

                    if ($bytesRead -eq 0) {
                        break
                    }

                    $fileStream.Write($buffer, 0, $bytesRead)
                    $totalRead += $bytesRead

                    $now = [DateTime]::Now
                    if (($now - $lastUpdate) -gt $updateInterval) {
                        $percent = [int](($totalRead / $totalBytes) * 100)
                        $mbDownloaded = [math]::Round($totalRead / 1MB, 1)

                        $progressBar.Value = $percent
                        Set-Status "Downloading: $percent% - $mbDownloaded MB / $totalMB MB"

                        [System.Windows.Forms.Application]::DoEvents()
                        $lastUpdate = $now
                    }
                }

                $fileStream.Close()
                $downloadStream.Close()
                $response.Dispose()
                $httpClient.Dispose()

                $progressBar.Value = 100
                Set-Status "Download complete!"
                Start-Sleep -Milliseconds 500
                $progressBar.Value = 0
                $statusLabel.Text = ""

                $fileInfo = Get-Item $Destination
                $fileSizeGB = [math]::Round($fileInfo.Length / 1GB, 2)
                Write-Log "Downloaded file size: $fileSizeGB GB"

                if ($fileInfo.Length -lt 2GB) {
                    Write-Log "File size too small, download may be corrupted" -Error
                    Remove-Item $Destination -Force
                    continue
                }

                if (-not (Test-IsoChecksum -FilePath $Destination)) {
                    Write-Log "Checksum verification failed, trying next mirror..." -Error
                    continue
                }

                return $true
            } else {
                throw "HTTP Error: $($response.StatusCode)"
            }
        }
        catch {
            Write-Log "Download failed: $_" -Error

            if (Test-Path $Destination) {
                try {
                    Remove-Item $Destination -Force -ErrorAction SilentlyContinue
                    Write-Log "Removed incomplete download"
                } catch {}
            }

            if ($i -lt $mirrors.Count - 1) {
                Write-Log "Trying next mirror..."
            }
        }
    }

    # All mirrors failed
    Write-Log "All automatic download attempts failed" -Error

    $response = [System.Windows.Forms.MessageBox]::Show(
        "Automatic download failed. Would you like to:`n`n" +
        "- Download manually from your browser?`n" +
        "- Place the file at: $Destination`n" +
        "- Then run the installer again`n`n" +
        "Click Yes to open the $isoName download page, No to cancel",
        "Download Failed",
        [System.Windows.Forms.MessageBoxButtons]::YesNo,
        [System.Windows.Forms.MessageBoxIcon]::Information
    )

    if ($response -eq [System.Windows.Forms.DialogResult]::Yes) {
        Start-Process $distro.DownloadPage
        Write-Log $distro.DownloadMsg
        Write-Log $Destination
        Write-Log "Then run the installer again"
    }

    return $false
}

# ============================================================
# rEFInd DOWNLOAD AND INSTALL
# ============================================================
function Save-Refind {
    $dest = Join-Path $env:TEMP $script:RefindFilename
    if (Test-Path $dest) {
        Write-Log "Found cached rEFInd: $dest"
        return $dest
    }
    Write-Log "Downloading rEFInd boot manager..."
    Set-Status "Downloading rEFInd..."
    try {
        Add-Type -AssemblyName System.Net.Http
        $handler = New-Object System.Net.Http.HttpClientHandler
        $handler.AllowAutoRedirect = $true
        $httpClient = New-Object System.Net.Http.HttpClient($handler)
        $httpClient.Timeout = [TimeSpan]::FromMinutes(10)
        $httpClient.DefaultRequestHeaders.UserAgent.ParseAdd("windows-installer/1.0")

        $response = $httpClient.GetAsync(
            $script:RefindUrl,
            [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead
        ).Result

        if ($response.IsSuccessStatusCode) {
            $totalBytes = $response.Content.Headers.ContentLength
            $fileStream = [System.IO.File]::Create($dest)
            $downloadStream = $response.Content.ReadAsStreamAsync().Result
            $buffer = New-Object byte[] 81920
            $totalRead = [int64]0
            $lastUpdate = [DateTime]::Now

            while ($true) {
                $bytesRead = $downloadStream.Read($buffer, 0, $buffer.Length)
                if ($bytesRead -eq 0) { break }
                $fileStream.Write($buffer, 0, $bytesRead)
                $totalRead += $bytesRead
                $now = [DateTime]::Now
                if (($now - $lastUpdate).TotalMilliseconds -gt 500) {
                    if ($totalBytes -gt 0) {
                        $percent = [int](($totalRead / $totalBytes) * 100)
                        Set-Status "Downloading rEFInd... $percent%"
                    }
                    [System.Windows.Forms.Application]::DoEvents()
                    $lastUpdate = $now
                }
            }

            $fileStream.Close()
            $downloadStream.Close()
            $response.Dispose()
            $httpClient.Dispose()

            $sizeMB = [math]::Round((Get-Item $dest).Length / 1MB, 1)
            Write-Log "rEFInd downloaded: $sizeMB MB"
            Set-Status ""
            return $dest
        } else {
            throw "HTTP Error: $($response.StatusCode)"
        }
    }
    catch {
        Write-Log "rEFInd download failed: $_" -Error
        if (Test-Path $dest) { Remove-Item $dest -Force }
        return $null
    }
}

function Install-Refind {
    param(
        [string]$RefindDriveLetter,
        [string]$BootDriveLetter,
        [string]$DistroLabel
    )

    Write-Log ""
    Write-Log "== Installing rEFInd boot manager =="

    $refindZip = Save-Refind
    if (-not $refindZip) {
        Write-Log "Cannot install rEFInd without the download." -Error
        return $false
    }

    # Extract rEFInd
    Write-Log "Extracting rEFInd..."
    Set-Status "Extracting rEFInd..."
    $extractDir = Join-Path $env:TEMP "refind_extract"
    if (Test-Path $extractDir) { Remove-Item $extractDir -Recurse -Force }

    try {
        Expand-Archive -Path $refindZip -DestinationPath $extractDir -Force
    }
    catch {
        Write-Log "Failed to extract rEFInd: $_" -Error
        return $false
    }

    $refindSrc = Join-Path $extractDir "refind-bin-0.14.2\refind"
    $refindDrive = "${RefindDriveLetter}:"
    $efiBoot = Join-Path $refindDrive "EFI\BOOT"
    New-Item -Path $efiBoot -ItemType Directory -Force | Out-Null

    Write-Log "Copying rEFInd files..."
    Set-Status "Installing rEFInd..."

    # Copy refind_x64.efi as default UEFI loader
    $srcEfi = Join-Path $refindSrc "refind_x64.efi"
    if (Test-Path $srcEfi) {
        Copy-Item $srcEfi (Join-Path $efiBoot "BOOTx64.EFI") -Force
        Write-Log "  Copied refind_x64.efi as BOOTx64.EFI"
    } else {
        Write-Log "refind_x64.efi not found in extracted archive!" -Error
        return $false
    }

    # Copy filesystem drivers (ext4 needed to read LINUX_LIVE if ext4)
    $driversDir = Join-Path $efiBoot "drivers_x64"
    New-Item -Path $driversDir -ItemType Directory -Force | Out-Null
    $driversSrc = Join-Path $refindSrc "drivers_x64"
    foreach ($drv in @("ext4_x64.efi", "ext2_x64.efi")) {
        $src = Join-Path $driversSrc $drv
        if (Test-Path $src) {
            Copy-Item $src $driversDir -Force
            Write-Log "  Copied driver: $drv"
        }
    }

    # Copy icons
    $iconsSrc = Join-Path $refindSrc "icons"
    if (Test-Path $iconsSrc) {
        $iconsDir = Join-Path $efiBoot "icons"
        New-Item -Path $iconsDir -ItemType Directory -Force | Out-Null
        robocopy $iconsSrc $iconsDir /E /R:2 /W:2 /NP /NFL /NDL | Out-Null
        Write-Log "  Copied rEFInd icons."
    }

    # Detect boot layout on LINUX_LIVE partition
    $bootDrive = "${BootDriveLetter}:"
    $hasPxeboot = Test-Path (Join-Path $bootDrive "images\pxeboot\vmlinuz")
    $hasCasper = Test-Path (Join-Path $bootDrive "casper\vmlinuz")
    $hasLive = Test-Path (Join-Path $bootDrive "live\vmlinuz")

    # Extract kernel args from distro's grub.cfg
    $extraArgs = ""
    foreach ($cfgPath in @("EFI\BOOT\grub.cfg", "boot\grub2\grub.cfg", "boot\grub\grub.cfg")) {
        $full = Join-Path $bootDrive $cfgPath
        if (Test-Path $full) {
            try {
                $content = Get-Content $full -Raw -ErrorAction Stop
                foreach ($line in ($content -split "`n")) {
                    $s = $line.Trim()
                    if ($s -match "^(linux|linuxefi)\s") {
                        $parts = $s -split "\s+"
                        $extraKernelArgs = @()
                        for ($i = 2; $i -lt $parts.Count; $i++) {
                            $p = $parts[$i]
                            if ($p -match "^root=") { continue }
                            if ($p -match "CDLABEL=" -or $p -match "LABEL=") {
                                $p = $p -replace "(CDLABEL=|LABEL=)\S+", '$1LINUX_LIVE'
                            }
                            $extraKernelArgs += $p
                        }
                        $extraArgs = $extraKernelArgs -join " "
                        break
                    }
                }
            } catch {}
            if ($extraArgs) { break }
        }
    }

    # Write refind.conf
    Write-Log "Writing rEFInd configuration..."
    $conf = "# rEFInd configuration - generated by ULLI`n"
    $conf += "timeout 10`n"
    $conf += "use_graphics_for linux`n"
    $conf += "scanfor internal,external,manual`n"
    $conf += "scan_all_linux_kernels false`n"
    $conf += "`n"

    if ($hasPxeboot) {
        if (-not $extraArgs) { $extraArgs = "rd.live.image rhgb quiet" }
        $conf += "menuentry `"$DistroLabel`" {`n"
        $conf += "  volume LINUX_LIVE`n"
        $conf += "  loader /images/pxeboot/vmlinuz`n"
        $conf += "  initrd /images/pxeboot/initrd.img`n"
        $conf += "  options `"root=live:LABEL=LINUX_LIVE $extraArgs`"`n"
        $conf += "}`n`n"
        $conf += "menuentry `"$DistroLabel (verbose)`" {`n"
        $conf += "  volume LINUX_LIVE`n"
        $conf += "  loader /images/pxeboot/vmlinuz`n"
        $conf += "  initrd /images/pxeboot/initrd.img`n"
        $conf += "  options `"root=live:LABEL=LINUX_LIVE rd.live.image`"`n"
        $conf += "}`n"
    }
    elseif ($hasCasper) {
        if (-not $extraArgs) { $extraArgs = "quiet splash" }
        $conf += "menuentry `"$DistroLabel`" {`n"
        $conf += "  volume LINUX_LIVE`n"
        $conf += "  loader /casper/vmlinuz`n"
        $conf += "  initrd /casper/initrd`n"
        $conf += "  options `"boot=casper $extraArgs`"`n"
        $conf += "}`n"
    }
    elseif ($hasLive) {
        if (-not $extraArgs) { $extraArgs = "boot=live components quiet splash" }
        $conf += "menuentry `"$DistroLabel`" {`n"
        $conf += "  volume LINUX_LIVE`n"
        $conf += "  loader /live/vmlinuz`n"
        $conf += "  initrd /live/initrd.img`n"
        $conf += "  options `"$extraArgs`"`n"
        $conf += "}`n"
    }
    else {
        $conf += "# Unknown layout - rEFInd will auto-scan.`n"
    }

    Set-Content -Path (Join-Path $efiBoot "refind.conf") -Value $conf -Encoding UTF8 -Force
    Write-Log "rEFInd configuration written."

    # Clean up extract dir
    if (Test-Path $extractDir) { Remove-Item $extractDir -Recurse -Force -ErrorAction SilentlyContinue }

    Write-Log "rEFInd installed successfully."
    Write-Log "  rEFInd partition: $refindDrive"
    return $true
}

function New-RefindPartition {
    param(
        [int]$DiskNumber,
        [int64]$AfterOffset = 0
    )

    Write-Log "Creating 100 MB rEFInd partition..."
    Set-Status "Creating rEFInd partition..."

    $refindSize = [int64]($script:RefindSizeMB * 1MB)
    $refindDriveLetter = $null

    # Align offset to 1 MB boundary
    if ($AfterOffset -gt 0) {
        $alignedOffset = [int64]([Math]::Ceiling($AfterOffset / 1MB)) * 1MB
    } else {
        $alignedOffset = 0
    }

    try {
        if ($alignedOffset -gt 0) {
            $refindPartition = New-Partition -DiskNumber $DiskNumber `
                -Offset $alignedOffset `
                -Size $refindSize `
                -AssignDriveLetter `
                -ErrorAction Stop
        } else {
            $refindPartition = New-Partition -DiskNumber $DiskNumber `
                -Size $refindSize `
                -AssignDriveLetter `
                -ErrorAction Stop
        }

        Start-Sleep -Seconds 2
        $refindDriveLetter = $refindPartition.DriveLetter

        if (-not $refindDriveLetter) {
            $refindPartition | Add-PartitionAccessPath -AssignDriveLetter -ErrorAction SilentlyContinue | Out-Null
            Start-Sleep -Seconds 2
            $refindPartition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $refindPartition.PartitionNumber
            $refindDriveLetter = $refindPartition.DriveLetter
        }

        if (-not $refindDriveLetter) {
            throw "Could not assign a drive letter to the rEFInd partition"
        }

        Format-Volume -DriveLetter $refindDriveLetter `
            -FileSystem FAT32 `
            -NewFileSystemLabel "REFIND" `
            -Confirm:$false `
            -ErrorAction Stop | Out-Null

        Write-Log "rEFInd partition created as ${refindDriveLetter}: (REFIND)"
        return $refindDriveLetter
    }
    catch {
        Write-Log "Failed to create rEFInd partition: $_" -Error

        # Fallback: try diskpart
        if ($alignedOffset -gt 0) {
            Write-Log "Trying diskpart method for rEFInd partition..."
            $offsetMB = [int64]([Math]::Floor($alignedOffset / 1MB))
            $sizeMB = $script:RefindSizeMB

            $diskpartScript = @"
select disk $DiskNumber
create partition primary offset=$offsetMB size=$sizeMB
assign
exit
"@
            $scriptPath = Join-Path $env:TEMP "create_refind_partition.txt"
            $diskpartScript | Out-File -FilePath $scriptPath -Encoding ASCII
            $result = & diskpart /s $scriptPath 2>&1
            Remove-Item $scriptPath -Force

            $resultString = $result -join "`n"
            if ($resultString -match "successfully created") {
                Start-Sleep -Seconds 3

                # Find the new partition
                $targetSize = [int64]($sizeMB * 1MB)
                $tolerance = [int64](10MB)
                $newParts = Get-Partition -DiskNumber $DiskNumber |
                    Where-Object { [Math]::Abs($_.Size - $targetSize) -lt $tolerance }
                $refPart = $newParts | Sort-Object Offset -Descending | Select-Object -First 1

                if ($refPart) {
                    $refindDriveLetter = $refPart.DriveLetter
                    if (-not $refindDriveLetter) {
                        $refPart | Add-PartitionAccessPath -AssignDriveLetter -ErrorAction SilentlyContinue | Out-Null
                        Start-Sleep -Seconds 2
                        $refPart = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $refPart.PartitionNumber
                        $refindDriveLetter = $refPart.DriveLetter
                    }

                    if ($refindDriveLetter) {
                        Format-Volume -DriveLetter $refindDriveLetter `
                            -FileSystem FAT32 `
                            -NewFileSystemLabel "REFIND" `
                            -Confirm:$false `
                            -ErrorAction Stop | Out-Null
                        Write-Log "rEFInd partition created via diskpart as ${refindDriveLetter}: (REFIND)"
                        return $refindDriveLetter
                    }
                }
            }
        }

        Write-Log "All rEFInd partition creation methods failed" -Error
        return $null
    }
}

# ─── 11. Install state ──────────────────────────────────────────────────────
# START-INSTALLATION - top-level state machine
# ────────────────────────────────────────────────────────────────────────────

function Start-Installation {
    if ($script:IsRunning) {
        return
    }

    $distro = Get-SelectedDistro
    $distroName = $distro.Name

    # ========================================
    # SHOW DISK PLAN - user must approve
    # ========================================
    $planResult = Show-DiskPlan -DistroName $distroName

    if (-not $planResult.Approved) {
        Write-Log "Installation cancelled by user at disk plan review."
        Set-Status "Ready to install"
        return
    }

    $selectedStrategy = $planResult.Strategy
    $targetDiskNumber = $planResult.TargetDiskNumber
    $isOtherDrive = ($selectedStrategy -eq "other_drive" -or $selectedStrategy -eq "other_drive_shrink" -or $selectedStrategy -eq "wipe_disk")
    $otherDriveShrinkLetter = $planResult.ShrinkDriveLetter
    $otherDriveShrinkAmountGB = $planResult.ShrinkAmountGB
    $linuxSizeGB = $planResult.LinuxSizeGB
    $useRefind = $planResult.UseRefind
    $refindGB = if ($useRefind) { 0.1 } else { 0 }
    $totalNeededGB = $linuxSizeGB + $script:MinPartitionSizeGB + $refindGB
    $refindNote = if ($useRefind) { ", rEFInd: yes" } else { "" }
    Write-Log "Disk plan approved. Strategy: $selectedStrategy, Target disk: $targetDiskNumber, Linux size: $linuxSizeGB GB$refindNote"

    # Now lock the UI and proceed
    $script:IsRunning = $true
    Set-UILocked $true

    try {
        # Determine ISO path
        if ($customRadio.Checked) {
            if (-not $script:CustomIsoPath -or -not (Test-Path $script:CustomIsoPath)) {
                Write-Log "Error: Please select a valid ISO file!" -Error
                return
            }
            $script:IsoPath = $script:CustomIsoPath
            Write-Log "Using custom ISO: $script:IsoPath"
            $isoInfo = Get-Item $script:IsoPath
            Write-Log "ISO file size: $([math]::Round($isoInfo.Length / 1GB, 2)) GB"
        } else {
            $script:IsoPath = Join-Path $env:TEMP $distro.IsoFilename
            Write-Log "Selected distribution: $distroName"
        }

        # Check space (only if we're shrinking C:)
        if ($selectedStrategy -eq "shrink_all") {
            $headroom = $script:Const.ShrinkAllCHeadroomGB
            if ($script:CDriveInfo.FreeGB -lt ($totalNeededGB + $headroom)) {
                Write-Log "Error: Not enough free space on C: to shrink!" -Error
                Write-Log "Need: $($totalNeededGB + $headroom) GB free on C:" -Error
                Write-Log "Have: $($script:CDriveInfo.FreeGB) GB" -Error
                return
            }
        } elseif ($selectedStrategy -eq "use_free_boot") {
            $headroom = $script:Const.UseFreeBootCHeadroomGB
            if ($script:CDriveInfo.FreeGB -lt ($linuxSizeGB + $headroom)) {
                Write-Log "Error: Not enough free space on C: to shrink!" -Error
                Write-Log "Need: $($linuxSizeGB + $headroom) GB free on C:" -Error
                Write-Log "Have: $($script:CDriveInfo.FreeGB) GB" -Error
                return
            }
        } elseif ($selectedStrategy -eq "other_drive") {
            $otherDiskFreeGB = Get-DiskUnallocatedGB -DiskNumber $targetDiskNumber
            $minNeededFreeGB = $script:MinPartitionSizeGB + $refindGB + $script:Const.GapSlackGB
            if ($otherDiskFreeGB -lt $minNeededFreeGB) {
                Write-Log "Error: Not enough unallocated space on Disk $targetDiskNumber!" -Error
                Write-Log "Need: $minNeededFreeGB GB, Have: $otherDiskFreeGB GB" -Error
                return
            }
        } elseif ($selectedStrategy -eq "other_drive_shrink") {
            if (-not $otherDriveShrinkLetter) {
                Write-Log "Error: No partition selected to shrink on Disk $targetDiskNumber!" -Error
                return
            }
            try {
                $shrinkVol = Get-Volume -DriveLetter $otherDriveShrinkLetter -ErrorAction Stop
                $shrinkFreeGB = [math]::Round($shrinkVol.SizeRemaining / 1GB, 2)
                $headroom = $script:Const.OtherDriveShrinkHeadroomGB
                if ($shrinkFreeGB -lt ($otherDriveShrinkAmountGB + $headroom)) {
                    Write-Log "Error: Not enough free space on ${otherDriveShrinkLetter}: to shrink!" -Error
                    Write-Log "Need: $($otherDriveShrinkAmountGB + $headroom) GB free, Have: $shrinkFreeGB GB" -Error
                    return
                }
            } catch {
                Write-Log "Error: Cannot access volume ${otherDriveShrinkLetter}: - $_" -Error
                return
            }
        }

        # Download ISO if needed
        if (-not $customRadio.Checked) {
            if (Test-Path $script:IsoPath) {
                Write-Log "Found existing ISO at: $script:IsoPath"

                try {
                    $fileInfo = Get-Item $script:IsoPath
                    $fileSizeGB = [math]::Round($fileInfo.Length / 1GB, 2)
                    Write-Log "Existing ISO size: $fileSizeGB GB"

                    if ($fileInfo.Length -lt 2GB) {
                        Write-Log "Existing ISO appears corrupted (too small)" -Error
                        Write-Log "Deleting corrupted file..." -Error
                        Remove-Item $script:IsoPath -Force

                        Set-Status "Re-downloading $distroName ISO..."
                        if (-not (Save-LinuxIso -Destination $script:IsoPath)) {
                            Write-Log "Failed to download $distroName ISO!" -Error
                            return
                        }
                    } else {
                        if (-not (Test-IsoChecksum -FilePath $script:IsoPath)) {
                            Write-Log "Existing ISO failed checksum verification" -Error

                            Set-Status "Re-downloading $distroName ISO..."
                            if (-not (Save-LinuxIso -Destination $script:IsoPath)) {
                                Write-Log "Failed to download $distroName ISO!" -Error
                                return
                            }
                        } else {
                            if (-not $distro.IsHybrid) {
                                try {
                                    Get-DiskImage -ImagePath $script:IsoPath -ErrorAction Stop | Out-Null
                                    Write-Log "ISO mount test passed"
                                }
                                catch {
                                    Write-Log "Existing ISO appears corrupted (mount test failed)" -Error
                                    Write-Log "Error: $_" -Error

                                    $response = [System.Windows.Forms.MessageBox]::Show(
                                        "The existing ISO file appears to be corrupted. Would you like to re-download it?",
                                        "Corrupted ISO",
                                        [System.Windows.Forms.MessageBoxButtons]::YesNo,
                                        [System.Windows.Forms.MessageBoxIcon]::Warning
                                    )

                                    if ($response -eq [System.Windows.Forms.DialogResult]::Yes) {
                                        Remove-Item $script:IsoPath -Force
                                        Set-Status "Re-downloading $distroName ISO..."
                                        if (-not (Save-LinuxIso -Destination $script:IsoPath)) {
                                            Write-Log "Failed to download $distroName ISO!" -Error
                                            return
                                        }
                                    } else {
                                        Write-Log "Installation cancelled by user" -Error
                                        return
                                    }
                                }
                            } else {
                                Write-Log "ISO mount test skipped ($($distro.Keyword) hybrid ISO format)"
                            }
                        }
                    }
                }
                catch {
                    Write-Log "Error checking existing ISO: $_" -Error
                    return
                }
            } else {
                Set-Status "Downloading $distroName ISO..."
                if (-not (Save-LinuxIso -Destination $script:IsoPath)) {
                    Write-Log "Failed to download $distroName ISO!" -Error
                    return
                }
            }
        }

        # ── Wipe-disk strategy (secondary drives only) ──────────────────────
        if ($selectedStrategy -eq "wipe_disk") {
            # ── power warning dialog ─────────────────────────────────
            $powerConfirm = [System.Windows.Forms.MessageBox]::Show(
                "Keep your computer plugged in!`n`n" +
                "A partition resize is about to begin. Power loss during this process " +
                "could corrupt your partition table.`n`n" +
                "Make sure your computer is connected to AC power before continuing.",
                "Power Requirement Warning",
                [System.Windows.Forms.MessageBoxButtons]::OKCancel,
                [System.Windows.Forms.MessageBoxIcon]::Warning
            )
            if ($powerConfirm -ne [System.Windows.Forms.DialogResult]::OK) {
                return
            }

            Write-Log ""
            Write-Log "== Strategy: wipe & reformat entire disk =="
            Write-Log "Target disk: Disk $targetDiskNumber"

            if ($targetDiskNumber -eq $script:CDriveInfo.DiskNumber) {
                Write-Log "REFUSING to wipe the disk containing Windows!" -Error
                return
            }

            Set-Status "Wiping disk $targetDiskNumber..."
            Write-Log "Clearing all data from Disk $targetDiskNumber..."

            try {
                Clear-Disk -Number $targetDiskNumber -RemoveData -RemoveOEM -Confirm:$false -ErrorAction Stop
                Write-Log "Disk cleared successfully."
            }
            catch {
                Write-Log "Clear-Disk failed: $_" -Error
                Write-Log "Trying diskpart fallback..."

                $diskpartScript = @"
select disk $targetDiskNumber
clean
convert gpt
exit
"@
                $scriptPath = Join-Path $env:TEMP "wipe_disk.txt"
                $diskpartScript | Out-File -FilePath $scriptPath -Encoding ASCII
                $result = diskpart /s $scriptPath
                Remove-Item $scriptPath -Force

                $resultString = $result -join "`n"
                if ($resultString -notmatch "succeeded|successfully") {
                    Write-Log "Diskpart wipe also failed!" -Error
                    Write-Log $resultString -Error
                    return
                }
                Write-Log "Disk wiped via diskpart."
            }

            Start-Sleep -Seconds 2

            try {
                $diskStatus = Get-Disk -Number $targetDiskNumber
                if ($diskStatus.PartitionStyle -ne "GPT") {
                    Initialize-Disk -Number $targetDiskNumber -PartitionStyle GPT -ErrorAction Stop
                    Write-Log "Disk initialized as GPT."
                }
            } catch {
                Write-Log "Note: GPT initialization: $_"
            }

            Start-Sleep -Seconds 2

            # Create boot partition (7 GB)
            Write-Log "Creating $($script:MinPartitionSizeGB) GB boot partition..."
            Set-Status "Creating boot partition..."
            try {
                $bootPartition = New-Partition -DiskNumber $targetDiskNumber `
                    -Size ([int64]($script:MinPartitionSizeGB * 1GB)) `
                    -AssignDriveLetter `
                    -ErrorAction Stop

                Start-Sleep -Seconds 2
                $driveLetter = $bootPartition.DriveLetter

                if (-not $driveLetter) {
                    $bootPartition | Add-PartitionAccessPath -AssignDriveLetter -ErrorAction SilentlyContinue
                    Start-Sleep -Seconds 2
                    $bootPartition = Get-Partition -DiskNumber $targetDiskNumber -PartitionNumber $bootPartition.PartitionNumber
                    $driveLetter = $bootPartition.DriveLetter
                }

                if (-not $driveLetter) {
                    throw "Could not assign a drive letter to the boot partition"
                }

                Format-Volume -DriveLetter $driveLetter `
                    -FileSystem FAT32 `
                    -NewFileSystemLabel "LINUX_LIVE" `
                    -Confirm:$false `
                    -ErrorAction Stop

                Write-Log "Boot partition created as ${driveLetter}: (LINUX_LIVE)"
                $script:NewDrive = "${driveLetter}:"
                $script:VolumeLabel = "LINUX_LIVE"
            }
            catch {
                Write-Log "Failed to create boot partition: $_" -Error
                return
            }

            # Create rEFInd partition if enabled (wipe_disk)
            if ($useRefind) {
                Start-Sleep -Seconds 2
                $bootPartInfo = Get-Partition -DiskNumber $targetDiskNumber |
                    Where-Object { $_.DriveLetter -eq $driveLetter } | Select-Object -First 1
                $refindAfterOffset = $bootPartInfo.Offset + $bootPartInfo.Size
                $script:RefindDriveLetter = New-RefindPartition -DiskNumber $targetDiskNumber -AfterOffset $refindAfterOffset
                if (-not $script:RefindDriveLetter) {
                    Write-Log "Warning: rEFInd partition creation failed. Continuing without rEFInd." -Error
                    $useRefind = $false
                }
            }

            Start-Sleep -Seconds 2
            $diskAfter = Get-Disk -Number $targetDiskNumber
            $partsAfter = Get-Partition -DiskNumber $targetDiskNumber | Sort-Object Offset
            $usedBytes = [int64]0
            foreach ($p in $partsAfter) { $usedBytes += $p.Size }
            $unallocGB = [math]::Round(($diskAfter.Size - $usedBytes) / 1GB, 1)

            Write-Log ""
            Write-Log "Disk $targetDiskNumber wiped and reformatted successfully:"
            Write-Log "  Partition 1: LINUX_LIVE ($($script:MinPartitionSizeGB) GB, ${driveLetter}:)"
            if ($useRefind -and $script:RefindDriveLetter) {
                Write-Log "  Partition 2: REFIND ($($script:RefindSizeMB) MB, $($script:RefindDriveLetter):)"
            }
            Write-Log "  Unallocated: ~$unallocGB GB (for Linux installer)"
            Write-Log ""
        }
        # ── Shrink/free-space strategies ──────────────────────────────────────
        elseif ($selectedStrategy -eq "other_drive_shrink") {
            # ── power warning dialog ────────────────────────────
            $powerConfirm = [System.Windows.Forms.MessageBox]::Show(
                "Keep your computer plugged in!`n`n" +
                "A partition resize is about to begin. Power loss during this process " +
                "could corrupt your partition table.`n`n" +
                "Make sure your computer is connected to AC power before continuing.",
                "Power Requirement Warning",
                [System.Windows.Forms.MessageBoxButtons]::OKCancel,
                [System.Windows.Forms.MessageBoxIcon]::Warning
            )
            if ($powerConfirm -ne [System.Windows.Forms.DialogResult]::OK) {
                return
            }

            Set-Status "Shrinking ${otherDriveShrinkLetter}: partition on Disk $targetDiskNumber..."
            Write-Log "Shrinking ${otherDriveShrinkLetter}: partition by $otherDriveShrinkAmountGB GB..."
            Write-Log "This will create space for Linux: $linuxSizeGB GB and boot partition: $($script:MinPartitionSizeGB) GB..."

            if (-not (Invoke-PartitionShrink -DriveLetter $otherDriveShrinkLetter -ShrinkAmountGB $otherDriveShrinkAmountGB)) {
                return
            }

            Start-Sleep -Seconds 5
        } elseif ($selectedStrategy -ne "use_free_all" -and -not $isOtherDrive) {
            # ── power warning dialog ────────────────────────────
            $powerConfirm = [System.Windows.Forms.MessageBox]::Show(
                "Keep your computer plugged in!`n`n" +
                "A partition resize is about to begin. Power loss during this process " +
                "could corrupt your partition table.`n`n" +
                "Make sure your computer is connected to AC power before continuing.",
                "Power Requirement Warning",
                [System.Windows.Forms.MessageBoxButtons]::OKCancel,
                [System.Windows.Forms.MessageBoxIcon]::Warning
            )
            if ($powerConfirm -ne [System.Windows.Forms.DialogResult]::OK) {
                return
            }

            $shrinkAmountGB = if ($selectedStrategy -eq "use_free_boot") { $linuxSizeGB } else { $totalNeededGB }

            Set-Status "Shrinking C: partition..."
            Write-Log "Shrinking C: partition by $shrinkAmountGB GB..."

            if ($selectedStrategy -eq "shrink_all") {
                $bootSizeGB = $script:MinPartitionSizeGB
                Write-Log "This will create space for Linux: $linuxSizeGB GB and boot partition: $bootSizeGB GB..."
            } else {
                Write-Log "This will create $linuxSizeGB GB of space for Linux installation..."
                Write-Log "The 7 GB boot partition will use existing unallocated space."
            }

            if (-not (Invoke-PartitionShrink -DriveLetter 'C' -ShrinkAmountGB $shrinkAmountGB)) {
                return
            }

            Start-Sleep -Seconds 5
        } else {
            if ($isOtherDrive) {
                Write-Log "Skipping C: partition shrink - installing to a separate disk (Disk $targetDiskNumber)."
            } else {
                Write-Log "Skipping C: partition shrink - using existing unallocated space."
            }
        }

        # Create boot partition (skip for wipe_disk -- already created above)
        if ($selectedStrategy -ne "wipe_disk") {
        Set-Status "Creating boot partition..."
        Write-Log "Creating $script:MinPartitionSizeGB GB boot partition on Disk $targetDiskNumber..."

        try {
            Start-Sleep -Seconds 2
            $disk = Get-Disk -Number $targetDiskNumber
            $partitions = Get-Partition -DiskNumber $targetDiskNumber | Sort-Object Offset

            if (-not $isOtherDrive) {
                $cPartition = Get-Partition -DriveLetter C
                $cPartitionEnd = $cPartition.Offset + $cPartition.Size
                $anchorEnd = $cPartitionEnd
            } elseif ($selectedStrategy -eq "other_drive_shrink" -and $otherDriveShrinkLetter) {
                $shrunkPartition = Get-Partition -DriveLetter $otherDriveShrinkLetter
                $anchorEnd = $shrunkPartition.Offset + $shrunkPartition.Size
            } else {
                if ($partitions -and $partitions.Count -gt 0) {
                    $lastPart = $partitions | Sort-Object Offset | Select-Object -Last 1
                    $anchorEnd = $lastPart.Offset + $lastPart.Size
                } else {
                    $anchorEnd = [int64](1MB)
                }
            }

            # ── Scan ALL unallocated gaps on the disk ────────────────────────
            $gaps = @()
            $sortedParts = $partitions | Sort-Object Offset
            $prevEnd = [int64]0

            foreach ($part in $sortedParts) {
                $gapSize = $part.Offset - $prevEnd
                if ($gapSize -gt 1MB) {
                    $gaps += [PSCustomObject]@{
                        Start = $prevEnd
                        End   = $part.Offset
                        Size  = $gapSize
                    }
                }
                $prevEnd = $part.Offset + $part.Size
            }
            $trailingGap = $disk.Size - $prevEnd
            if ($trailingGap -gt 1MB) {
                $gaps += [PSCustomObject]@{
                    Start = $prevEnd
                    End   = $disk.Size
                    Size  = $trailingGap
                }
            }

            $bootPartitionSize = [int64]($script:MinPartitionSizeGB * 1GB)
            $alignmentSize = [int64](1MB)
            $refindReserve = if ($useRefind) { [int64]($script:RefindSizeMB * 1MB) } else { [int64]0 }
            $bufferSize = [int64](16MB) + $refindReserve
            $minGapRequired = $bootPartitionSize + $bufferSize + $alignmentSize

            Write-Log "Scanning disk for unallocated gaps..."
            foreach ($gap in $gaps) {
                $gapGB = [math]::Round($gap.Size / 1GB, 2)
                $gapStartGB = [math]::Round($gap.Start / 1GB, 2)
                Write-Log "  Gap at $gapStartGB GB: $gapGB GB"
            }

            $usableGaps = $gaps | Where-Object { $_.Size -ge $minGapRequired }

            if (-not $usableGaps) {
                throw "No unallocated gap large enough for the $script:MinPartitionSizeGB GB boot partition"
            }

            $anchorGap = $usableGaps | Where-Object {
                $_.Start -ge ($anchorEnd - 1MB) -and $_.Start -le ($anchorEnd + 1MB)
            } | Select-Object -First 1

            $chosenGap = if ($anchorGap) { $anchorGap }
                         else { $usableGaps | Sort-Object Size -Descending | Select-Object -First 1 }

            $chosenGapGB = [math]::Round($chosenGap.Size / 1GB, 2)
            $chosenStartGB = [math]::Round($chosenGap.Start / 1GB, 2)
            Write-Log "Selected gap for boot partition: $chosenGapGB GB starting at $chosenStartGB GB"

            $bootPartitionEndOffset = $chosenGap.End - $bufferSize
            $bootPartitionOffset = $bootPartitionEndOffset - $bootPartitionSize
            $bootPartitionOffset = [int64]([Math]::Floor($bootPartitionOffset / $alignmentSize)) * $alignmentSize

            if ($bootPartitionOffset -lt ($chosenGap.Start + $alignmentSize)) {
                throw "Selected gap ($chosenGapGB GB) is too small after alignment for the boot partition"
            }

            $linuxSpace = $bootPartitionOffset - $chosenGap.Start
            $linuxSpaceGB = [math]::Round($linuxSpace / 1GB, 2)

            Write-Log "Unallocated space starts at: $chosenStartGB GB"
            Write-Log "Boot partition will start at: $([math]::Round($bootPartitionOffset / 1GB, 2)) GB"
            Write-Log "Gap ends at: $([math]::Round($chosenGap.End / 1GB, 2)) GB"
            Write-Log "Linux will have $linuxSpaceGB GB of unallocated space"

            Write-Log "Creating boot partition..."

            $bootPartitionSize = [int64]($script:MinPartitionSizeGB * 1GB)
            $offsetMB = [int64]([Math]::Floor($bootPartitionOffset / 1MB))
            $sizeMB = [int64]($script:MinPartitionSizeGB * 1024)

            if ($offsetMB -lt 0 -or $bootPartitionOffset -gt $disk.Size) {
                throw "Invalid offset calculated: $offsetMB MB (from $bootPartitionOffset bytes)"
            }

            Write-Log "Attempting to create partition at offset: $([math]::Round($bootPartitionOffset / 1GB, 2)) GB - $offsetMB MB"

            $partitionCreated = $false
            $newPartition = $null

            try {
                Write-Log "Attempting PowerShell method with specific offset..."
                $newPartition = New-Partition -DiskNumber $targetDiskNumber `
                    -Offset $bootPartitionOffset `
                    -Size $bootPartitionSize `
                    -AssignDriveLetter `
                    -ErrorAction Stop

                $partitionCreated = $true
                $driveLetter = $newPartition.DriveLetter
                Write-Log "Success! Partition created using PowerShell method"
            }
            catch {
                Write-Log "PowerShell method failed: $_"
                Write-Log "Trying diskpart method..."

                $attempts = @(
                    @{Offset = $offsetMB; Description = "Calculated position"},
                    @{Offset = [int64]($offsetMB - 1024); Description = "1GB before calculated position"},
                    @{Offset = [int64]($offsetMB - 2048); Description = "2GB before calculated position"},
                    @{Offset = [int64]($offsetMB - 5120); Description = "5GB before calculated position"}
                )

                $attempts = $attempts | Where-Object { $_.Offset -gt 0 }

                foreach ($attempt in $attempts) {
                    Write-Log "Attempt: $($attempt.Description)"
                    Write-Log "Trying offset: $([math]::Round($attempt.Offset * 1MB / 1GB, 2)) GB"

                    $diskpartScript = @"
select disk $targetDiskNumber
create partition primary offset=$($attempt.Offset) size=$sizeMB
assign
exit
"@
                    $scriptPath = Join-Path $env:TEMP "create_boot_partition.txt"
                    $diskpartScript | Out-File -FilePath $scriptPath -Encoding ASCII

                    $result = & diskpart /s $scriptPath 2>&1
                    Remove-Item $scriptPath -Force

                    $resultString = $result -join "`n"

                    if ($resultString -match "successfully created" -or $resultString -match "DiskPart successfully created") {
                        Write-Log "Success! Boot partition created at offset $([math]::Round($attempt.Offset * 1MB / 1GB, 2)) GB"
                        $partitionCreated = $true
                        break
                    } else {
                        if ($resultString -match "not enough usable space") {
                            Write-Log "Not enough space at this offset, trying next position..."
                        } else {
                            Write-Log "Failed with error: $($resultString | Select-String -Pattern 'error' -SimpleMatch)"
                        }
                    }
                }
            }

            if (-not $partitionCreated -and -not $isOtherDrive) {
                Write-Log "Offset-based creation failed. Trying alternative approach..."

                $currentPartitions = Get-Partition -DiskNumber $targetDiskNumber | Sort-Object Offset
                $cPartition = $currentPartitions | Where-Object { $_.DriveLetter -eq 'C' }
                $cEndOffset = $cPartition.Offset + $cPartition.Size

                $recoveryPartition = $currentPartitions | Where-Object {
                    $_.Type -eq "Recovery" -or $_.GptType -match "de94bba4"
                } | Sort-Object Offset | Select-Object -First 1

                if ($recoveryPartition) {
                    $gapSize = $recoveryPartition.Offset - $cEndOffset
                    $gapSizeGB = [math]::Round($gapSize / 1GB, 2)
                    Write-Log "Gap between C: and Recovery: $gapSizeGB GB"

                    $fillerSize = [int64]($gapSize - ($script:MinPartitionSizeGB * 1GB) - (1GB))
                    $fillerSizeGB = [math]::Round($fillerSize / 1GB, 2)

                    if ($fillerSize -gt 0) {
                        Write-Log "Attempting workaround: Creating filler partition of $fillerSizeGB GB"

                        try {
                            $fillerPartition = New-Partition -DiskNumber $targetDiskNumber `
                                -Size $fillerSize `
                                -ErrorAction Stop

                            Write-Log "Filler partition created. Now creating boot partition..."

                            $bootPartition = New-Partition -DiskNumber $targetDiskNumber `
                                -Size ($script:MinPartitionSizeGB * 1GB) `
                                -AssignDriveLetter `
                                -ErrorAction Stop

                            Write-Log "Removing filler partition..."
                            Remove-Partition -DiskNumber $targetDiskNumber `
                                -PartitionNumber $fillerPartition.PartitionNumber `
                                -Confirm:$false `
                                -ErrorAction Stop

                            Write-Log "Filler partition removed. Boot partition should now be at end."
                            $partitionCreated = $true
                            $newPartition = $bootPartition
                            $driveLetter = $bootPartition.DriveLetter

                            if (-not $driveLetter) {
                                Start-Sleep -Seconds 3
                                $bootPartition = Get-Partition -DiskNumber $targetDiskNumber -PartitionNumber $bootPartition.PartitionNumber
                                $driveLetter = $bootPartition.DriveLetter
                            }
                        }
                        catch {
                            Write-Log "Workaround failed: $_" -Error
                        }
                    }
                }
            }

            if (-not $partitionCreated) {
                Write-Log "All offset methods failed. Creating partition without specific offset..."
                try {
                    $newPartition = New-Partition -DiskNumber $targetDiskNumber `
                        -Size ($script:MinPartitionSizeGB * 1GB) `
                        -AssignDriveLetter `
                        -ErrorAction Stop

                    $driveLetter = $newPartition.DriveLetter
                    $partitionCreated = $true
                    Write-Log "Boot partition created using standard method"
                }
                catch {
                    throw "All partition creation methods failed: $_"
                }
            }

            if ($partitionCreated -and -not $driveLetter) {
                Start-Sleep -Seconds 3

                $targetSize = [int64]($script:MinPartitionSizeGB * 1GB)
                $tolerance = [int64](100MB)

                $newPartitions = Get-Partition -DiskNumber $targetDiskNumber |
                    Where-Object { [Math]::Abs($_.Size - $targetSize) -lt $tolerance }

                $bootPartition = $newPartitions | Sort-Object Offset -Descending | Select-Object -First 1

                if ($bootPartition) {
                    $driveLetter = $bootPartition.DriveLetter

                    if (-not $driveLetter) {
                        $bootPartition | Add-PartitionAccessPath -AssignDriveLetter
                        Start-Sleep -Seconds 2
                        $bootPartition = Get-Partition -DiskNumber $targetDiskNumber -PartitionNumber $bootPartition.PartitionNumber
                        $driveLetter = $bootPartition.DriveLetter
                    }
                } else {
                    throw "Cannot find newly created boot partition"
                }
            }

            if (-not $driveLetter) {
                throw "Failed to get drive letter for boot partition"
            }

            Write-Log "Formatting boot partition as FAT32..."

            $volumeLabel = "LINUX_LIVE"

            Format-Volume -DriveLetter $driveLetter `
                -FileSystem FAT32 `
                -NewFileSystemLabel $volumeLabel `
                -Confirm:$false `
                -ErrorAction Stop

            Write-Log "Boot partition created and assigned to ${driveLetter}:"
            $script:NewDrive = "${driveLetter}:"
            $script:VolumeLabel = $volumeLabel

            Write-Log ""
            Write-Log "=== Final Disk Layout (Disk $targetDiskNumber) ==="
            $finalPartitions = Get-Partition -DiskNumber $targetDiskNumber | Sort-Object Offset

            $previousEnd = [int64]0
            foreach ($part in $finalPartitions) {
                $sizeGB = [math]::Round($part.Size / 1GB, 2)
                $offsetGB = [math]::Round($part.Offset / 1GB, 2)
                $endGB = [math]::Round(($part.Offset + $part.Size) / 1GB, 2)

                if ($part.Offset -gt ($previousEnd + 1MB)) {
                    $gapSize = [math]::Round(($part.Offset - $previousEnd) / 1GB, 2)
                    if ($gapSize -gt 0.1) {
                        Write-Log "[Unallocated: $gapSize GB]"
                    }
                }

                $label = if ($part.DriveLetter) { "Drive $($part.DriveLetter):" }
                        elseif ($part.Type -eq "Recovery" -or $part.GptType -match "de94bba4") { "(Recovery)" }
                        elseif ($part.IsSystem) { "(System)" }
                        else { "(No letter)" }

                Write-Log "Partition $($part.PartitionNumber): $label - Size: $sizeGB GB - Location: $offsetGB-$endGB GB"

                $previousEnd = [int64]($part.Offset + $part.Size)
            }

            if ($disk.Size -gt ($previousEnd + 1MB)) {
                $trailingGap = [math]::Round(($disk.Size - $previousEnd) / 1GB, 2)
                if ($trailingGap -gt 0.1) {
                    Write-Log "[Unallocated: $trailingGap GB]"
                }
            }

            Write-Log ""
            Write-Log "Boot partition successfully created!"

            # Create rEFInd partition if enabled (non-wipe strategies)
            if ($useRefind) {
                $bootPartInfo = Get-Partition -DiskNumber $targetDiskNumber |
                    Where-Object { $_.DriveLetter -eq $driveLetter } | Select-Object -First 1
                if ($bootPartInfo) {
                    $refindAfterOffset = $bootPartInfo.Offset + $bootPartInfo.Size
                    $script:RefindDriveLetter = New-RefindPartition -DiskNumber $targetDiskNumber -AfterOffset $refindAfterOffset
                    if (-not $script:RefindDriveLetter) {
                        Write-Log "Warning: rEFInd partition creation failed. Continuing without rEFInd." -Error
                        $useRefind = $false
                    }
                } else {
                    Write-Log "Warning: Could not find boot partition to place rEFInd after." -Error
                    $useRefind = $false
                }
            }

            Write-Log "Linux can use the unallocated space for installation"

        }
        catch {
            Write-Log "Failed to create boot partition: $_" -Error
            return
        }
        } # end if ($selectedStrategy -ne "wipe_disk")

        # Mount ISO
        Set-Status "Mounting ISO..."
        Write-Log "Mounting ISO..."

        try {
            if (-not (Test-Path $script:IsoPath)) {
                Write-Log "ISO file not found at: $script:IsoPath" -Error
                return
            }

            $mountResult = Mount-DiskImage -ImagePath $script:IsoPath -StorageType ISO -PassThru -ErrorAction Stop
            Start-Sleep -Seconds 2

            $isoVolume = Get-Volume -DiskImage $mountResult -ErrorAction Stop | Select-Object -First 1

            if (-not $isoVolume) {
                Write-Log "Failed to get volume information from mounted ISO" -Error
                Dismount-DiskImage -ImagePath $script:IsoPath -ErrorAction SilentlyContinue
                return
            }

            $sourceDrive = "$($isoVolume.DriveLetter):"
            Write-Log "ISO mounted at $sourceDrive"

            if (-not $customRadio.Checked) {
                $validationFile = "$sourceDrive\$($distro.ValidationFile)"

                if (-not (Test-Path $validationFile)) {
                    Write-Log "Warning: ISO may not be a valid $distroName image (missing expected files)" -Error

                    $response = [System.Windows.Forms.MessageBox]::Show(
                        "The ISO doesn't appear to be a valid $distroName image. Continue anyway?",
                        "Invalid ISO",
                        [System.Windows.Forms.MessageBoxButtons]::YesNo,
                        [System.Windows.Forms.MessageBoxIcon]::Warning
                    )

                    if ($response -ne [System.Windows.Forms.DialogResult]::Yes) {
                        Dismount-DiskImage -ImagePath $script:IsoPath
                        return
                    }
                }
            } else {
                Write-Log "Custom ISO mounted. Skipping validation."
            }
        }
        catch {
            Write-Log "Failed to mount ISO: $_" -Error

            $response = [System.Windows.Forms.MessageBox]::Show(
                "Failed to mount the ISO file. It may be corrupted. Would you like to delete it and re-download?",
                "Mount Failed",
                [System.Windows.Forms.MessageBoxButtons]::YesNo,
                [System.Windows.Forms.MessageBoxIcon]::Error
            )

            if ($response -eq [System.Windows.Forms.DialogResult]::Yes -and -not $customRadio.Checked) {
                try {
                    Remove-Item $script:IsoPath -Force
                    Write-Log "Deleted corrupted ISO"

                    Set-Status "Re-downloading $distroName ISO..."
                    if (Save-LinuxIso -Destination $script:IsoPath) {
                        Start-Installation
                        return
                    }
                } catch {
                    Write-Log "Error handling corrupted ISO: $_" -Error
                }
            }
            return
        }

        # Copy files
        Set-Status "Copying files..."
        Write-Log "Copying $distroName files to $script:NewDrive..."
        Write-Log "This may take 10-20 minutes..."

        try {
            $robocopyArgs = @(
                $sourceDrive,
                $script:NewDrive,
                "/E",
                "/R:3",
                "/W:5",
                "/NP",
                "/NFL",
                "/NDL",
                "/ETA"
            )

            $result = robocopy @robocopyArgs

            if ($LASTEXITCODE -ge 8) {
                Write-Log "Failed to copy files! Exit code: $LASTEXITCODE" -Error
                return
            }

            Write-Log "Files copied successfully!"

            Write-Log "Removing read-only attributes..."
            Set-Status "Removing read-only attributes..."
            try {
                Get-ChildItem -Path $script:NewDrive -Recurse -Force -ErrorAction SilentlyContinue |
                    Where-Object { $_.Attributes -band [System.IO.FileAttributes]::ReadOnly } |
                    ForEach-Object {
                        $_.Attributes = $_.Attributes -band (-bnot [System.IO.FileAttributes]::ReadOnly)
                    }
            } catch {
                Write-Log "Warning: Could not remove all read-only attributes: $_" -Error
            }
        }
        catch {
            Write-Log "Error during file copy: $_" -Error
            return
        }
        finally {
            Dismount-DiskImage -ImagePath $script:IsoPath
        }

        # Fedora-specific: fix volume label in GRUB and isolinux configs
        if ($distro.Keyword -eq "Fedora") {
            Set-Status "Fixing Fedora boot labels..."
            Write-Log "Fixing Fedora volume label references in boot configs..."

            $fedoraLabel = $script:VolumeLabel

            $bootConfigFiles = @()
            $searchPaths = @(
                (Join-Path $script:NewDrive "EFI\BOOT\grub.cfg"),
                (Join-Path $script:NewDrive "EFI\BOOT\BOOT.conf"),
                (Join-Path $script:NewDrive "boot\grub2\grub.cfg"),
                (Join-Path $script:NewDrive "boot\grub\grub.cfg"),
                (Join-Path $script:NewDrive "isolinux\isolinux.cfg"),
                (Join-Path $script:NewDrive "isolinux\grub.conf"),
                (Join-Path $script:NewDrive "syslinux\syslinux.cfg")
            )

            foreach ($cfgPath in $searchPaths) {
                if (Test-Path $cfgPath) {
                    $bootConfigFiles += $cfgPath
                }
            }

            if ($bootConfigFiles.Count -eq 0) {
                Write-Log "Warning: No boot config files found to patch" -Error
            } else {
                $patchedCount = 0
                foreach ($cfgFile in $bootConfigFiles) {
                    try {
                        $content = Get-Content $cfgFile -Raw -ErrorAction Stop
                        $originalContent = $content

                        $content = $content -replace '(root=live:(?:CD)?LABEL=)([^\s\\]+)', "`$1$fedoraLabel"
                        $content = $content -replace '(set isolabel=)([^\s]+)', "`$1$fedoraLabel"
                        $content = $content -replace '(CDLABEL=)([^\s\\]+)', "`$1$fedoraLabel"

                        if ($content -ne $originalContent) {
                            Set-Content -Path $cfgFile -Value $content -Encoding UTF8 -Force
                            Write-Log "  Patched: $(Split-Path -Leaf $cfgFile)"
                            $patchedCount++
                        } else {
                            Write-Log "  No label references in: $(Split-Path -Leaf $cfgFile)"
                        }
                    }
                    catch {
                        Write-Log "  Warning: Could not patch $($cfgFile): $_" -Error
                    }
                }

                if ($patchedCount -gt 0) {
                    Write-Log "Patched $patchedCount boot config file(s) with label '$fedoraLabel'"
                } else {
                    Write-Log "Warning: No LABEL references found to patch. Fedora may not boot correctly." -Error
                    Write-Log "You may need to manually edit EFI\BOOT\grub.cfg and replace the LABEL= value with '$fedoraLabel'" -Error
                }
            }
        }

        # CachyOS/Arch-specific: fix archisolabel in GRUB, syslinux, and loader configs
        if ($distro.Keyword -eq "CachyOS") {
            Set-Status "Fixing CachyOS boot labels..."
            Write-Log "Fixing CachyOS volume label references in boot configs..."

            $cachyLabel = $script:VolumeLabel

            $bootConfigFiles = @()
            $searchPaths = @(
                (Join-Path $script:NewDrive "EFI\BOOT\grub.cfg"),
                (Join-Path $script:NewDrive "boot\grub\grub.cfg"),
                (Join-Path $script:NewDrive "syslinux\archiso_sys-linux.cfg"),
                (Join-Path $script:NewDrive "syslinux\archiso_pxe-linux.cfg"),
                (Join-Path $script:NewDrive "syslinux\archiso_sys.cfg"),
                (Join-Path $script:NewDrive "syslinux\archiso_pxe.cfg"),
                (Join-Path $script:NewDrive "syslinux\syslinux.cfg")
            )

            foreach ($cfgPath in $searchPaths) {
                if (Test-Path $cfgPath) {
                    $bootConfigFiles += $cfgPath
                }
            }

            # Also check systemd-boot loader entries
            $loaderDir = Join-Path $script:NewDrive "loader\entries"
            if (Test-Path $loaderDir) {
                $loaderConfFiles = Get-ChildItem -Path $loaderDir -Filter "*.conf" -ErrorAction SilentlyContinue
                foreach ($lf in $loaderConfFiles) {
                    $bootConfigFiles += $lf.FullName
                }
            }

            if ($bootConfigFiles.Count -eq 0) {
                Write-Log "Warning: No boot config files found to patch" -Error
            } else {
                $patchedCount = 0
                foreach ($cfgFile in $bootConfigFiles) {
                    try {
                        $content = Get-Content $cfgFile -Raw -ErrorAction Stop
                        $originalContent = $content

                        # Patch archisolabel= and archisosearchlabel= (kernel command line)
                        $content = $content -replace '(archiso(?:search)?label=)([^\s\\]+)', "`$1$cachyLabel"

                        # Patch archisodevice=/dev/disk/by-label/LABEL
                        $content = $content -replace '(archisodevice=/dev/disk/by-label/)([^\s\\]+)', "`$1$cachyLabel"

                        # Patch GRUB search commands with --label or --fs-label
                        $content = $content -replace "(search\s+[^\r\n]*?--(?:label|fs-label)\s+)(\S+)", "`$1$cachyLabel"

                        if ($content -ne $originalContent) {
                            Set-Content -Path $cfgFile -Value $content -Encoding UTF8 -Force
                            Write-Log "  Patched: $(Split-Path -Leaf $cfgFile)"
                            $patchedCount++
                        } else {
                            Write-Log "  No label references in: $(Split-Path -Leaf $cfgFile)"
                        }
                    }
                    catch {
                        Write-Log "  Warning: Could not patch $($cfgFile): $_" -Error
                    }
                }

                if ($patchedCount -gt 0) {
                    Write-Log "Patched $patchedCount boot config file(s) with label '$cachyLabel'"
                } else {
                    Write-Log "Warning: No archisolabel references found to patch. CachyOS may not boot correctly." -Error
                    Write-Log "You may need to manually edit the boot config files and replace archisolabel= with '$cachyLabel'" -Error
                }
            }
        }

        # Install rEFInd if enabled
        if ($useRefind -and $script:RefindDriveLetter) {
            $bootDriveLetter = $script:NewDrive.TrimEnd(':')
            $refindInstalled = Install-Refind `
                -RefindDriveLetter $script:RefindDriveLetter `
                -BootDriveLetter $bootDriveLetter `
                -DistroLabel $distroName
            if (-not $refindInstalled) {
                Write-Log "Warning: rEFInd installation failed. Falling back to direct boot." -Error
                $useRefind = $false
            }
        }

        # Create boot configuration
        Set-Status "Creating boot configuration..."
        Write-Log "Creating boot configuration..."

        $efiPath = $script:NewDrive + "\EFI\BOOT"
        if (-not (Test-Path $efiPath)) {
            New-Item -Path $efiPath -ItemType Directory -Force
        }

        # For wipe_disk on a secondary drive: install the bootloader into the
        # Windows ESP so the firmware can boot it. (Skip if rEFInd handles booting.)
        $script:WipeBootInstalled = $false
        if ($selectedStrategy -eq "wipe_disk" -and -not ($useRefind -and $script:RefindDriveLetter)) {
            try {
                Write-Log "Installing bootloader into Windows ESP..."
                Set-Status "Installing bootloader into Windows ESP..."

                $winEspPart = Get-Partition -DiskNumber $script:CDriveInfo.DiskNumber |
                    Where-Object { $_.GptType -eq '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}' } |
                    Select-Object -First 1

                if (-not $winEspPart) {
                    throw "Could not find Windows EFI System Partition"
                }

                $winEspLetter = $winEspPart.DriveLetter
                $removeLetter = $false
                if (-not $winEspLetter) {
                    $winEspPart | Add-PartitionAccessPath -AssignDriveLetter -ErrorAction Stop
                    Start-Sleep -Seconds 2
                    $winEspPart = Get-Partition -DiskNumber $winEspPart.DiskNumber -PartitionNumber $winEspPart.PartitionNumber
                    $winEspLetter = $winEspPart.DriveLetter
                    $removeLetter = $true
                }

                if (-not $winEspLetter) {
                    throw "Could not assign drive letter to Windows ESP"
                }

                $winEspDrive = "${winEspLetter}:"
                Write-Log "Windows ESP mounted at $winEspDrive"

                $safeName = ($distroName -replace '[^a-zA-Z0-9]', '').Trim()
                if (-not $safeName) { $safeName = "Linux" }
                $script:WipeEspDistroDir = "\EFI\$safeName"
                $distroEspDir = "$winEspDrive$($script:WipeEspDistroDir)"
                New-Item -Path $distroEspDir -ItemType Directory -Force | Out-Null

                $sourceEfi = $script:NewDrive + "\EFI\BOOT"
                if (Test-Path $sourceEfi) {
                    robocopy $sourceEfi $distroEspDir /E /R:2 /W:2 /NP /NFL /NDL | Out-Null
                    Write-Log "EFI\BOOT directory copied to $distroEspDir"
                } else {
                    throw "No EFI\BOOT directory found on $($script:NewDrive)"
                }

                foreach ($grubDir in @("boot\grub", "boot\grub2")) {
                    $srcGrub = Join-Path $script:NewDrive $grubDir
                    if (Test-Path $srcGrub) {
                        $dstGrub = Join-Path $distroEspDir $grubDir
                        New-Item -Path $dstGrub -ItemType Directory -Force | Out-Null
                        robocopy $srcGrub $dstGrub /E /R:2 /W:2 /NP /NFL /NDL | Out-Null
                        Write-Log "Copied $grubDir to ESP"
                    }
                }

                $liveLabel = $script:VolumeLabel
                Write-Log "Patching boot configs in ESP to use label '$liveLabel'..."

                $cfgFiles = Get-ChildItem -Path $distroEspDir -Recurse -Include "*.cfg","*.conf" -ErrorAction SilentlyContinue
                $patchedCount = 0
                foreach ($cfgFile in $cfgFiles) {
                    try {
                        $content = Get-Content $cfgFile.FullName -Raw -ErrorAction Stop
                        $original = $content

                        $content = $content -replace "(search\s+[^`n]*(?:--label|-l)\s+')[^']+(')", "`$1$liveLabel`$2"
                        $content = $content -replace '(search\s+[^\n]*(?:--label|-l)\s+")([^"]+)(")', "`$1$liveLabel`$3"
                        $content = $content -replace "(search\s+[^`n]*(?:--label|-l)\s+)(\S+)(\s)", "`$1$liveLabel`$3"

                        $content = $content -replace '(root=live:(?:CD)?LABEL=)([^\s\\]+)', "`$1$liveLabel"
                        $content = $content -replace '(set isolabel=)([^\s]+)', "`$1$liveLabel"
                        $content = $content -replace '(CDLABEL=)([^\s\\]+)', "`$1$liveLabel"

                        $content = $content -replace '(LABEL=)([^\s\\]+)', "`$1$liveLabel"

                        if ($content -ne $original) {
                            Set-Content -Path $cfgFile.FullName -Value $content -Encoding UTF8 -Force
                            $patchedCount++
                        }
                    } catch {
                        Write-Log "  Warning: Could not patch $($cfgFile.Name): $_" -Error
                    }
                }
                Write-Log "Patched $patchedCount config file(s) in ESP"

                $script:WipeEfiName = "BOOTx64.EFI"
                foreach ($candidate in @("shimx64.efi", "grubx64.efi")) {
                    if (Test-Path "$distroEspDir\$candidate") {
                        $script:WipeEfiName = $candidate
                        break
                    }
                }
                Write-Log "Boot binary: $($script:WipeEfiName)"

                $script:WipeWinEspDrive = $winEspDrive
                $script:WipeBootInstalled = $true

                if ($removeLetter -and $winEspLetter) {
                    $script:WipeEspRemoveLetter = $true
                    $script:WipeEspLetter = $winEspLetter
                    $script:WipeEspPartition = $winEspPart
                } else {
                    $script:WipeEspRemoveLetter = $false
                }

                Write-Log "Bootloader installed to Windows ESP at $($script:WipeEspDistroDir)"
            } catch {
                Write-Log "Failed to install bootloader to Windows ESP: $_" -Error
                Write-Log "You may need to configure boot manually in UEFI/BIOS settings" -Error
            }
        }

        if ($autoRestartCheck.Checked) {
            Write-Log "Configuring UEFI boot priority..."
            Set-Status "Configuring UEFI boot priority..."

            try {
                if ($useRefind -and $script:RefindDriveLetter) {
                    # rEFInd boot entry - point to the rEFInd partition
                    Write-Log "Creating UEFI boot entry for rEFInd..."
                    $refindDrive = "$($script:RefindDriveLetter):"
                    $bootCreated = New-UefiBootEntry -DistroName "rEFInd - ULLI" `
                        -DevicePartition $refindDrive -EfiPath "\EFI\BOOT\BOOTx64.EFI"

                    if ($bootCreated) {
                        Write-Log "rEFInd UEFI boot entry created and set as default!"
                    } else {
                        Write-Log "Could not create rEFInd boot entry automatically" -Error
                        Write-Log "You will need to select 'rEFInd - ULLI' manually in UEFI/BIOS boot menu" -Error
                    }
                } else {
                    # Standard boot entry (no rEFInd)
                    $bcdeditOutput = bcdedit /enum firmware 2>&1
                    $lines = $bcdeditOutput

                    $bootEntries = @()
                    $currentEntry = $null

                    foreach ($line in $lines) {
                        if ($line -match '^Firmware Application \(') {
                            if ($currentEntry) {
                                $bootEntries += $currentEntry
                            }
                            $currentEntry = @{}
                        }
                        elseif ($line -match '^identifier\s+(.+)$') {
                            if ($currentEntry) {
                                $currentEntry.ID = $matches[1].Trim()
                            }
                        }
                        elseif ($line -match '^description\s+(.+)$') {
                            if ($currentEntry) {
                                $currentEntry.Description = $matches[1].Trim()
                            }
                        }
                    }
                    if ($currentEntry) {
                        $bootEntries += $currentEntry
                    }

                    Write-Log "Found $($bootEntries.Count) firmware boot entries:"
                    foreach ($entry in $bootEntries) {
                        Write-Log "  $($entry.Description) [$($entry.ID)]"
                    }

                    $distroKeyword = $distro.Keyword

                    $targetEntry = $null

                    $targetEntry = $bootEntries | Where-Object { $_.Description -like "*$distroKeyword*" } | Select-Object -First 1
                    if ($targetEntry) {
                        Write-Log "Found existing boot entry for '$distroKeyword'"
                    }

                    if (-not $targetEntry) {
                        $targetEntry = $bootEntries | Where-Object { $_.Description -like '*UEFI OS*' } | Select-Object -First 1
                        if ($targetEntry) {
                            Write-Log "Found generic 'UEFI OS' boot entry"
                        }
                    }

                    if ($targetEntry) {
                        Write-Log "Setting boot priority to: $($targetEntry.Description) [$($targetEntry.ID)]"

                        $process = Start-Process -FilePath "bcdedit.exe" `
                            -ArgumentList "/set", "{fwbootmgr}", "default", $targetEntry.ID `
                            -Wait -PassThru -NoNewWindow

                        if ($process.ExitCode -eq 0) {
                            Write-Log "UEFI boot priority set successfully!"
                        } else {
                            Write-Log "bcdedit /set default returned exit code $($process.ExitCode)" -Error
                        }
                    } else {
                        Write-Log "No existing boot entry found for $distroName"
                        Write-Log "Creating new UEFI firmware boot entry..."
                        $bootCreated = $false

                        if ($script:WipeBootInstalled) {
                            Write-Log "Creating firmware boot entry (Windows ESP)..."
                            $wipeEfiPath = "$($script:WipeEspDistroDir)\$($script:WipeEfiName)"
                            $bootCreated = New-UefiBootEntry -DistroName $distroName `
                                -DevicePartition $script:WipeWinEspDrive -EfiPath $wipeEfiPath

                            # Clean up ESP drive letter if we assigned it
                            if ($script:WipeEspRemoveLetter -and $script:WipeEspLetter) {
                                $script:WipeEspPartition | Remove-PartitionAccessPath -AccessPath "$($script:WipeEspLetter):\" -ErrorAction SilentlyContinue
                            }
                        } else {
                            $bootDeviceDrive = $script:NewDrive
                            Write-Log "Boot entry will point to partition: $bootDeviceDrive"
                            Write-Log "Attempting bcdedit /copy method..."
                            $bootCreated = New-UefiBootEntry -DistroName $distroName `
                                -DevicePartition $bootDeviceDrive -EfiPath "\EFI\BOOT\BOOTx64.EFI"
                        }

                        if (-not $bootCreated) {
                            Write-Log "Could not create UEFI boot entry automatically" -Error
                            Write-Log "You will need to set boot priority manually in UEFI/BIOS settings" -Error
                            Write-Log "Or use the one-time boot menu (usually F12) to select the $distroName partition" -Error
                        }
                    }
                }
            }
            catch {
                Write-Log "Error configuring UEFI boot: $_" -Error
                Write-Log "You may need to set boot priority manually in UEFI/BIOS settings" -Error
            }
        }

        # Success
        Write-Log "====================================="
        Write-Log "Installation Complete!"
        Write-Log "====================================="
        Write-Log "$distroName boot partition created at drive $script:NewDrive"
        if ($customRadio.Checked) {
            Write-Log "ISO used: $(Split-Path -Leaf $script:CustomIsoPath)"
        }
        Write-Log ""
        Write-Log "*** DISK LAYOUT ***"
        $finalPartitions = Get-Partition -DiskNumber $targetDiskNumber | Sort-Object Offset
        foreach ($part in $finalPartitions) {
            $sizeGB = [math]::Round($part.Size / 1GB, 2)
            $label = if ($part.DriveLetter) { "Drive $($part.DriveLetter)" }
                    elseif ($part.Type -eq "Recovery" -or $part.GptType -match "de94bba4") { "Recovery" }
                    elseif ($part.IsSystem) { "System" }
                    else { "No letter" }
            Write-Log "- ${label}: $sizeGB GB"
        }
        Write-Log ""
        Write-Log "The unallocated space is ready for $distroName installation."
        Write-Log "The installer will automatically detect and use this space."
        Write-Log ""

        if ($useRefind -and $script:RefindDriveLetter) {
            Write-Log ""
            Write-Log "rEFInd boot manager has been installed and set as the default UEFI boot entry."
            Write-Log ""
        }

        if ($autoRestartCheck.Checked) {
            Write-Log "*** AUTOMATIC RESTART ENABLED ***"
            Write-Log "UEFI boot priority has been configured."
            Write-Log "The system will restart in 30 seconds!"
            if ($useRefind -and $script:RefindDriveLetter) {
                Write-Log "After restart, rEFInd should appear automatically and show `"$distroName`"."
            } else {
                Write-Log "After restart, the system will boot into $distroName"
            }
            Write-Log ""
        } else {
            if ($useRefind -and $script:RefindDriveLetter) {
                Write-Log "To boot ${distroName}:"
                Write-Log "1. Restart your computer"
                Write-Log "2. rEFInd should appear automatically and show `"$distroName`""
                Write-Log "3. If rEFInd doesn't appear, enter UEFI/BIOS (F2/F10/F12/DEL)"
                Write-Log "   and select `"rEFInd - ULLI`" from the boot menu"
                Write-Log "4. Disable Secure Boot if needed"
            } else {
                Write-Log "To boot $distroName, use the UEFI boot menu:"
                Write-Log "1. Restart your computer"
                Write-Log "2. Press F2, F10, F12, DEL, or ESC during startup"
                Write-Log "3. Select the $distroName entry"
                Write-Log "4. Make sure Secure Boot is disabled"
            }
            Write-Log ""
        }

        Set-Status "Installation complete!"

        # Delete ISO if requested
        if ($deleteIsoCheck.Checked -and -not $customRadio.Checked) {
            try {
                Remove-Item $script:IsoPath -Force
                Write-Log "ISO file deleted."
            }
            catch {
                Write-Log "Could not delete ISO file."
            }
        }

        # Auto-restart if enabled
        if ($autoRestartCheck.Checked) {
            Write-Log ""
            Write-Log "Preparing for automatic restart..."

            $countdownForm = New-Object System.Windows.Forms.Form
            $countdownForm.Text = "System Restart"
            $countdownForm.Size = New-Object System.Drawing.Size(420, 210)
            $countdownForm.StartPosition = "CenterScreen"
            $countdownForm.FormBorderStyle = "FixedDialog"
            $countdownForm.MaximizeBox = $false
            $countdownForm.MinimizeBox = $false

            $countdownLabel = New-Object System.Windows.Forms.Label
            $countdownLabel.Text = "System will restart in 30 seconds...`n`nUEFI boot priority has been configured.`nThe system will boot directly into $distroName."
            $countdownLabel.Font = New-Object System.Drawing.Font("Segoe UI", 10)
            $countdownLabel.Location = New-Object System.Drawing.Point(20, 20)
            $countdownLabel.Size = New-Object System.Drawing.Size(380, 90)
            $countdownLabel.TextAlign = "MiddleCenter"
            $countdownForm.Controls.Add($countdownLabel)

            $cancelButton = New-Object System.Windows.Forms.Button
            $cancelButton.Text = "Cancel Restart"
            $cancelButton.Font = New-Object System.Drawing.Font("Segoe UI", 10)
            $cancelButton.Location = New-Object System.Drawing.Point(135, 120)
            $cancelButton.Size = New-Object System.Drawing.Size(150, 35)
            $countdownForm.Controls.Add($cancelButton)

            $script:CancelRestart = $false
            $cancelButton.Add_Click({
                $script:CancelRestart = $true
                $countdownForm.Close()
            })

            $timer = New-Object System.Windows.Forms.Timer
            $timer.Interval = 1000
            $script:CountdownSeconds = $script:Const.RestartCountdownSeconds

            $timer.Add_Tick({
                $script:CountdownSeconds--
                $countdownLabel.Text = "System will restart in $script:CountdownSeconds seconds...`n`nUEFI boot priority has been configured.`nThe system will boot directly into $distroName."

                if ($script:CountdownSeconds -le 0) {
                    $timer.Stop()
                    $countdownForm.Close()
                }
            })

            $timer.Start()
            $countdownForm.ShowDialog()
            $timer.Stop()

            if (-not $script:CancelRestart) {
                Write-Log "Restarting system..."
                Start-Sleep -Seconds 2
                Restart-Computer -Force
            } else {
                Write-Log "Restart cancelled by user"
                Write-Log "You can restart manually when ready"
            }
        }
    }
    catch {
        Write-Log "Installation error: $_" -Error
        Set-Status "Installation failed!"
    }
    finally {
        $script:IsRunning = $false
        Set-UILocked $false
    }
}

# Event handlers
$startButton.Add_Click({
    Start-Installation
})

$exitButton.Add_Click({
    if ($script:IsRunning) {
        $result = [System.Windows.Forms.MessageBox]::Show(
            "Installation is in progress. Are you sure you want to exit?",
            "Confirm Exit",
            [System.Windows.Forms.MessageBoxButtons]::YesNo,
            [System.Windows.Forms.MessageBoxIcon]::Warning
        )

        if ($result -eq [System.Windows.Forms.DialogResult]::Yes) {
            $form.Close()
        }
    } else {
        $form.Close()
    }
})

$customRadio.Add_CheckedChanged({
    if ($customRadio.Checked) {
        $customIsoTextbox.Enabled = $true
        $browseButton.Enabled = $true
        $distroCombo.Enabled = $false
    } else {
        $customIsoTextbox.Enabled = $false
        $browseButton.Enabled = $false
        $distroCombo.Enabled = $true
    }
})

$browseButton.Add_Click({
    $openFileDialog = New-Object System.Windows.Forms.OpenFileDialog
    $openFileDialog.Title = "Select Linux ISO File"
    $openFileDialog.Filter = "ISO Files (*.iso)|*.iso|All Files (*.*)|*.*"
    $openFileDialog.FilterIndex = 1
    $openFileDialog.RestoreDirectory = $true

    if ($openFileDialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        $script:CustomIsoPath = $openFileDialog.FileName
        $customIsoTextbox.Text = $script:CustomIsoPath

        $fileInfo = Get-Item $script:CustomIsoPath
        $fileSizeGB = [math]::Round($fileInfo.Length / 1GB, 2)
        Write-Log "Selected ISO: $(Split-Path -Leaf $script:CustomIsoPath)"
        Write-Log "File size: $fileSizeGB GB"
    }
})

# ─── 12. Entry point ─────────────────────────────────────────────────────────
# Initialize
Update-DiskInfo

# Show form
$form.ShowDialog() | Out-Null
