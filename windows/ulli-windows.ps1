# Linux Installer for Windows 10/11 UEFI Systems - Enhanced Edition with Auto-Restart
# PowerShell GUI Version - Fixed unit conversions for proper partition placement
# Run as Administrator: powershell -ExecutionPolicy Bypass -File linux_installer.ps1
# Distributions: Linux Mint 22.3 "Zena" (Cinnamon Edition), CachyOS Desktop, Ubuntu 26.04 LTS, Kubuntu 26.04 LTS, Linux Lite 8.0, Debian Live 13.6.0 KDE, Fedora 43 KDE
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
# Load the distro catalog relative to this script. Supports both development
# layout (<repo>\distros.json) and packaged layout (<script>\distros.json).
# If the external file is missing or malformed, fall back to the built-in catalog.
# ─── Distro catalog loader (called from section 3) ──────────────────────────
function Get-DistroCatalog {
    # 1. Try distros.json next to this script (release layout: the .json sits
    #    in the same directory as the .ps1).
    $scriptRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)
    $parentRoot = [System.IO.Directory]::GetParent($scriptRoot).FullName
    $candidates = @(
        (Join-Path $scriptRoot "distros.json"),
        (Join-Path $parentRoot "distros.json")
    ) | Select-Object -Unique
    foreach ($catalogPath in $candidates) {
        Write-Host "Checking distro catalog: $catalogPath"
        if (Test-Path -LiteralPath $catalogPath -PathType Leaf) {
            Write-Host "  Found distro catalog."
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
$subHeaderLabel.Text = "Mint 22.3, Ubuntu 26.04, Kubuntu 26.04, Linux Lite 8.0, Debian 13.6.0, or Fedora 43  |  No USB required"
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