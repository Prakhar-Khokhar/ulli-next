# Linux Installer for Windows 10/11 UEFI Systems - Enhanced Edition with Auto-Restart
# PowerShell GUI Version - Fixed unit conversions for proper partition placement
# Run as Administrator: powershell -ExecutionPolicy Bypass -File linux_installer.ps1
# Distributions: Linux Mint 22.3 "Zena" (Cinnamon Edition), CachyOS Desktop, Ubuntu 26.04 LTS, Kubuntu 26.04 LTS, Debian Live 13.7.0 KDE, Fedora 44 KDE
# Optional RefindPlus boot manager on a dedicated FAT32 partition with ext4 driver

#Requires -Version 5.1

# ─── Auto-elevate to Administrator ────────────────────────────────────────────
if (-not ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    try {
        Start-Process powershell.exe -ArgumentList @(
            "-ExecutionPolicy", "Bypass",
            "-File", "`"$PSCommandPath`""
        ) -Verb RunAs
    } catch {
        Write-Error "ERROR: Administrator privileges are required to run ULLI."
        Write-Warning "Please right-click the script and select 'Run as Administrator'."
        Read-Host "Press Enter to exit"
    }
    exit
}

# Add required assemblies
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

# Global variables
$script:MinPartitionSizeGB = 7
$script:MinLinuxSizeGB = 20
$script:RefindPlusUrl = "https://github.com/RefindPlusRepo/RefindPlus/releases/download/v0.14.2.AF/x64-RefindPlus_001402-AF.zip"
$script:RefindPlusFilename = "x64-RefindPlus_001402-AF.zip"
$script:RefindPlusSha256 = "8e3aae2fd622e5f5632ef57f6cf8b692c14d278416c936da535779d2e4133434"
$script:RefindPlusSizeMB = 100  # 100 MB FAT32 partition for RefindPlus
$script:RefindPlusVersion = "v0.14.2.AF"

# ─── Distro Data Table ────────────────────────────────────────────────────────
$script:Distros = [ordered]@{
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
        Checksum      = "a081ab202cfda17f6924128dbd2de8b63518ac0531bcfe3f1a1b88097c459bd4"
        IsoFilename   = "linuxmint-22.3-cinnamon-64bit.iso"
        DownloadPage  = "https://linuxmint.com/edition.php?id=326"
        DownloadMsg   = "Please download Linux Mint 22.3 Cinnamon (64-bit) and save it as:"
        Keyword       = "Mint"
        ValidationFile = "casper\vmlinuz"
        IsHybrid      = $false
        ToRam         = $false
    }
    cachyos = @{
        Name          = "CachyOS Desktop"
        RadioLabel    = "CachyOS Desktop (approx. 3 GB)"
        ExpectedSize  = "approximately 3 GB"
        Mirrors       = @(
            "https://cdn77.cachyos.org/ISO/desktop/260809/cachyos-desktop-linux-260809.iso"
        )
        Checksum      = "959f6577f45e25ee9fd8c220fd221b08e4ea79412c7315c0f922dd6d86d5e33c"
        IsoFilename   = "cachyos-desktop-linux-260809.iso"
        DownloadPage  = "https://cachyos.org/download/"
        DownloadMsg   = "Please download CachyOS Desktop and save it as:"
        Keyword       = "CachyOS"
        ValidationFile = "arch\boot\x86_64\vmlinuz-linux-cachyos"
        IsHybrid      = $true
        ToRam         = $false
    }
    ubuntu = @{
        Name          = "Ubuntu 26.04 LTS"
        RadioLabel    = "Ubuntu 26.04 LTS - GNOME Edition (approx. 6.2GB)"
        ExpectedSize  = "approximately 6.1 GB"
        Mirrors       = @(
            "https://gsl-syd.mm.fcix.net/ubuntu-releases/26.04/ubuntu-26.04-desktop-amd64.iso",
            "https://mirror.xenyth.net/ubuntu-releases/26.04/ubuntu-26.04-desktop-amd64.iso",
            "https://ftp.udx.icscoe.jp/Linux/ubuntu-releases/26.04/ubuntu-26.04-desktop-amd64.iso"
        )
        Checksum      = "487f87faaf547ea30e0aba4d5b53346292571256b25333a978db1692bcee9dd2"
        IsoFilename   = "ubuntu-26.04-desktop-amd64.iso"
        DownloadPage  = "https://ubuntu.com/download/desktop"
        DownloadMsg   = "Please download Ubuntu 26.04 LTS (64-bit) and save it as:"
        Keyword       = "Ubuntu"
        ValidationFile = "casper\vmlinuz"
        IsHybrid      = $false
        ToRam         = $false
    }
    kubuntu = @{
        Name          = "Kubuntu 26.04 LTS"
        RadioLabel    = "Kubuntu 26.04 LTS - KDE Plasma 6 Edition (approx. 4.7 GB)"
        ExpectedSize  = "approximately 4.7 GB"
        Mirrors       = @(
            "https://ftp.linux.org.tr/kubuntu/26.04/release/kubuntu-26.04-desktop-amd64.iso",
            "https://www.mirrorservice.org/sites/cdimage.ubuntu.com/cdimage/kubuntu/releases/26.04/release/kubuntu-26.04-desktop-amd64.iso",
            "https://cdimage.ubuntu.com/kubuntu/releases/26.04/release/kubuntu-26.04-desktop-amd64.iso"
        )
        Checksum      = "95ce9cf68f13015b9a88bd1ef86fcf7eda77c99979fda48c69e28aa0a84f88ac"
        IsoFilename   = "kubuntu-26.04-desktop-amd64.iso"
        DownloadPage  = "https://kubuntu.org/getkubuntu/"
        DownloadMsg   = "Please download Kubuntu 26.04 LTS (64-bit) and save it as:"
        Keyword       = "Kubuntu"
        ValidationFile = "casper\vmlinuz"
        IsHybrid      = $false
        ToRam         = $false
    }
    debian = @{
        Name          = "Debian Live 13.7.0 KDE"
        RadioLabel    = "Debian Live 13.7.0 - KDE Edition (approx. 3.9 GB)"
        ExpectedSize  = "approximately 3.9 GB"
        Mirrors       = @(
            "https://cdimage.debian.org/debian-cd/current-live/amd64/iso-hybrid/debian-live-13.7.0-amd64-kde.iso",
            "https://mirrors.edge.kernel.org/debian-cd/current-live/amd64/iso-hybrid/debian-live-13.7.0-amd64-kde.iso",
            "https://mirror.csclub.uwaterloo.ca/debian-cd/current-live/amd64/iso-hybrid/debian-live-13.7.0-amd64-kde.iso"
        )
        Checksum      = "befe10dcea16f56f711a426c41d08d594a3e9b7a3c9d59d300eded33977228e7"
        IsoFilename   = "debian-live-13.7.0-amd64-kde.iso"
        DownloadPage  = "https://www.debian.org/CD/live/"
        DownloadMsg   = "Please download Debian Live 13.7.0 KDE (amd64) and save it as:"
        Keyword       = "Debian"
        ValidationFile = "live\vmlinuz"
        IsHybrid      = $true
        ToRam         = $false
    }
    linuxlite = @{
        Name          = "Linux Lite 8.0"
        RadioLabel    = "Linux Lite 8.0 - XFCE Desktop (approx. 2.36 GB)"
        ExpectedSize  = "approximately 2.36 GB"
        Mirrors       = @(
            "https://mirrors.sjtug.sjtu.edu.cn/linuxliteos/isos/8.0/linux-lite-8.0-64bit.iso",
            "https://mirror.lagoon.nc/linuxlite/linuxlite/isos/8.0/linux-lite-8.0-64bit.iso"
        )
        Checksum      = "7cfc63baf597156a0a5ecac87e860aff3967279694b19fa67fb410a34802857e"
        IsoFilename   = "linux-lite-8.0-64bit.iso"
        DownloadPage  = "https://www.linuxliteos.com/download.php"
        DownloadMsg   = "Please download Linux Lite 8.0 (64-bit) and save it as:"
        Keyword       = "Linux Lite"
        ValidationFile = "casper\vmlinuz"
        IsHybrid      = $false
        ToRam         = $false
    }
    zorin = @{
        Name          = "Zorin OS 18.1"
        RadioLabel    = "Zorin OS 18.1 - Core Desktop (approx. 3.6 GB)"
        ExpectedSize  = "approximately 3.6 GB"
        Mirrors       = @(
            "https://mirrors.dotsrc.org/zorinos/18/Zorin-OS-18.1-Core-64-bit.iso",
            "https://mirror.metanet.ch/zorinos/18/Zorin-OS-18.1-Core-64-bit.iso",
            "https://ftp.lysator.liu.se/pub/zorinos/isos/18/Zorin-OS-18.1-Core-64-bit.iso",
            "https://ftp.yz.yamagata-u.ac.jp/pub/linux/zorinos-isos/18/Zorin-OS-18.1-Core-64-bit.iso"
        )
        Checksum      = "44649b97bd307fc4c8529205d098ebbf98575a1e1ba2ee7d7005b697af1721d5"
        IsoFilename   = "Zorin-OS-18.1-Core-64-bit.iso"
        DownloadPage  = "https://zorin.com/os/download/"
        DownloadMsg   = "Please download Zorin OS 18.1 Core (64-bit) and save it as:"
        Keyword       = "Zorin OS"
        ValidationFile = "casper\vmlinuz"
        IsHybrid      = $false
        ToRam         = $true
    }
    fedora = @{
        Name          = "Fedora 44 KDE"
        RadioLabel    = "Fedora 44 - KDE Plasma Desktop (approx. 3.0 GB)"
        ExpectedSize  = "approximately 3.0 GB"
        Mirrors       = @(
            "https://ftp.kaist.ac.kr/pub/fedora/linux/releases/44/KDE/x86_64/iso/Fedora-KDE-Desktop-Live-44-1.7.x86_64.iso",
            "https://gsl-syd.mm.fcix.net/fedora/linux/releases/44/KDE/x86_64/iso/Fedora-KDE-Desktop-Live-44-1.7.x86_64.iso",
            "https://muug.ca/mirror/fedora/linux/releases/44/KDE/x86_64/iso/Fedora-KDE-Desktop-Live-44-1.7.x86_64.iso"
        )
        Checksum      = "c8295961d4c41adbf785a31a17c21a971d3b7415fda72dcad0c11c49577bf03a"
        IsoFilename   = "Fedora-KDE-Desktop-Live-44-1.7.x86_64.iso"
        DownloadPage  = "https://fedoraproject.org/kde/download/"
        DownloadMsg   = "Please download Fedora 44 KDE Plasma Desktop (x86_64) and save it as:"
        Keyword       = "Fedora"
        ValidationFile = "LiveOS\squashfs.img"
        IsHybrid      = $true
        ToRam         = $true
    }
}

$script:IsoPath = ""
$script:CustomIsoPath = ""
$script:IsRunning = $false
$script:MaxAvailableGB = 10000
# Effective boot (LINUX_LIVE) partition size in GB. Starts at the 7 GB
# minimum; the disk plan dialog lets the user raise it (e.g. ISO size plus
# headroom for persistence). Kept at script scope so the plan dialog, its
# callbacks, and Start-Installation all agree on one value.
$script:BootPartSizeGB = $script:MinPartitionSizeGB

# Detect screen resolution and adapt window size
$primaryScreen = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$scrH = $primaryScreen.Height
if ($scrH -le 900) {
    $formW = 720
    $formH = [Math]::Min($scrH - 60, 645)
} else {
    $formW = 720
    $formH = 665
}

# Create main form
$form = New-Object System.Windows.Forms.Form
$form.Text = "ULLI-Next"
$form.Size = New-Object System.Drawing.Size($formW, $formH)
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "Sizable"
$form.MinimumSize = New-Object System.Drawing.Size(720, 480)
$form.AutoScroll = $true
$form.Icon = [System.Drawing.SystemIcons]::Application

# Create fonts
$headerFont = New-Object System.Drawing.Font("Segoe UI", 16, [System.Drawing.FontStyle]::Bold)
$normalFont = New-Object System.Drawing.Font("Segoe UI", 9)
$boldFont = New-Object System.Drawing.Font("Segoe UI", 9, [System.Drawing.FontStyle]::Bold)

# Header label
$headerLabel = New-Object System.Windows.Forms.Label
$headerLabel.Text = "ULLI-Next, by Prakhar Khokhar"
$headerLabel.Font = $headerFont
$headerLabel.ForeColor = [System.Drawing.Color]::FromArgb(135, 185, 74)
$headerLabel.Location = New-Object System.Drawing.Point(10, 10)
$headerLabel.Size = New-Object System.Drawing.Size(680, 30)
$headerLabel.TextAlign = "MiddleCenter"
$form.Controls.Add($headerLabel)

# Sub-header label
$subHeaderLabel = New-Object System.Windows.Forms.Label
$subHeaderLabel.Text = "Mint 22.3, Ubuntu 26.04, Kubuntu 26.04, Debian 13.7.0, or Fedora 44  |  No USB required"
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

# ─── Distro dropdown list ─────────────────────────────────────────────────────
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

$distroCombo.Add_SelectedIndexChanged({
    try {
        $idx = $distroCombo.SelectedIndex
        if ($idx -ge 0 -and $idx -lt $script:DistroKeys.Count) {
            $selected = $script:Distros[$script:DistroKeys[$idx]]

            # RAM-boot notice for distros whose live environment is loaded into RAM
            if ($selected.ToRam) {
                $sizeMatch = [regex]::Match([string]$selected.ExpectedSize, '([\d.]+)\s*GB')
                $isoGB = 0.0
                if ($sizeMatch.Success) { $isoGB = [double]$sizeMatch.Groups[1].Value }
                $recGB = [math]::Round($isoGB + 3)
                $sizeText = if ($isoGB -gt 0) { "about $isoGB GB" } else { "an unknown size" }
                $ramMsg = "$($selected.Name) will be loaded into RAM when it boots.`n`n" +
                          "For a smoother installation, you should have about $recGB GB of available RAM " +
                          "(the ISO is $sizeText, plus roughly 3 GB).`n`n" +
                          "If your computer has less RAM than that, consider choosing a different distribution.`n`n" +
                          "This is only a recommendation, not a strict requirement. You can still continue with less RAM."
                [System.Windows.Forms.MessageBox]::Show($ramMsg, "RAM boot notice", [System.Windows.Forms.MessageBoxButtons]::OK, [System.Windows.Forms.MessageBoxIcon]::Information) | Out-Null
            }
        }
    } catch {
        # Don't let UI warnings break the app; log and continue
        Write-LogMessage "Error showing distro warning: $_" -IsError
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
$logBox.Size = New-Object System.Drawing.Size(660, 60)
$logGroup.Controls.Add($logBox)

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

# RefindPlus checkbox
$refindCheck = New-Object System.Windows.Forms.CheckBox
$refindCheck.Text = "Install RefindPlus boot manager  -  may require disabling Secure Boot on some systems"
$refindCheck.Font = $normalFont
$refindCheck.Location = New-Object System.Drawing.Point(10, 503)
$refindCheck.Size = New-Object System.Drawing.Size(680, 25)
$refindCheck.Checked = $false
$form.Controls.Add($refindCheck)

$refindCheck.Add_CheckedChanged({
    if ($refindCheck.Checked) {
        $refindInfo = "RefindPlus is a great bootloader and is ideal for dual booting.`n`n" +
                      "It may require disabling Secure Boot on some systems.`n`n" +
                      "For more information, see:`nhttps://github.com/RefindPlusRepo/RefindPlus"
        [System.Windows.Forms.MessageBox]::Show(
            $refindInfo,
            "RefindPlus boot manager",
            [System.Windows.Forms.MessageBoxButtons]::OK,
            [System.Windows.Forms.MessageBoxIcon]::Information
        ) | Out-Null
    }
})

# Live-only checkbox
$liveOnlyCheck = New-Object System.Windows.Forms.CheckBox
$liveOnlyCheck.Text = "Live environment only - test distros without installing Linux (only needs the boot partition space)"
$liveOnlyCheck.Font = $normalFont
$liveOnlyCheck.Location = New-Object System.Drawing.Point(10, 528)
$liveOnlyCheck.Size = New-Object System.Drawing.Size(680, 25)
$liveOnlyCheck.Checked = $false
$form.Controls.Add($liveOnlyCheck)

# Start button
$startButton = New-Object System.Windows.Forms.Button
$startButton.Text = "Start Installation"
$startButton.Font = $boldFont
$startButton.Location = New-Object System.Drawing.Point(390, 560)
$startButton.Size = New-Object System.Drawing.Size(140, 35)
$startButton.BackColor = [System.Drawing.Color]::FromArgb(135, 185, 74)
$startButton.ForeColor = [System.Drawing.Color]::White
$startButton.FlatStyle = "Flat"
$form.Controls.Add($startButton)

# Exit button
$exitButton = New-Object System.Windows.Forms.Button
$exitButton.Text = "Exit"
$exitButton.Font = $normalFont
$exitButton.Location = New-Object System.Drawing.Point(540, 560)
$exitButton.Size = New-Object System.Drawing.Size(140, 35)
$form.Controls.Add($exitButton)

# ============================================================
# HELPER FUNCTIONS
# ============================================================

function Write-LogMessage {
    param(
        [string]$Message,
        [switch]$IsError
    )

    $timestamp = Get-Date -Format "HH:mm:ss"
    $fullMessage = "[$timestamp] $Message"

    $logBox.AppendText("$fullMessage`r`n")
    $logBox.SelectionStart = $logBox.TextLength
    $logBox.ScrollToCaret()

    if ($IsError) {
        Write-Error $fullMessage
    } else {
        Write-Output $fullMessage
    }
}

function Set-Status {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$Status)
    if (-not $PSCmdlet.ShouldProcess("status bar", "update status")) { return }
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

# ─── RAM boot helpers ─────────────────────────────────────────────────
# ToRam is a per-distro switch (like IsHybrid). When enabled, the distro's
# live environment is loaded into RAM at boot. Each boot mechanism uses a
# different kernel parameter, so the mapping is centralized here instead of
# scattering distro names and parameter strings through the script.
function Get-RamBootParameter {
    param($Distro)
    if (-not $Distro -or -not $Distro.ToRam) { return $null }
    switch ($Distro.Keyword) {
        "Fedora"  { return "rd.live.ram=1" }  # dracut dmsquash-live module
        "CachyOS" { return "copytoram=y" }    # archiso (mkinitcpio-archiso)
        "Mint"    { return "toram" }          # casper
        "Ubuntu"  { return "toram" }          # casper
        "Kubuntu" { return "toram" }          # casper
        "Debian"  { return "toram" }          # live-boot
        "Linux Lite" { return "toram" }       # casper (Ubuntu-based live system)
        "Zorin OS"   { return "toram" }       # casper (Ubuntu-based live system)
        default   { return $null }            # unknown mechanism - never guess
    }
}

function Add-KernelParameterIfMissing {
    param(
        [string]$ArgsLine,
        [string]$Param
    )
    if (-not $Param) { return $ArgsLine }
    # Match the parameter name with or without an =value so we never add it twice
    $paramName = $Param -replace '=.*$', ''
    $pattern = '(^|\s)' + [regex]::Escape($paramName) + '(=[^\s]*)?(\s|$)'
    if ($ArgsLine -match $pattern) { return $ArgsLine }
    return ($ArgsLine + " " + $Param).Trim()
}

function Add-RamBootParameterToContent {
    param(
        [string]$Content,
        [string]$RamParam
    )
    if (-not $RamParam) { return $Content }
    $lines = $Content -split "`n"
    $newLines = @()
    foreach ($line in $lines) {
        # Kernel command lines: grub (linux/linuxefi), syslinux (append),
        # systemd-boot (options). -match is case-insensitive, so APPEND matches too.
        if ($line.Trim() -match '^(linux|linuxefi|append|options)\s') {
            # A bare "linux <path>" line (exactly two tokens) is a systemd-boot
            # kernel path, not a kernel command line - the command line lives on
            # the "options" line there. GRUB "linux"/"linuxefi" lines always carry
            # arguments after the kernel path, so only append when args are present.
            $trimmed = $line.Trim()
            $isLinuxLine = $trimmed -match '^(linux|linuxefi)\s'
            $tokenCount = ($trimmed -split '\s+').Count
            if (-not $isLinuxLine -or $tokenCount -gt 2) {
                $line = Add-KernelParameterIfMissing -ArgsLine $line -Param $RamParam
            }
        }
        $newLines += $line
    }
    return ($newLines -join "`n")
}

function Add-RamBootToBootConfig {
    param(
        [string]$RootPath,
        [hashtable]$Distro
    )
    $ramParam = Get-RamBootParameter -Distro $Distro
    if (-not $ramParam) { return 0 }
    $searchPaths = @(
        "EFI\BOOT\grub.cfg",
        "EFI\BOOT\BOOT.conf",
        "boot\grub2\grub.cfg",
        "boot\grub\grub.cfg",
        "isolinux\isolinux.cfg",
        "isolinux\grub.conf",
        "syslinux\syslinux.cfg",
        "syslinux\archiso_sys-linux.cfg",
        "syslinux\archiso_pxe-linux.cfg",
        "syslinux\archiso_sys.cfg",
        "syslinux\archiso_pxe.cfg",
        "loader\entries"
    )
    $patchedCount = 0
    $root = $RootPath.TrimEnd('\')
    foreach ($relPath in $searchPaths) {
        $full = "$root\$relPath"
        if (-not (Test-Path $full)) { continue }
        $files = @()
        if ((Get-Item $full).PSIsContainer) {
            $files = @(Get-ChildItem -Path $full -Filter "*.conf" -ErrorAction SilentlyContinue)
        } else {
            $files = @($full)
        }
        foreach ($file in $files) {
            try {
                $content = Get-Content $file.FullName -Raw -ErrorAction Stop
                $original = $content
                $content = Add-RamBootParameterToContent -Content $content -RamParam $ramParam
                if ($content -ne $original) {
                    Set-Content -Path $file.FullName -Value $content -Encoding UTF8 -Force
                    $patchedCount++
                }
            } catch {
                Write-LogMessage "Warning: Failed to patch $($file.Name) for RAM boot: $_" -IsError
            }
        }
    }
    return $patchedCount
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
        [switch]$UseRefind,
        [switch]$LiveOnly
    )
    $lines = @()
    foreach ($part in $Partitions) {
        $sGB = [math]::Round($part.Size / 1GB, 2)

        if ($ShrinkLetter -and $part.DriveLetter -eq $ShrinkLetter) {
            $label = Get-PartitionLabel -Part $part
            $lines += "  $label $NewShrinkSizeGB GB  (shrunk)"
            # Live-only mode never reserves space for a Linux install
            if (-not $LiveOnly) {
                $lines += "  [Unallocated - Linux]  $LinuxSizeGB GB  <-- for Linux installer"
            }
            if (-not $ShrinkLinuxOnly) {
                $lines += "  LINUX_LIVE (FAT32)     $BootPartSizeGB GB  <-- $DistroName live boot"
            }
            if ($UseRefind) {
                $lines += "  REFIND (FAT32)         0.1 GB  <-- RefindPlus boot manager"
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
            if ($LiveOnly) {
                $lines += "  [Unallocated - unused]  $RemainingFreeGB GB  <-- not needed for live-only"
            } else {
                $lines += "  [Unallocated - Linux]  $RemainingFreeGB GB  <-- for Linux installer"
            }
        }
        $lines += "  LINUX_LIVE (FAT32)     $BootPartSizeGB GB  <-- $DistroName live boot"
        if ($UseRefind) {
            $lines += "  REFIND (FAT32)         0.1 GB  <-- RefindPlus boot manager"
        }
    }

    if ($NoChanges) {
        $lines += ""
        $lines += "  (No changes - disk cannot be used as-is)"
    }

    return $lines
}

function Shrink-UllPartition {
    param(
        [string]$DriveLetter,
        [double]$ShrinkAmountGB,
        [int]$TargetDiskNumber
    )
    # Query and validate the partition BEFORE attempting any resize so the
    # original size and partition identity are retained for later verification.
    # Partition safety: only shrink the partition the approved plan targets
    # on the intended target disk. Never shrink the Windows system partition
    # or an unrelated volume merely because a drive letter (e.g. E:) appears
    # in a log line.
    try {
        $partition = Get-Partition -DriveLetter $DriveLetter -ErrorAction Stop
    } catch {
        Write-LogMessage "Failed to query partition ${DriveLetter}: $_" -IsError
        return $false
    }
    if (-not $partition) {
        Write-LogMessage "Refusing to shrink ${DriveLetter}: partition query returned no object." -IsError
        return $false
    }
    if ($partition.DiskNumber -ne $TargetDiskNumber) {
        Write-LogMessage "Refusing to shrink ${DriveLetter}: (Disk $($partition.DiskNumber)) - not on the approved target Disk $TargetDiskNumber." -IsError
        return $false
    }
    $partitionNumber = $partition.PartitionNumber
    if ($null -eq $partitionNumber -or $partitionNumber -le 0) {
        Write-LogMessage "Refusing to shrink ${DriveLetter}: unable to establish a valid partition number." -IsError
        return $false
    }
    $currentSize = $partition.Size
    if ($null -eq $currentSize) {
        Write-LogMessage "Refusing to shrink ${DriveLetter}: unable to determine the current partition size." -IsError
        return $false
    }
    # Query the supported size range before resizing so we never shrink
    # below the filesystem/Windows minimum.
    try {
        $supported = Get-PartitionSupportedSize -DriveLetter $DriveLetter -ErrorAction Stop
    } catch {
        Write-LogMessage "Failed to query supported size for ${DriveLetter}: $_" -IsError
        return $false
    }
    if (-not $supported) {
        Write-LogMessage "Failed to query supported size for ${DriveLetter}: no size range returned." -IsError
        return $false
    }
    $sizeMin = $supported.SizeMin
    if ($null -eq $sizeMin) {
        Write-LogMessage "Refusing to shrink ${DriveLetter}: unsupported minimum partition size." -IsError
        return $false
    }
    $shrinkBytes = [int64]($ShrinkAmountGB * 1GB)
    if ($shrinkBytes -le 0) {
        Write-LogMessage "Refusing to shrink ${DriveLetter}: shrink amount must be greater than zero." -IsError
        return $false
    }
    $newSize = $currentSize - $shrinkBytes
    if ($newSize -lt $sizeMin) {
        Write-LogMessage "Refusing to shrink ${DriveLetter}: target size $([math]::Round($newSize / 1GB, 2)) GB is below the supported minimum $([math]::Round($sizeMin / 1GB, 2)) GB." -IsError
        return $false
    }
    if ($newSize -gt $currentSize) {
        Write-LogMessage "Refusing to shrink ${DriveLetter}: computed size exceeds the current partition size." -IsError
        return $false
    }

    Write-LogMessage "Shrinking ${DriveLetter}: partition from $([math]::Round($currentSize / 1GB, 2)) GB to $([math]::Round($newSize / 1GB, 2)) GB..."

    $resizeSucceeded = $false

    # Attempt 1: Storage API.
    # Resize-Partition lives in the built-in Storage module (Windows PowerShell 5.1).
    # There is no 'Microsoft.PowerShell.Storage' module; qualifying with that name
    # fails with 'The module ... could not be loaded'. Call it unqualified so the
    # Storage module auto-loads, consistent with Get-Partition/Get-PartitionSupportedSize
    # used earlier in this function. Keep DiskPart as a fallback.
    try {
        Resize-Partition -DriveLetter $DriveLetter -Size $newSize -ErrorAction Stop
        $resizeSucceeded = $true
        Write-LogMessage "${DriveLetter}: partition shrunk successfully!"
    } catch {
        Write-LogMessage "Storage API shrink failed for ${DriveLetter}: $_" -IsError
    }

    # Attempt 2: diskpart fallback, ONLY if the Storage API did not succeed.
    # A post-resize verification failure later must NOT reach this block.
    if (-not $resizeSucceeded) {
        Write-LogMessage "Trying diskpart method..."
        $sizeMB = [int]($ShrinkAmountGB * 1024)
        $diskpartScript = @"
select volume $DriveLetter
shrink desired=$sizeMB
exit
"@
        $scriptPath = Join-Path $env:TEMP "shrink_script.txt"
        $diskpartExitCode = $null
        $result = $null
        try {
            $diskpartScript | Out-File -FilePath $scriptPath -Encoding ASCII -ErrorAction Stop
            $result = & diskpart /s $scriptPath 2>&1
            $diskpartExitCode = $LASTEXITCODE
        } catch {
            Write-LogMessage "DiskPart fallback failed for ${DriveLetter}: $_" -IsError
            return $false
        } finally {
            if (Test-Path $scriptPath) {
                Remove-Item $scriptPath -Force -ErrorAction SilentlyContinue
            }
        }

        $resultString = if ($null -ne $result) { ($result | ForEach-Object { "$_" }) -join "`n" } else { "" }
        if ($diskpartExitCode -ne 0 -or $resultString -notmatch "successfully") {
            $hint = if ($DriveLetter -eq 'C') {
                "You may need to: 1) Run disk cleanup 2) Disable hibernation (powercfg -h off) 3) Reboot"
            } else {
                "You may need to: 1) Run disk cleanup 2) Defragment the drive 3) Reboot"
            }
            Write-LogMessage "Failed to shrink ${DriveLetter}: partition!" -IsError
            Write-LogMessage $resultString -IsError
            Write-LogMessage $hint -IsError
            return $false
        }
        $resizeSucceeded = $true
        Write-LogMessage "${DriveLetter}: partition shrunk successfully!"
    }

    # Postcondition verification: re-query the SAME validated partition on the
    # approved target disk and confirm the actual reduction meets the requested
    # shrink amount. This runs after a reported-successful resize only; a
    # failure here returns failure WITHOUT re-invoking diskpart.
    if ($resizeSucceeded) {
        try {
            if ($null -eq $partitionNumber -or $partitionNumber -le 0) {
                throw "original partition number is not established"
            }
            # Re-query by the exact identity captured before resizing. Never fall
            # back to drive-letter-only matching: a drive letter can be reassigned
            # to a different volume after a resize, which would shrink the wrong
            # partition.
            $verifyPartition = Get-Partition -DiskNumber $TargetDiskNumber -PartitionNumber $partitionNumber -ErrorAction Stop
            if (-not $verifyPartition) {
                throw "partition not found on Disk $TargetDiskNumber Partition $partitionNumber"
            }
        } catch {
            Write-LogMessage "Post-shrink verification failed: could not re-query partition ${DriveLetter} on Disk $TargetDiskNumber, Partition $($partitionNumber): $_" -IsError
            return $false
        }
        # Explicitly verify the returned object is the intended partition on the
        # approved disk. Querying by identity does not guarantee correctness.
        if ($verifyPartition.DiskNumber -ne $TargetDiskNumber) {
            Write-LogMessage "Post-shrink verification failed: partition no longer resides on the approved target Disk $TargetDiskNumber." -IsError
            return $false
        }
        if ($verifyPartition.PartitionNumber -ne $partitionNumber) {
            Write-LogMessage "Post-shrink verification failed: re-queried partition number $($verifyPartition.PartitionNumber) does not match the original Partition $partitionNumber." -IsError
            return $false
        }
        $actualSize = $verifyPartition.Size
        if ($null -eq $actualSize) {
            Write-LogMessage "Post-shrink verification failed: unable to determine the re-queried partition size." -IsError
            return $false
        }
        $actualReduction = $currentSize - $actualSize
        $discrepancyBytes = $actualReduction - $shrinkBytes
        # Symmetric tolerance: diskpart shrinks in whole MiB and NTFS/filesystem
        # alignment may leave a few MiB unshrunk; 10 MiB covers truncation and
        # cluster alignment without masking a genuinely incorrect shrink.
        # Reject the result when the actual reduction differs from the requested
        # reduction by more than the tolerance in EITHER direction.
        $toleranceBytes = [int64](10MB)
        $lowerBound = $shrinkBytes - $toleranceBytes
        $upperBound = $shrinkBytes + $toleranceBytes

        Write-LogMessage "Post-shrink verification: Disk $TargetDiskNumber Partition $partitionNumber (${DriveLetter}:) - requested reduction $([math]::Round($shrinkBytes / 1MB, 0)) MiB, actual reduction $([math]::Round($actualReduction / 1MB, 0)) MiB, signed discrepancy $([math]::Round($discrepancyBytes / 1MB, 2)) MiB, tolerance $([math]::Round($toleranceBytes / 1MB, 0)) MiB, new size $([math]::Round($actualSize / 1GB, 4)) GB."

        if ($actualReduction -lt $lowerBound -or $actualReduction -gt $upperBound) {
            Write-LogMessage "Post-shrink verification failed: actual reduction $([math]::Round($actualReduction / 1GB, 4)) GB differs from requested reduction $([math]::Round($shrinkBytes / 1GB, 4)) GB by more than the $([math]::Round($toleranceBytes / 1MB, 0)) MiB tolerance (signed discrepancy $([math]::Round($discrepancyBytes / 1MB, 2)) MiB); partition now $([math]::Round($actualSize / 1GB, 4)) GB on Disk $TargetDiskNumber Partition $partitionNumber." -IsError
            return $false
        }
        Write-LogMessage "Post-shrink verified: ${DriveLetter}: reduced by $([math]::Round($actualReduction / 1GB, 2)) GB (now $([math]::Round($actualSize / 1GB, 2)) GB)."
    }

    return $true
}

function New-UefiBootEntry {
    [CmdletBinding(SupportsShouldProcess = $true)]
    [OutputType([string])]
    param(
        [string]$DistroName,
        [string]$DevicePartition,
        [string]$EfiPath
    )
    if (-not $PSCmdlet.ShouldProcess("UEFI boot entry", "create UEFI boot entry")) { return }
    try {
        $copyOutput = & bcdedit.exe /copy "{bootmgr}" /d "`"$DistroName`"" 2>&1
        $copyExitCode = $LASTEXITCODE
        $copyOutputStr = $copyOutput -join " "

        if ($copyExitCode -ne 0) {
            Write-LogMessage "bcdedit /copy failed with exit code $copyExitCode`: $copyOutputStr" -IsError
            return $null
        }

        # A realistic Windows /copy success looks like:
        #   "The entry was successfully copied to {11111111-1111-1111-1111-111111111111}."
        # Accept only a single, canonical GUID in braces. A malformed or
        # non-canonical identifier such as {123} must not be mistaken for an
        # entry GUID, and zero or multiple candidates are ambiguous -> fail closed.
        $guidPattern = '\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\}'

        if ($copyOutputStr -notmatch $guidPattern) {
            # bcdedit /copy may have created an entry, but its identity cannot be
            # determined from the output. Do not delete anything we cannot identify,
            # to avoid removing an unrelated firmware entry.
            Write-LogMessage "bcdedit /copy succeeded but no entry GUID could be parsed: $copyOutputStr" -IsError
            Write-LogMessage "A firmware entry may have been created but its identity is unknown; it will not be deleted or modified." -IsError
            return $null
        }
        $guidMatches = [regex]::Matches($copyOutputStr, $guidPattern)
        if ($guidMatches.Count -ne 1) {
            # Zero or more than one GUID: missing or ambiguous identity. Do not
            # guess which identifier belongs to the newly created entry, and never
            # delete an entry we cannot positively claim as our own.
            Write-LogMessage "bcdedit /copy output did not contain exactly one entry GUID ($($guidMatches.Count) found): $copyOutputStr" -IsError
            Write-LogMessage "A firmware entry may have been created but its identity is ambiguous; it will not be deleted or modified." -IsError
            return $null
        }
        $newGuid = $guidMatches[0].Value
        Write-LogMessage "Created new entry: $newGuid"

        $inheritedProps = @("default", "displayorder", "toolsdisplayorder", "timeout", "resumeobject", "inherit", "locale")
        foreach ($prop in $inheritedProps) {
            Start-Process "bcdedit.exe" -ArgumentList "/deletevalue", $newGuid, $prop -Wait -NoNewWindow -ErrorAction SilentlyContinue 2>$null | Out-Null
        }

        Write-LogMessage "Setting device=partition=$DevicePartition path=$EfiPath"

        $r1 = Start-Process "bcdedit.exe" -ArgumentList "/set", $newGuid, "device", "partition=$DevicePartition" -Wait -PassThru -NoNewWindow
        if ($r1.ExitCode -ne 0) {
            Write-LogMessage "Failed to set device for $newGuid (exit $($r1.ExitCode))" -IsError
            Start-Process "bcdedit.exe" -ArgumentList "/delete", $newGuid -Wait -NoNewWindow -ErrorAction SilentlyContinue | Out-Null
            return $null
        }
        $r2 = Start-Process "bcdedit.exe" -ArgumentList "/set", $newGuid, "path", $EfiPath -Wait -PassThru -NoNewWindow
        if ($r2.ExitCode -ne 0) {
            Write-LogMessage "Failed to set path for $newGuid (exit $($r2.ExitCode))" -IsError
            Start-Process "bcdedit.exe" -ArgumentList "/delete", $newGuid -Wait -NoNewWindow -ErrorAction SilentlyContinue | Out-Null
            return $null
        }
        $rDesc = Start-Process "bcdedit.exe" -ArgumentList "/set", $newGuid, "description", "`"$DistroName`"" -Wait -PassThru -NoNewWindow
        if ($rDesc.ExitCode -ne 0) {
            Write-LogMessage "Failed to set description for $newGuid (exit $($rDesc.ExitCode))" -IsError
            Start-Process "bcdedit.exe" -ArgumentList "/delete", $newGuid -Wait -NoNewWindow -ErrorAction SilentlyContinue | Out-Null
            return $null
        }

        # Making the entry the firmware default is the only step that changes
        # where the firmware boots on the next restart. Merely creating an entry
        # does not make the system boot Linux, so readiness requires this to
        # succeed and to be verified. The firmware is not guaranteed to honor it.
        $rDefault = Start-Process "bcdedit.exe" -ArgumentList "/set", "{fwbootmgr}", "default", $newGuid -Wait -PassThru -NoNewWindow
        if ($rDefault.ExitCode -ne 0) {
            Write-LogMessage "Failed to set firmware default to $newGuid (exit $($rDefault.ExitCode))" -IsError
            Start-Process "bcdedit.exe" -ArgumentList "/delete", $newGuid -Wait -NoNewWindow -ErrorAction SilentlyContinue | Out-Null
            return $null
        }

        $rOrder = Start-Process "bcdedit.exe" -ArgumentList "/displayorder", $newGuid, "/addlast" -Wait -PassThru -NoNewWindow
        if ($rOrder.ExitCode -ne 0) {
            Write-LogMessage "Warning: failed to add $newGuid to the firmware display order (exit $($rOrder.ExitCode)); it remains the firmware default." -IsError
        }

        if (-not (Test-UelliFirmwareDefault -Guid $newGuid)) {
            Write-LogMessage "Firmware default verification failed for $newGuid; the entry exists but may not be the next boot target." -IsError
            Start-Process "bcdedit.exe" -ArgumentList "/delete", $newGuid -Wait -NoNewWindow -ErrorAction SilentlyContinue | Out-Null
            return $null
        }

        Write-LogMessage "UEFI boot entry created and set as the firmware default: $newGuid"
        return $newGuid
    }
    catch {
        Write-LogMessage "Failed to create boot entry: $_" -IsError
        return $null
    }
}

function Test-UelliFirmwareDefault {
    param([string]$Guid)
    try {
        $output = bcdedit.exe /enum "{fwbootmgr}" 2>&1
    } catch {
        return $false
    }
    $str = ($output -join " ")
    if ($str -match 'default\s+(\{[0-9a-fA-F-]+\})') {
        return ($matches[1] -eq $Guid)
    }
    return $false
}

function Test-UelliEntryDevice {
    param(
        [hashtable]$Entry,
        [string]$BootDriveLetter,
        # Optional injectable resolver for native device paths. Defaults to
        # Get-UelliDriveLetterFromDevicePath; tests supply a mock so no real
        # system state is required. Fail-closed: an unresolvable device never
        # matches.
        [scriptblock]$ResolveDriveLetter = $null
    )
    $device = if ($Entry.ContainsKey('Device')) { $Entry.Device } else { $null }
    if (-not $device) { return $false }

    # Drive-letter form: partition=D:  (reliable, direct match)
    if ($device -imatch '^partition=([A-Z]):\s*$') {
        return ($matches[1] -ieq $BootDriveLetter)
    }

    # Native device path form: partition=\Device\HarddiskVolumeN.
    # Resolve to a drive letter before comparing; never infer a match from
    # string similarity, and never match when resolution fails.
    if ($device -imatch '^partition=(\\Device\\HarddiskVolume\d+)\s*$') {
        if (-not $ResolveDriveLetter) {
            $resolved = Get-UelliDriveLetterFromDevicePath -DevicePath $device
        } else {
            $resolved = & $ResolveDriveLetter -DevicePath $device
        }
        if (-not $resolved) { return $false }
        return ($resolved -ieq $BootDriveLetter)
    }

    # Any other device representation is unsupported or ambiguous -> fail closed.
    return $false
}

function Get-UelliDriveLetterFromDevicePath {
    param([string]$DevicePath)
    # Resolve a native device path such as partition=\Device\HarddiskVolumeN to
    # a Windows drive letter without guessing from the string.
    #
    # Method: QueryDosDevice (kernel32.dll) is the documented Win32 API for
    # translating MS-DOS device names (drive letters) to their NT device paths.
    # For each drive letter A-Z we ask Windows what NT path it maps to, and
    # return the letter only when the OS-reported mapping exactly equals the
    # requested \Device\HarddiskVolumeN. A drive letter is never inferred from
    # string similarity, partition numbers, or volume ordering.
    #
    # This has not been validated on a real Windows system (development and
    # testing run on Linux). kernel32.dll and QueryDosDevice are Windows-only;
    # on any failure - including the resolver being unavailable off-Windows -
    # the function returns $null so the caller treats the device as unresolved
    # (fail-closed: an entry whose device cannot be confirmed is never reused).
    if ($DevicePath -notmatch '^partition=(\\Device\\HarddiskVolume\d+)\s*$') {
        return $null
    }
    $targetDevice = $matches[1]

    $typeName = 'UelliNativeMethods'
    if (-not ([Type]::GetType($typeName))) {
        try {
            $typeDefinition = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class UelliNativeMethods {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    public static extern int QueryDosDevice(string lpDeviceName, StringBuilder lpTargetPath, int ucchMax);
}
'@
            $null = Add-Type -TypeDefinition $typeDefinition -ErrorAction Stop
        } catch {
            return $null
        }
    }

    foreach ($letter in 'A'..'Z') {
        $deviceName = "${letter}:"
        try {
            $sb = New-Object System.Text.StringBuilder 260
            $result = [UelliNativeMethods]::QueryDosDevice($deviceName, $sb, 260)
            if ($result -gt 0) {
                $ntPath = (($sb.ToString() -split [char]0)[0]).TrimEnd('\')
                if ($ntPath -ieq $targetDevice) {
                    return $letter
                }
            }
        } catch {
            # QueryDosDevice is Windows-only. On any failure skip this letter
            # and continue; never guess a mapping.
            continue
        }
    }

    return $null
}

function Select-UelliExistingBootEntry {
    param(
        [array]$BootEntries,
        [string]$DistroName,
        [string]$BootDriveLetter,
        # The EFI loader path the installer intends to use. An existing entry is
        # reused only when its path matches this intended loader path, so the
        # standard Linux-loader and RefindPlus paths stay distinct.
        [string]$EfiPath
    )
    # Do not select an entry as belonging to this installation on a broad
    # description substring alone. Only an exact description match that also
    # references the boot partition created by this run and uses the intended
    # EFI loader path is reused; anything ambiguous is left untouched and a new
    # entry is created instead.
    $exactMatches = @($BootEntries | Where-Object { $_.Description -eq $DistroName })
    if ($exactMatches.Count -eq 1) {
        $candidate = $exactMatches[0]
        if (Test-UelliEntryDevice -Entry $candidate -BootDriveLetter $BootDriveLetter) {
            $candidatePath = if ($candidate.ContainsKey('Path')) { $candidate.Path } else { $null }
            if (-not $candidatePath) {
                Write-LogMessage "Existing entry '$($candidate.Description)' has no EFI path recorded; treating as ambiguous." -IsError
                return $null
            }
            if ($candidatePath -ine $EfiPath) {
                Write-LogMessage "Existing entry '$($candidate.Description)' EFI path '$candidatePath' does not match the intended loader path '$EfiPath'; treating as ambiguous." -IsError
                return $null
            }
            return $candidate
        }
        Write-LogMessage "Existing entry '$($candidate.Description)' does not reference boot partition ${BootDriveLetter}:; treating as ambiguous." -IsError
        return $null
    }
    if ($exactMatches.Count -gt 1) {
        Write-LogMessage "Multiple existing entries match '$DistroName'; refusing to modify any of them (ambiguous)." -IsError
        return $null
    }
    return $null
}

function Get-FirmwareEntriesFromBcdedit {
    [OutputType([array])]
    param()
    $entries = @()
    $current = $null
    try {
        $lines = bcdedit /enum firmware 2>&1
    } catch {
        Write-LogMessage "Failed to enumerate firmware boot entries: $_" -IsError
        return $entries
    }
    foreach ($line in $lines) {
        if ($line -match '^Firmware Application \(') {
            if ($current) { $entries += $current }
            $current = @{}
        }
        elseif ($line -match '^identifier\s+(.+)$') {
            if ($current) { $current.ID = $matches[1].Trim() }
        }
        elseif ($line -match '^description\s+(.+)$') {
            if ($current) { $current.Description = $matches[1].Trim() }
        }
        elseif ($line -match '^device\s+(.+)$') {
            if ($current) { $current.Device = $matches[1].Trim() }
        }
        elseif ($line -match '^path\s+(.+)$') {
            if ($current) { $current.Path = $matches[1].Trim() }
        }
    }
    if ($current) { $entries += $current }
    return $entries
}

function Set-FirmwareDefault {
    param([string]$Guid)
    try {
        $process = Start-Process -FilePath "bcdedit.exe" -ArgumentList "/set", "{fwbootmgr}", "default", $Guid -Wait -PassThru -NoNewWindow
        if ($process.ExitCode -ne 0) {
            Write-LogMessage "bcdedit /set default returned exit code $($process.ExitCode)" -IsError
            return $false
        }
        return (Test-UelliFirmwareDefault -Guid $Guid)
    } catch {
        Write-LogMessage "Failed to set firmware default: $_" -IsError
        return $false
    }
}

function Configure-UelliBootEntry {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [string]$DistroName,
        [string]$BootDriveLetter,
        [string]$EfiPath,
        [switch]$UseRefind,
        [string]$RefindDriveLetter,
        [string]$RefindEfiPath = "\EFI\refindplus\x64_RefindPlus_REL.efi",
        # Injectable dependencies (defaults use the real Windows commands above).
        [scriptblock]$NewUefiBootEntry = $null,
        [scriptblock]$GetFirmwareEntries = $null,
        [scriptblock]$SetFirmwareDefault = $null
    )
    if (-not $PSCmdlet.ShouldProcess("UEFI boot entry", "configure UEFI boot entry")) { return }
    $script:uefiBootReady = $false
    $useRefindLocal = [bool]$UseRefind

    if (-not $NewUefiBootEntry) {
        $NewUefiBootEntry = {
            param($d, $dp, $ep)
            New-UefiBootEntry -DistroName $d -DevicePartition $dp -EfiPath $ep
        }
    }
    if (-not $GetFirmwareEntries) {
        $GetFirmwareEntries = { Get-FirmwareEntriesFromBcdedit }
    }
    if (-not $SetFirmwareDefault) {
        $SetFirmwareDefault = { param($g) Set-FirmwareDefault -Guid $g }
    }

    if ($UseRefind -and $RefindDriveLetter) {
        Write-LogMessage "Creating UEFI boot entry for RefindPlus..."
        $guid = & $NewUefiBootEntry "RefindPlus - ULLI" "${RefindDriveLetter}:" $RefindEfiPath
        if ($guid) {
            Write-LogMessage "RefindPlus UEFI boot entry created and set as the firmware default: $guid"
            $script:uefiBootReady = $true
        } else {
            Write-LogMessage "Could not create RefindPlus boot entry automatically; falling back to standard UEFI boot entry." -IsError
            $useRefindLocal = $false
        }
    }

    if (-not $useRefindLocal) {
        $entries = & $GetFirmwareEntries
        $target = Select-UelliExistingBootEntry -BootEntries $entries -DistroName $DistroName -BootDriveLetter $BootDriveLetter -EfiPath $EfiPath
        if ($target) {
            $ok = & $SetFirmwareDefault $target.ID
            if ($ok) {
                Write-LogMessage "UEFI boot default set to existing entry: $($target.Description)"
                $script:uefiBootReady = $true
            } else {
                Write-LogMessage "Failed to set firmware default to existing entry $($target.ID)" -IsError
            }
        } else {
            Write-LogMessage "No reliable existing boot entry for '$DistroName'; creating a new UEFI firmware boot entry..."
            $guid = & $NewUefiBootEntry $DistroName "${BootDriveLetter}:" $EfiPath
            if ($guid) {
                Write-LogMessage "New UEFI boot entry created and set as the firmware default: $guid"
                $script:uefiBootReady = $true
            } else {
                Write-LogMessage "Could not create UEFI boot entry automatically" -IsError
            }
        }
    }

    return [PSCustomObject]@{
        Ready    = [bool]$script:uefiBootReady
        UseRefind = $useRefindLocal
    }
}

function Get-UelliBootStatusText {
    param([bool]$BootReady)
    if ($BootReady) { return "Installation complete - boot default configured" }
    return "Installation finished - boot configuration incomplete"
}

function Test-UelliBootRestartEligible {
    param([bool]$AutoRestart, [bool]$BootReady)
    return ($AutoRestart -and $BootReady)
}

function Set-UILocked {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([bool]$Locked)
    if (-not $PSCmdlet.ShouldProcess("UI", "lock UI")) { return }
    $enabled = -not $Locked
    $startButton.Enabled = $enabled
    $exitButton.Enabled = $enabled
    $deleteIsoCheck.Enabled = $enabled
    $autoRestartCheck.Enabled = $enabled
    $customRadio.Enabled = $enabled
    $browseButton.Enabled = $enabled
    $distroCombo.Enabled = $enabled
    $refindCheck.Enabled = $enabled
    $liveOnlyCheck.Enabled = $enabled
}

function Update-DiskInfo {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param()
    if (-not $PSCmdlet.ShouldProcess("disk information", "update disk information")) { return }
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
        Write-LogMessage "Error getting disk information: $_" -IsError
        $diskInfoText.Text = "Error retrieving disk information"
    }
}

# ============================================================
# DISK PLAN DIALOG
# ============================================================
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
            } catch {
                Write-LogMessage "Warning: Failed to get volume info for drive $($part.DriveLetter): $_" -IsError
            }
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

function Show-DiskPlan {
    param(
        [string]$DistroName
    )

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
    $planForm.MinimumSize = New-Object System.Drawing.Size(720, 480)
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
    $sizeNumeric.Maximum = if ($script:MaxAvailableGB -gt $script:MinLinuxSizeGB) { $script:MaxAvailableGB } else { 10000 }
    $sizeNumeric.Value = 30
    $sizeGroup.Controls.Add($sizeNumeric)

    $sizeHelpLabel = New-Object System.Windows.Forms.Label
    $sizeHelpLabel.Text = "Minimum: 20 GB, Recommended: 100+ GB"
    $sizeHelpLabel.Font = $planFont
    $sizeHelpLabel.Location = New-Object System.Drawing.Point(240, 24)
    $sizeHelpLabel.Size = New-Object System.Drawing.Size(400, 20)
    $sizeGroup.Controls.Add($sizeHelpLabel)

    # Live-only mode never reserves space for a Linux install, so the size
    # picker is hidden and the dialog flows without its reserved space.
    $sizeGroup.Visible = -not $liveOnlyCheck.Checked
    if ($sizeGroup.Visible) { $yPos += 66 }

    # ---- BOOT / LIVE PARTITION SIZE ----
    # The boot partition holds the live system. 7 GB fits every supported ISO,
    # but users can raise it (ISO size + headroom) to leave room for
    # persistence or extra files. Defaults back to the 7 GB minimum.
    $bootSizeGroup = New-Object System.Windows.Forms.GroupBox
    $bootSizeGroup.Text = "Boot / Live Partition Size"
    $bootSizeGroup.Font = $planBoldFont
    $bootSizeGroup.Location = New-Object System.Drawing.Point(16, $yPos)
    $bootSizeGroup.Size = New-Object System.Drawing.Size(670, 56)
    $planForm.Controls.Add($bootSizeGroup)

    $bootSizeLabel = New-Object System.Windows.Forms.Label
    $bootSizeLabel.Text = "Size for live boot (GB):"
    $bootSizeLabel.Font = $planFont
    $bootSizeLabel.Location = New-Object System.Drawing.Point(10, 24)
    $bootSizeLabel.Size = New-Object System.Drawing.Size(130, 20)
    $bootSizeGroup.Controls.Add($bootSizeLabel)

    $bootSizeNumeric = New-Object System.Windows.Forms.NumericUpDown
    $bootSizeNumeric.Font = $planFont
    $bootSizeNumeric.Location = New-Object System.Drawing.Point(145, 22)
    $bootSizeNumeric.Size = New-Object System.Drawing.Size(80, 24)
    $bootSizeNumeric.Minimum = $script:MinPartitionSizeGB
    $bootSizeNumeric.Maximum = if ($script:MaxAvailableGB -gt $script:MinPartitionSizeGB) { $script:MaxAvailableGB } else { 10000 }
    $bootSizeNumeric.Value = $script:MinPartitionSizeGB
    $bootSizeGroup.Controls.Add($bootSizeNumeric)

    $bootSizeHelpLabel = New-Object System.Windows.Forms.Label
    $bootSizeHelpLabel.Font = $planFont
    $bootSizeHelpLabel.Location = New-Object System.Drawing.Point(240, 24)
    $bootSizeHelpLabel.Size = New-Object System.Drawing.Size(420, 20)
    $bootSizeGroup.Controls.Add($bootSizeHelpLabel)
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

    # ---- PARTITION TO SHRINK (C: disk only) ----
    $shrinkPartGroup = New-Object System.Windows.Forms.GroupBox
    $shrinkPartGroup.Text = "Partition to shrink"
    $shrinkPartGroup.Font = $planBoldFont
    $shrinkPartGroup.Location = New-Object System.Drawing.Point(16, $yPos)
    $shrinkPartGroup.Size = New-Object System.Drawing.Size(670, 56)
    $planForm.Controls.Add($shrinkPartGroup)

    $shrinkPartLabel = New-Object System.Windows.Forms.Label
    $shrinkPartLabel.Text = "Shrink this partition:"
    $shrinkPartLabel.Font = $planFont
    $shrinkPartLabel.Location = New-Object System.Drawing.Point(10, 24)
    $shrinkPartLabel.Size = New-Object System.Drawing.Size(130, 20)
    $shrinkPartGroup.Controls.Add($shrinkPartLabel)

    $script:ShrinkPartitionCombo = New-Object System.Windows.Forms.ComboBox
    $script:ShrinkPartitionCombo.Font = $planFont
    $script:ShrinkPartitionCombo.DropDownStyle = "DropDownList"
    $script:ShrinkPartitionCombo.Location = New-Object System.Drawing.Point(145, 22)
    $script:ShrinkPartitionCombo.Size = New-Object System.Drawing.Size(300, 24)
    $script:ShrinkPartitionCombo.Enabled = $false
    $script:ShrinkPartitionCombo.Add_SelectedIndexChanged({ & $updateAll })
    $shrinkPartGroup.Controls.Add($script:ShrinkPartitionCombo)

    $shrinkPartHelpLabel = New-Object System.Windows.Forms.Label
    $shrinkPartHelpLabel.Font = $planFont
    $shrinkPartHelpLabel.Location = New-Object System.Drawing.Point(455, 24)
    $shrinkPartHelpLabel.Size = New-Object System.Drawing.Size(210, 20)
    $shrinkPartGroup.Controls.Add($shrinkPartHelpLabel)
    $yPos += 66

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
    $script:UpdatingDiskPlan = $false
    $updateAll = {
        if ($script:UpdatingDiskPlan) { return }
        $selIndex = $diskCombo.SelectedIndex
        if ($selIndex -lt 0) { return }
        $script:UpdatingDiskPlan = $true
        try {
            $selDisk = $diskItems[$selIndex]
        $selDiskNum = $selDisk.Number
        $isTargetCDisk = $selDisk.IsCDisk

        $LinuxSizeGB = [int]$sizeNumeric.Value
        $script:BootPartSizeGB = [int]$bootSizeNumeric.Value
        $useRefind = $refindCheck.Checked
        $refindGB = if ($useRefind) { 0.1 } else { 0 }
        # Live-only mode bypasses the Linux install partition entirely: only
        # the boot partition (+ optional RefindPlus partition) needs space.
        $liveOnly = $liveOnlyCheck.Checked
        $totalNeededGB = if ($liveOnly) { $script:BootPartSizeGB + $refindGB } else { $LinuxSizeGB + $script:BootPartSizeGB + $refindGB }
        $sizeGroup.Visible = -not $liveOnly

        # Keep the Linux size picker honest: it can never exceed the free
        # space left after the chosen boot partition size.
        $linuxMaxGB = [math]::Max($script:MinLinuxSizeGB,
            [math]::Floor($script:CDriveInfo.FreeGB - $script:BootPartSizeGB - 10))
        $sizeNumeric.Maximum = if ($linuxMaxGB -gt $script:MinLinuxSizeGB) { $linuxMaxGB } else { 10000 }

        # Show the selected ISO's floor and the recommended size (ISO + 2 GB
        # headroom) so users know when to raise the 7 GB default.
        $isoHintGB = 0
        try {
            if ($customRadio.Checked -and $script:CustomIsoPath -and (Test-Path $script:CustomIsoPath)) {
                $isoHintGB = [math]::Round((Get-Item $script:CustomIsoPath).Length / 1GB, 2)
            } else {
                $hintDistro = Get-SelectedDistro
                if ($hintDistro -and $hintDistro.ExpectedSize) {
                    $isoSizeMatch = [regex]::Match([string]$hintDistro.ExpectedSize, '([\d.]+)\s*GB')
                    if ($isoSizeMatch.Success) { $isoHintGB = [double]$isoSizeMatch.Groups[1].Value }
                }
            }
        } catch {
            $isoHintGB = 0
        }
        if ($isoHintGB -gt 0) {
            $recBootGB = [math]::Ceiling($isoHintGB + 2)
            if ($script:BootPartSizeGB -lt ($isoHintGB + 0.2)) {
                $bootSizeHelpLabel.Text = "TOO SMALL for this ISO ($isoHintGB GB): needs about $recBootGB GB"
            } else {
                $bootSizeHelpLabel.Text = "Minimum: 7 GB. Recommended for this ISO: ~$recBootGB GB"
            }
        } else {
            $bootSizeHelpLabel.Text = "Minimum: 7 GB. Raise it if you need extra room (e.g. persistence)."
        }

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

            # ---- Partition to shrink picker (C: disk) ----
            # Let the user pick which NTFS partition on the Windows disk to shrink
            # instead of always targeting C:. Falls back to C: when nothing else is
            # shrinkable or when the combo has no valid choice.
            # Capture the user's currently chosen partition BEFORE clearing the
            # combo, otherwise SelectedIndex is -1 afterwards and the selection
            # snaps back to C: on every update.
            $prevSelLetter = $null
            if ($script:ShrinkPartitionCombo.SelectedIndex -ge 0) {
                $prevSelLetter = $script:ShrinkPartitionCombo.SelectedItem -replace '^([A-Z]): .*$', '$1'
            }
            if (-not $prevSelLetter -and $script:DiskPlanShrinkLetter) {
                $prevSelLetter = $script:DiskPlanShrinkLetter
            }
            $script:ShrinkPartitionCombo.Items.Clear()
            $diskParts = Get-Partition -DiskNumber $selDiskNum -ErrorAction SilentlyContinue | Sort-Object Offset
            if ($diskParts) {
                foreach ($p in $diskParts) {
                    if ($p.DriveLetter) {
                        try {
                            $vol = Get-Volume -DriveLetter $p.DriveLetter -ErrorAction Stop
                            if ($vol.FileSystem -eq "NTFS" -and $vol.SizeRemaining -gt (($totalNeededGB + 1) * 1GB)) {
                                $pSizeGB = [math]::Round($p.Size / 1GB, 2)
                                $pFreeGB = [math]::Round($vol.SizeRemaining / 1GB, 2)
                                $script:ShrinkPartitionCombo.Items.Add(
                                    "$($p.DriveLetter): ($pSizeGB GB, $pFreeGB GB free)"
                                ) | Out-Null
                            }
                        } catch {
                            Write-LogMessage "Warning: Failed to check partition $($p.DriveLetter): $_" -IsError
                        }
                    }
                }
            }
            $script:ShrinkPartitionCombo.Enabled = ($script:ShrinkPartitionCombo.Items.Count -gt 0)
            $selLetter = 'C'
            $newIndex = -1
            if ($script:ShrinkPartitionCombo.Items.Count -gt 0) {
                # Prefer the letter the user previously picked, if still present.
                if ($prevSelLetter) {
                    for ($i = 0; $i -lt $script:ShrinkPartitionCombo.Items.Count; $i++) {
                        if ($script:ShrinkPartitionCombo.Items[$i] -like "${prevSelLetter}: *") { $newIndex = $i; break }
                    }
                }
                # Fall back to C:, then to the first listed partition.
                if ($newIndex -lt 0) {
                    for ($i = 0; $i -lt $script:ShrinkPartitionCombo.Items.Count; $i++) {
                        if ($script:ShrinkPartitionCombo.Items[$i] -like "C: *") { $newIndex = $i; break }
                    }
                    if ($newIndex -lt 0) { $newIndex = 0 }
                }
                $script:ShrinkPartitionCombo.SelectedIndex = $newIndex
                $selLetter = $script:ShrinkPartitionCombo.Items[$newIndex] -replace '^([A-Z]): .*$', '$1'
                $shrinkPartHelpLabel.Text = "$($script:ShrinkPartitionCombo.Items.Count) shrinkable NTFS partition(s) on Disk $selDiskNum"
            } else {
                $shrinkPartHelpLabel.Text = "No shrinkable NTFS partitions on Disk $selDiskNum"
            }
            $script:DiskPlanShrinkLetter = $selLetter
            $shrinkPart = Get-Partition -DriveLetter $selLetter -ErrorAction SilentlyContinue
            $shrinkSizeGB = if ($shrinkPart) { [math]::Round($shrinkPart.Size / 1GB, 2) } else { $cSizeGB }
            $shrinkPartEnd = if ($shrinkPart) { $shrinkPart.Offset + $shrinkPart.Size } else { $cPartitionEnd }

            $canFreeAll = ($usableFreeGB -ge ($totalNeededGB + 1))
            $canFreeBoot = ($usableFreeGB -ge ($script:BootPartSizeGB + $refindGB + 1))

            $refindNote = if ($useRefind) { " + RefindPlus (0.1 GB)" } else { "" }
            if ($liveOnly) {
                # The freed gap must be larger than the boot partition plus the
                # gap scan's alignment/buffer, so shrink 1 GB more than needed.
                $liveShrinkAmountGB = $totalNeededGB + 1
                $radioShrink.Text = "Shrink ${selLetter}: by $liveShrinkAmountGB GB for the live boot partition ($script:BootPartSizeGB GB)$refindNote"
            } else {
                $radioShrink.Text = "Shrink ${selLetter}: by $totalNeededGB GB for Linux ($LinuxSizeGB GB) + boot ($script:BootPartSizeGB GB)$refindNote"
            }
            $radioShrink.Visible = $true
            $radioShrink.Enabled = $true

            if ($canFreeAll) {
                $radioFreeAll.Text = "Use existing unallocated space ($([math]::Round($usableFreeGB, 1)) GB available) - no shrink needed"
                $radioFreeAll.Visible = $true
                $radioFreeAll.Enabled = $true
            } elseif ($canFreeBoot) {
                $radioFreeAll.Text = "Use existing free space for $script:BootPartSizeGB GB boot partition, shrink C: by $LinuxSizeGB GB for Linux only"
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
            # use_free_boot shrinks C: (not the picker's partition); use_free_all
            # shrinks nothing. Keep DiskPlanShrinkLetter consistent with the
            # strategy so Start-Installation shrinks the right drive.
            if ($strategy -eq "use_free_boot") {
                $script:DiskPlanShrinkLetter = 'C'
            } elseif ($strategy -eq "use_free_all") {
                $script:DiskPlanShrinkLetter = $null
            }

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
                    $shrinkByGB = if ($liveOnly) { $liveShrinkAmountGB } else { $totalNeededGB }
                    $newCSizeGB = [math]::Round($shrinkSizeGB - $shrinkByGB, 2)
                    $step = 1
                    $changeLines += "  $step. Shrink ${selLetter}: partition from $shrinkSizeGB GB to $newCSizeGB GB  (-$shrinkByGB GB)"
                    $step++
                    $changeLines += "  $step. Create $script:BootPartSizeGB GB FAT32 boot partition (LINUX_LIVE) with $distroName files"
                    if ($useRefind) {
                        $step++
                        $changeLines += "  $step. Create 100 MB FAT32 RefindPlus partition"
                    }
                    $step++
                    $changeLines += "  $step. Leave $LinuxSizeGB GB unallocated for Linux installation"
                    $step++
                    if ($useRefind) {
                        $changeLines += "  $step. Install RefindPlus boot manager and configure UEFI boot entry"
                    } else {
                        $changeLines += "  $step. Configure UEFI boot entry for $distroName"
                    }

                    $afterLines = Format-AfterLayout -Partitions $partitions -DistroName $distroName `
                        -BootPartSizeGB $script:BootPartSizeGB -LinuxSizeGB $LinuxSizeGB `
                        -ShrinkLetter $selLetter -NewShrinkSizeGB $newCSizeGB -UseRefind:$useRefind
                }
                "use_free_all" {
                    $step = 1
                    $changeLines += "  $step. C: partition is NOT modified (stays at $cSizeGB GB)"
                    $step++
                    $changeLines += "  $step. Create $($script:BootPartSizeGB) GB FAT32 boot partition (LINUX_LIVE) in existing free space"
                    if ($useRefind) {
                        $step++
                        $changeLines += "  $step. Create 100 MB FAT32 RefindPlus partition"
                    }
                    $step++
                    $remainFreeGB = [math]::Round($usableFreeGB - $script:BootPartSizeGB - $refindGB, 1)
                    if ($liveOnly) {
                        $changeLines += "  $step. Boot directly into the live environment (nothing is installed)"
                    } else {
                        $changeLines += "  $step. Remaining ~$remainFreeGB GB stays unallocated for Linux"
                    }
                    $step++
                    if ($useRefind) {
                        $changeLines += "  $step. Install RefindPlus boot manager and configure UEFI boot entry"
                    } else {
                        $changeLines += "  $step. Configure UEFI boot entry for $distroName"
                    }

                    $afterLines = Format-AfterLayout -Partitions $partitions -DistroName $distroName `
                        -BootPartSizeGB $script:BootPartSizeGB -ShowUnchanged -AppendLinuxAndBoot `
                        -RemainingFreeGB $remainFreeGB -UseRefind:$useRefind -LiveOnly:$liveOnly
                }
                "use_free_boot" {
                    $newCSizeGB = [math]::Round($cSizeGB - $LinuxSizeGB, 2)
                    $step = 1
                    $changeLines += "  $step. Shrink C: partition from $cSizeGB GB to $newCSizeGB GB  (-$LinuxSizeGB GB)"
                    $step++
                    $changeLines += "  $step. Create $($script:BootPartSizeGB) GB FAT32 boot partition (LINUX_LIVE) in existing free space"
                    if ($useRefind) {
                        $step++
                        $changeLines += "  $step. Create 100 MB FAT32 RefindPlus partition"
                    }
                    $step++
                    $changeLines += "  $step. Leave $LinuxSizeGB GB (from C: shrink) unallocated for Linux"
                    $step++
                    if ($useRefind) {
                        $changeLines += "  $step. Install RefindPlus boot manager and configure UEFI boot entry"
                    } else {
                        $changeLines += "  $step. Configure UEFI boot entry for $distroName"
                    }

                    $afterLines = Format-AfterLayout -Partitions $partitions -DistroName $distroName `
                        -BootPartSizeGB $script:BootPartSizeGB -LinuxSizeGB $LinuxSizeGB `
                        -ShrinkLetter 'C' -NewShrinkSizeGB $newCSizeGB -ShrinkLinuxOnly
                    $afterLines += "  LINUX_LIVE (FAT32)     $script:BootPartSizeGB GB  <-- $distroName live boot"
                    if ($useRefind) {
                        $afterLines += "  REFIND (FAT32)         0.1 GB  <-- RefindPlus boot manager"
                    }
                }
            }

            $changesText.Text = ($changeLines -join "`r`n")
            $afterText.Text = ($afterLines -join "`r`n")

        } else {
            # ---- Other disk ----
            # Partition picker only applies to the Windows (C:) disk
            $script:ShrinkPartitionCombo.Items.Clear()
            $script:ShrinkPartitionCombo.Enabled = $false
            $shrinkPartHelpLabel.Text = "Picker applies to the Windows disk only"
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
                        } catch {
                            Write-LogMessage "Warning: Failed to check NTFS partition $($part.DriveLetter): $_" -IsError
                        }
                    }
                }
            }

            $diskFreeGB = $selDisk.FreeGB
            $hasFreeSpace = ($diskFreeGB -ge ($script:BootPartSizeGB + $refindGB + 1))
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
                        } catch {
                            Write-LogMessage "Warning: Failed to check partition $($part.DriveLetter): $_" -IsError
                        }
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
            $wipeMinGB = $script:BootPartSizeGB + $refindGB + 1
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
                # Live-only shrink needs the same 1 GB slack as the C: path so
                # the freed gap clears the gap scan's buffer requirement.
                $script:DiskPlanShrinkAmount = if ($liveOnly) { $totalNeededGB + 1 } else { $totalNeededGB }
            } else {
                $script:DiskPlanStrategy = "other_drive"
                $script:DiskPlanShrinkLetter = $null
                $script:DiskPlanShrinkAmount = 0
            }

            $changeLines = @()
            $afterLines = @()

            if ($usingWipe) {
                $usableGB = [math]::Round($selDisk.TotalGB - $script:BootPartSizeGB - $refindGB, 1)

                $changeLines += "  ** WARNING: This will ERASE ALL DATA on this disk! **"
                $changeLines += ""
                $step = 1
                $changeLines += "  $step. C: partition is NOT modified (different disk)"
                $step++
                $changeLines += "  $step. Wipe Disk $selDiskNum and create a new GPT partition table"
                $step++
                $changeLines += "  $step. Create $script:BootPartSizeGB GB FAT32 boot partition (LINUX_LIVE)"
                if ($useRefind) {
                    $step++
                    $changeLines += "  $step. Create 100 MB FAT32 RefindPlus partition"
                }
                $step++
                if ($liveOnly) {
                    $changeLines += "  $step. Leave ~$usableGB GB unallocated (unused - live-only mode)"
                } else {
                    $changeLines += "  $step. Leave ~$usableGB GB unallocated for Linux installation"
                }
                $step++
                if ($useRefind) {
                    $changeLines += "  $step. Install RefindPlus boot manager and configure UEFI boot entry"
                } else {
                    $changeLines += "  $step. Install bootloader to Windows ESP and configure UEFI boot entry for $DistroName"
                }

                $afterLines += "  LINUX_LIVE (FAT32)     $script:BootPartSizeGB GB  <-- $DistroName live boot"
                if ($useRefind) {
                    $afterLines += "  REFIND (FAT32)         0.1 GB  <-- RefindPlus boot manager"
                }
                if ($liveOnly) {
                    $afterLines += "  [Unallocated - unused]  ~$usableGB GB  <-- live-only, nothing installed"
                } else {
                    $afterLines += "  [Unallocated - Linux]  ~$usableGB GB  <-- for Linux installer"
                }
            } elseif ($usingShrink) {
                $shrinkTarget = $bestShrink
                $newPartSizeGB = [math]::Round($shrinkTarget.SizeGB - $totalNeededGB, 2)
                $step = 1
                $changeLines += "  $step. C: partition is NOT modified (different disk selected)"
                $step++
                $changeLines += "  $step. Shrink $($shrinkTarget.DriveLetter): from $($shrinkTarget.SizeGB) GB to $newPartSizeGB GB  (-$totalNeededGB GB)"
                $step++
                $changeLines += "  $step. Create $($script:BootPartSizeGB) GB FAT32 boot partition (LINUX_LIVE) on Disk $selDiskNum"
                if ($useRefind) {
                    $step++
                    $changeLines += "  $step. Create 100 MB FAT32 RefindPlus partition"
                }
                $step++
                if ($liveOnly) {
                    $changeLines += "  $step. Boot directly into the live environment (nothing is installed)"
                } else {
                    $changeLines += "  $step. Leave $LinuxSizeGB GB unallocated for Linux installation"
                }
                $step++
                if ($useRefind) {
                    $changeLines += "  $step. Install RefindPlus boot manager and configure UEFI boot entry"
                } else {
                    $changeLines += "  $step. Configure UEFI boot entry for $DistroName"
                }

                if ($partitions) {
                    $afterLines = Format-AfterLayout -Partitions $partitions -DistroName $DistroName `
                        -BootPartSizeGB $script:BootPartSizeGB -LinuxSizeGB $LinuxSizeGB `
                        -ShrinkLetter $shrinkTarget.DriveLetter -NewShrinkSizeGB $newPartSizeGB -UseRefind:$useRefind
                }
            } else {
                if ($hasFreeSpace) {
                    $step = 1
                    $changeLines += "  $step. C: partition is NOT modified (different disk selected)"
                    $step++
                    $changeLines += "  $step. Create $($script:BootPartSizeGB) GB FAT32 boot partition (LINUX_LIVE) on Disk $selDiskNum"
                    if ($useRefind) {
                        $step++
                        $changeLines += "  $step. Create 100 MB FAT32 RefindPlus partition"
                    }
                    $step++
                    if ($liveOnly) {
                        $changeLines += "  $step. Remaining unallocated space stays free (nothing is installed)"
                    } else {
                        $changeLines += "  $step. Remaining unallocated space on Disk $selDiskNum available for Linux"
                    }
                    $step++
                    if ($useRefind) {
                        $changeLines += "  $step. Install RefindPlus boot manager and configure UEFI boot entry"
                    } else {
                        $changeLines += "  $step. Configure UEFI boot entry for $DistroName"
                    }

                    $remainFreeGB = [math]::Round($diskFreeGB - $script:BootPartSizeGB - $refindGB, 1)
                    if ($partitions) {
                        $afterLines = Format-AfterLayout -Partitions $partitions -DistroName $DistroName `
                            -BootPartSizeGB $script:BootPartSizeGB -ShowUnchanged -AppendLinuxAndBoot `
                            -RemainingFreeGB $remainFreeGB -UseRefind:$useRefind -LiveOnly:$liveOnly
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
                            -BootPartSizeGB $script:BootPartSizeGB -NoChanges
                    }
                }
            }

            $changesText.Text = ($changeLines -join "`r`n")
            $afterText.Text = ($afterLines -join "`r`n")
        }
    } finally {
        $script:UpdatingDiskPlan = $false
    }
}

    # Wire events
    $diskCombo.Add_SelectedIndexChanged({
        $radioShrink.Checked = $true
        & $updateAll
    })
    $sizeNumeric.Add_ValueChanged({ & $updateAll })
    $bootSizeNumeric.Add_ValueChanged({ & $updateAll })
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
                $minFreeNeeded = $script:BootPartSizeGB + (if ($refindCheck.Checked) { 0.1 } else { 0 }) + 1
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
            } elseif ($strat -eq "other_drive" -and -not ($selDisk.FreeGB -ge ($script:BootPartSizeGB + (if ($refindCheck.Checked) { 0.1 } else { 0 }) + 1))) {
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
        LiveOnly = $liveOnlyCheck.Checked
        BootPartSizeGB = $script:BootPartSizeGB
    }
}

function Test-ISOChecksum {
    param(
        [string]$FilePath
    )

    Write-LogMessage "Verifying ISO checksum..."
    Set-Status "Verifying ISO integrity..."

    try {
        $distro = Get-SelectedDistro
        $expectedHash = $distro.Checksum
        Write-LogMessage "Expected SHA256: $expectedHash"

        Write-LogMessage "Calculating SHA256 checksum of downloaded ISO (this may take a minute)..."
        $actualHash = (Get-FileHash -Path $FilePath -Algorithm SHA256 -ErrorAction Stop).Hash.ToLower()
        Write-LogMessage "Actual SHA256:   $actualHash"

        if ($actualHash -eq $expectedHash) {
            Write-LogMessage "[PASS] Checksum verification PASSED - ISO is authentic!"
            return $true
        } else {
            Write-LogMessage "[FAIL] Checksum verification FAILED - ISO may be corrupted or tampered!" -IsError

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
                    Write-LogMessage "Corrupted ISO deleted"
                } catch {
                    Write-LogMessage "Error deleting ISO: $_" -IsError
                }
            }

            return $false
        }
    }
    catch {
        Write-LogMessage "Error calculating checksum: $_" -IsError

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

function Save-LinuxISO {
    param(
        [string]$Destination
    )

    $distro = Get-SelectedDistro
    $isoName = $distro.Name
    $expectedSize = $distro.ExpectedSize
    $mirrors = $distro.Mirrors

    Write-LogMessage "Downloading $isoName ISO ($expectedSize)..."
    Write-LogMessage "This may take a while depending on your internet speed..."

    foreach ($i in 0..($mirrors.Count - 1)) {
        $mirror = $mirrors[$i]
        Write-LogMessage "Trying mirror $($i + 1)/$($mirrors.Count): $($mirror.Split('/')[2])"
        Set-Status "Connecting to mirror..."

        try {
            Add-Type -AssemblyName System.Net.Http

            $httpClient = New-Object System.Net.Http.HttpClient
            $httpClient.Timeout = [TimeSpan]::FromMinutes(60)

            $response = $httpClient.GetAsync($mirror, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).Result

            if ($response.IsSuccessStatusCode) {
                $totalBytes = $response.Content.Headers.ContentLength
                $totalMB = [math]::Round($totalBytes / 1MB, 1)
                Write-LogMessage "File size: $totalMB MB"

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
                Write-LogMessage "Downloaded file size: $fileSizeGB GB"

                if ($fileInfo.Length -lt 2GB) {
                    Write-LogMessage "File size too small, download may be corrupted" -IsError
                    Remove-Item $Destination -Force
                    continue
                }

                if (-not (Test-ISOChecksum -FilePath $Destination)) {
                    Write-LogMessage "Checksum verification failed, trying next mirror..." -IsError
                    continue
                }

                return $true
            } else {
                throw "HTTP Error: $($response.StatusCode)"
            }
        }
        catch {
            Write-LogMessage "Download failed: $_" -IsError

            if (Test-Path $Destination) {
                try {
                    Remove-Item $Destination -Force -ErrorAction SilentlyContinue
                    Write-LogMessage "Removed incomplete download"
                } catch {
                    Write-LogMessage "Warning: Failed to remove incomplete download: $_" -IsError
                }
            }

            if ($i -lt $mirrors.Count - 1) {
                Write-LogMessage "Trying next mirror..."
            }
        }
    }

    # All mirrors failed
    Write-LogMessage "All automatic download attempts failed" -IsError

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
        Write-LogMessage $distro.DownloadMsg
        Write-LogMessage $Destination
        Write-LogMessage "Then run the installer again"
    }

    return $false
}

# ============================================================
# RefindPlus DOWNLOAD AND INSTALL
# ============================================================
function Save-RefindPlus {
    $dest = Join-Path $env:TEMP $script:RefindPlusFilename
    if (Test-Path $dest) {
        $cachedHash = (Get-FileHash -Path $dest -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash
        if ($cachedHash -eq $script:RefindPlusSha256) {
            Write-LogMessage "Found cached RefindPlus: $dest"
            return $dest
        }
        Write-LogMessage "Cached RefindPlus checksum mismatch; re-downloading..." -IsError
        Remove-Item $dest -Force -ErrorAction SilentlyContinue
    }
    Write-LogMessage "Downloading RefindPlus boot manager..."
    Set-Status "Downloading RefindPlus..."
    try {
        Add-Type -AssemblyName System.Net.Http
        $handler = New-Object System.Net.Http.HttpClientHandler
        $handler.AllowAutoRedirect = $true
        $httpClient = New-Object System.Net.Http.HttpClient($handler)
        $httpClient.Timeout = [TimeSpan]::FromMinutes(10)
        $httpClient.DefaultRequestHeaders.UserAgent.ParseAdd("windows-installer/1.0")

        $response = $httpClient.GetAsync(
            $script:RefindPlusUrl,
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
                        Set-Status "Downloading RefindPlus... $percent%"
                    }
                    [System.Windows.Forms.Application]::DoEvents()
                    $lastUpdate = $now
                }
            }

            $fileStream.Close()
            $downloadStream.Close()
            $response.Dispose()
            $httpClient.Dispose()

            $actualHash = (Get-FileHash -Path $dest -Algorithm SHA256 -ErrorAction Stop).Hash
            if ($actualHash -ne $script:RefindPlusSha256) {
                Write-LogMessage "RefindPlus checksum mismatch! Expected $($script:RefindPlusSha256), got $actualHash" -IsError
                Remove-Item $dest -Force -ErrorAction SilentlyContinue
                return $null
            }

            $sizeMB = [math]::Round((Get-Item $dest).Length / 1MB, 1)
            Write-LogMessage "RefindPlus downloaded and verified: $sizeMB MB"
            Set-Status ""
            return $dest
        } else {
            throw "HTTP Error: $($response.StatusCode)"
        }
    }
    catch {
        Write-LogMessage "RefindPlus download failed: $_" -IsError
        if (Test-Path $dest) { Remove-Item $dest -Force }
        return $null
    }
}

function Install-RefindPlus {
    param(
        [string]$RefindPlusDriveLetter,
        [string]$BootDriveLetter,
        [string]$DistroLabel,
        [string]$RamBootParam = ""
    )

    Write-LogMessage ""
    Write-LogMessage "== Installing RefindPlus boot manager =="

    $refindZip = Save-RefindPlus
    if (-not $refindZip) {
        Write-LogMessage "Cannot install RefindPlus without the download." -IsError
        return $false
    }

    # Extract RefindPlus
    Write-LogMessage "Extracting RefindPlus..."
    Set-Status "Extracting RefindPlus..."
    $extractDir = Join-Path $env:TEMP "refind_extract"
    if (Test-Path $extractDir) { Remove-Item $extractDir -Recurse -Force }

    try {
        Expand-Archive -Path $refindZip -DestinationPath $extractDir -Force

        $refindSrc = Join-Path $extractDir "x64-RefindPlus_001402-AF"
        $refindDrive = "${RefindPlusDriveLetter}:"
        $refindDir = Join-Path $refindDrive "EFI\refindplus"
        New-Item -Path $refindDir -ItemType Directory -Force | Out-Null
        $driversDir = Join-Path $refindDir "drivers_x64"
        New-Item -Path $driversDir -ItemType Directory -Force | Out-Null
        $iconsDir = Join-Path $refindDir "icons"
        New-Item -Path $iconsDir -ItemType Directory -Force | Out-Null

        Write-LogMessage "Copying RefindPlus files..."
        Set-Status "Installing RefindPlus..."

        # Copy RefindPlus EFI binary
        $srcEfi = Join-Path $refindSrc "x64_RefindPlus_REL.efi"
        $destEfi = Join-Path $refindDir "x64_RefindPlus_REL.efi"
        if (Test-Path $srcEfi) {
            Copy-Item $srcEfi $destEfi -Force
            Write-LogMessage "  Copied x64_RefindPlus_REL.efi"
        } else {
            Write-LogMessage "x64_RefindPlus_REL.efi not found in extracted archive!" -IsError
            return $false
        }

        # Copy filesystem drivers
        $driversSrc = Join-Path $refindSrc "OtherBinaries\Drivers"
        foreach ($drv in @("x64_ext4.efi", "x64_ext2.efi")) {
            $src = Join-Path $driversSrc $drv
            if (Test-Path $src) {
                Copy-Item $src $driversDir -Force
                Write-LogMessage "  Copied driver: $drv"
            }
        }

        # Copy icons
        $iconsSrc = Join-Path $refindSrc "OtherMisc\icons"
        if (Test-Path $iconsSrc) {
            robocopy $iconsSrc $iconsDir /E /R:2 /W:2 /NP /NFL /NDL | Out-Null
            Write-LogMessage "  Copied RefindPlus icons."
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
                            $kernelArgs = @()
                            for ($i = 2; $i -lt $parts.Count; $i++) {
                                $p = $parts[$i]
                                if ($p -match "^root=") { continue }
                                if ($p -match "CDLABEL=" -or $p -match "LABEL=") {
                                    $p = $p -replace "(CDLABEL=|LABEL=)\S+", '$1LINUX_LIVE'
                                }
                                $kernelArgs += $p
                            }
                            $extraArgs = $kernelArgs -join " "
                            break
                        }
                    }
                } catch {
                    Write-LogMessage ("Warning: Failed to extract kernel args from {0}: {1}" -f $full, $_) -IsError
                }
                if ($extraArgs) { break }
            }
        }

        # Write RefindPlus config
        Write-LogMessage "Writing RefindPlus configuration..."
        $conf = "# RefindPlus configuration - generated by ULLI`n"
        $conf += "timeout 10`n"
        $conf += "use_graphics_for linux`n"
        $conf += "scanfor internal,external,manual`n"
        $conf += "scan_all_linux_kernels false`n"
        $conf += "`n"

        if ($hasPxeboot) {
            if (-not $extraArgs) { $extraArgs = "rd.live.image rhgb quiet" }
            $extraArgs = Add-KernelParameterIfMissing -ArgsLine $extraArgs -Param $RamBootParam
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
            $extraArgs = Add-KernelParameterIfMissing -ArgsLine $extraArgs -Param $RamBootParam
            $conf += "menuentry `"$DistroLabel`" {`n"
            $conf += "  volume LINUX_LIVE`n"
            $conf += "  loader /casper/vmlinuz`n"
            $conf += "  initrd /casper/initrd`n"
            $conf += "  options `"boot=casper $extraArgs`"`n"
            $conf += "}`n"
        }
        elseif ($hasLive) {
            if (-not $extraArgs) { $extraArgs = "boot=live components quiet splash" }
            $extraArgs = Add-KernelParameterIfMissing -ArgsLine $extraArgs -Param $RamBootParam
            $conf += "menuentry `"$DistroLabel`" {`n"
            $conf += "  volume LINUX_LIVE`n"
            $conf += "  loader /live/vmlinuz`n"
            $conf += "  initrd /live/initrd.img`n"
            $conf += "  options `"$extraArgs`"`n"
            $conf += "}`n"
        }
        else {
            $conf += "# Unknown layout - RefindPlus will auto-scan.`n"
        }

        Set-Content -Path (Join-Path $refindDir "config.conf") -Value $conf -Encoding UTF8 -Force
        Write-LogMessage "RefindPlus configuration written."

        Write-LogMessage "RefindPlus installed successfully."
        Write-LogMessage "  RefindPlus partition: $refindDrive"
        return $true
    }
    catch {
        Write-LogMessage "RefindPlus installation failed: $_" -IsError
        return $false
    }
    finally {
        if (Test-Path $extractDir) {
            Remove-Item $extractDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

function New-RefindPartition {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [int]$DiskNumber,
        [int64]$AfterOffset = 0
    )
    if (-not $PSCmdlet.ShouldProcess("RefindPlus partition", "create RefindPlus partition")) { return }

    Write-LogMessage "Creating 100 MB RefindPlus partition..."
    Set-Status "Creating RefindPlus partition..."

    $refindSize = [int64]($script:RefindPlusSizeMB * 1MB)
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
            throw "Could not assign a drive letter to the RefindPlus partition"
        }

        Format-Volume -DriveLetter $refindDriveLetter `
            -FileSystem FAT32 `
            -NewFileSystemLabel "REFIND" `
            -Confirm:$false `
            -ErrorAction Stop | Out-Null

        Write-LogMessage "RefindPlus partition created as ${refindDriveLetter}: (REFIND)"
        return $refindDriveLetter
    }
    catch {
        Write-LogMessage "Failed to create RefindPlus partition: $_" -IsError

        # Fallback: try diskpart
        if ($alignedOffset -gt 0) {
            Write-LogMessage "Trying diskpart method for RefindPlus partition..."
            $offsetMB = [int64]([Math]::Floor($alignedOffset / 1MB))
            $sizeMB = $script:RefindPlusSizeMB

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
                        Write-LogMessage "RefindPlus partition created via diskpart as ${refindDriveLetter}: (REFIND)"
                        return $refindDriveLetter
                    }
                }
            }
        }

        Write-LogMessage "All RefindPlus partition creation methods failed" -IsError
        return $null
    }
}

function Start-Installation {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param()
    if (-not $PSCmdlet.ShouldProcess("target disk", "install Linux")) { return }
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
        Write-LogMessage "Installation cancelled by user at disk plan review."
        Set-Status "Ready to install"
        return
    }

    $selectedStrategy = $planResult.Strategy
    $targetDiskNumber = $planResult.TargetDiskNumber
    $isOtherDrive = ($selectedStrategy -eq "other_drive" -or $selectedStrategy -eq "other_drive_shrink" -or $selectedStrategy -eq "wipe_disk")
    $otherDriveShrinkLetter = $planResult.ShrinkDriveLetter
    $otherDriveShrinkAmountGB = $planResult.ShrinkAmountGB
    # Which partition on the Windows disk to shrink (defaults to C:). Set by the
    # partition picker in the disk plan dialog.
    $shrinkDriveLetter = if ($planResult.ShrinkDriveLetter) { $planResult.ShrinkDriveLetter } else { 'C' }
    $linuxSizeGB = $planResult.LinuxSizeGB
    $useRefind = $planResult.UseRefind
    $isLiveOnly = $planResult.LiveOnly
    # Effective boot partition size from the approved disk plan (default 7 GB,
    # user can raise it in the plan dialog for ISO + headroom).
    $bootPartSizeGB = if ($planResult.BootPartSizeGB -gt 0) { $planResult.BootPartSizeGB } else { $script:MinPartitionSizeGB }
    $refindGB = if ($useRefind) { 0.1 } else { 0 }
    # Live-only mode bypasses the Linux install partition entirely: only the
    # boot partition (+ optional RefindPlus partition) needs space, so no shrink
    # is made for Linux space.
    if ($isLiveOnly) {
        $totalNeededGB = $bootPartSizeGB + $refindGB
    } else {
        $totalNeededGB = $linuxSizeGB + $bootPartSizeGB + $refindGB
    }
    $refindNote = if ($useRefind) { ", RefindPlus: yes" } else { "" }
    $liveNote = if ($isLiveOnly) { ", Live-only: yes" } else { "" }
    Write-LogMessage "Disk plan approved. Strategy: $selectedStrategy, Target disk: $targetDiskNumber, Linux size: $linuxSizeGB GB, Boot partition: $bootPartSizeGB GB$refindNote$liveNote"

    # Now lock the UI and proceed
    $script:IsRunning = $true
    Set-UILocked $true

    # Reset UEFI boot readiness at the start of every installation so a previous
    # run's result can never leak into the current run. The flag is only set to
    # true after the required boot configuration for THIS installation is
    # verified as successfully established.
    $script:uefiBootReady = $false

    try {
        # Determine ISO path
        if ($customRadio.Checked) {
            if (-not $script:CustomIsoPath -or -not (Test-Path $script:CustomIsoPath)) {
                Write-LogMessage "Error: Please select a valid ISO file!" -IsError
                return
            }
            $script:IsoPath = $script:CustomIsoPath
            Write-LogMessage "Using custom ISO: $script:IsoPath"
            $isoInfo = Get-Item $script:IsoPath
            Write-LogMessage "ISO file size: $([math]::Round($isoInfo.Length / 1GB, 2)) GB"
        } else {
            $script:IsoPath = Join-Path $env:TEMP $distro.IsoFilename
            Write-LogMessage "Selected distribution: $distroName"
        }

        # Check space (only if we're shrinking the Windows disk partition)
        if ($selectedStrategy -eq "shrink_all") {
            $shrinkVol = Get-Volume -DriveLetter $shrinkDriveLetter -ErrorAction SilentlyContinue
            $shrinkFreeGB = if ($shrinkVol) { [math]::Round($shrinkVol.SizeRemaining / 1GB, 2) } else { 0 }
            if ($shrinkFreeGB -lt ($totalNeededGB + 10)) {
                Write-LogMessage "Error: Not enough free space on ${shrinkDriveLetter}: to shrink!" -IsError
                Write-LogMessage "Need: $($totalNeededGB + 10) GB free on ${shrinkDriveLetter}:" -IsError
                Write-LogMessage "Have: $shrinkFreeGB GB" -IsError
                return
            }
        } elseif ($selectedStrategy -eq "use_free_boot") {
            $neededFreeGB = if ($isLiveOnly) { $bootPartSizeGB } else { $linuxSizeGB }
            $shrinkVol = Get-Volume -DriveLetter $shrinkDriveLetter -ErrorAction SilentlyContinue
            $shrinkFreeGB = if ($shrinkVol) { [math]::Round($shrinkVol.SizeRemaining / 1GB, 2) } else { 0 }
            if ($shrinkFreeGB -lt ($neededFreeGB + 10)) {
                Write-LogMessage "Error: Not enough free space on ${shrinkDriveLetter}: to shrink!" -IsError
                Write-LogMessage "Need: $($neededFreeGB + 10) GB free on ${shrinkDriveLetter}:" -IsError
                Write-LogMessage "Have: $shrinkFreeGB GB" -IsError
                return
            }
        } elseif ($selectedStrategy -eq "other_drive") {
            $otherDiskFreeGB = Get-DiskUnallocatedGB -DiskNumber $targetDiskNumber
            $minNeededFreeGB = $bootPartSizeGB + $refindGB + 1
            if ($otherDiskFreeGB -lt $minNeededFreeGB) {
                Write-LogMessage "Error: Not enough unallocated space on Disk $targetDiskNumber!" -IsError
                Write-LogMessage "Need: $minNeededFreeGB GB, Have: $otherDiskFreeGB GB" -IsError
                return
            }
        } elseif ($selectedStrategy -eq "other_drive_shrink") {
            if (-not $otherDriveShrinkLetter) {
                Write-LogMessage "Error: No partition selected to shrink on Disk $targetDiskNumber!" -IsError
                return
            }
            try {
                $shrinkVol = Get-Volume -DriveLetter $otherDriveShrinkLetter -ErrorAction Stop
                $shrinkFreeGB = [math]::Round($shrinkVol.SizeRemaining / 1GB, 2)
                if ($shrinkFreeGB -lt ($otherDriveShrinkAmountGB + 5)) {
                    Write-LogMessage "Error: Not enough free space on ${otherDriveShrinkLetter}: to shrink!" -IsError
                    Write-LogMessage "Need: $($otherDriveShrinkAmountGB + 5) GB free, Have: $shrinkFreeGB GB" -IsError
                    return
                }
            } catch {
                Write-LogMessage "Error: Cannot access volume ${otherDriveShrinkLetter}: - $_" -IsError
                return
            }
        }

        # Download ISO if needed
        if (-not $customRadio.Checked) {
            if (Test-Path $script:IsoPath) {
                Write-LogMessage "Found existing ISO at: $script:IsoPath"

                try {
                    $fileInfo = Get-Item $script:IsoPath
                    $fileSizeGB = [math]::Round($fileInfo.Length / 1GB, 2)
                    Write-LogMessage "Existing ISO size: $fileSizeGB GB"

                    if ($fileInfo.Length -lt 2GB) {
                        Write-LogMessage "Existing ISO appears corrupted (too small)" -IsError
                        Write-LogMessage "Deleting corrupted file..." -IsError
                        Remove-Item $script:IsoPath -Force

                        Set-Status "Re-downloading $distroName ISO..."
                        if (-not (Save-LinuxISO -Destination $script:IsoPath)) {
                            Write-LogMessage "Failed to download $distroName ISO!" -IsError
                            return
                        }
                    } else {
                        if (-not (Test-ISOChecksum -FilePath $script:IsoPath)) {
                            Write-LogMessage "Existing ISO failed checksum verification" -IsError

                            Set-Status "Re-downloading $distroName ISO..."
                            if (-not (Save-LinuxISO -Destination $script:IsoPath)) {
                                Write-LogMessage "Failed to download $distroName ISO!" -IsError
                                return
                            }
                        } else {
                            if (-not $distro.IsHybrid) {
                                try {
                                    Get-DiskImage -ImagePath $script:IsoPath -ErrorAction Stop | Out-Null
                                    Write-LogMessage "ISO mount test passed"
                                }
                                catch {
                                    Write-LogMessage "Existing ISO appears corrupted (mount test failed)" -IsError
                                    Write-LogMessage "Error: $_" -IsError

                                    $response = [System.Windows.Forms.MessageBox]::Show(
                                        "The existing ISO file appears to be corrupted. Would you like to re-download it?",
                                        "Corrupted ISO",
                                        [System.Windows.Forms.MessageBoxButtons]::YesNo,
                                        [System.Windows.Forms.MessageBoxIcon]::Warning
                                    )

                                    if ($response -eq [System.Windows.Forms.DialogResult]::Yes) {
                                        Remove-Item $script:IsoPath -Force
                                        Set-Status "Re-downloading $distroName ISO..."
                                        if (-not (Save-LinuxISO -Destination $script:IsoPath)) {
                                            Write-LogMessage "Failed to download $distroName ISO!" -IsError
                                            return
                                        }
                                    } else {
                                        Write-LogMessage "Installation cancelled by user" -IsError
                                        return
                                    }
                                }
                            } else {
                                Write-LogMessage "ISO mount test skipped ($($distro.Keyword) hybrid ISO format)"
                            }
                        }
                    }
                }
                catch {
                    Write-LogMessage "Error checking existing ISO: $_" -IsError
                    return
                }
            } else {
                Set-Status "Downloading $distroName ISO..."
                if (-not (Save-LinuxISO -Destination $script:IsoPath)) {
                    Write-LogMessage "Failed to download $distroName ISO!" -IsError
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

            Write-LogMessage ""
            Write-LogMessage "== Strategy: wipe & reformat entire disk =="
            Write-LogMessage "Target disk: Disk $targetDiskNumber"

            if ($targetDiskNumber -eq $script:CDriveInfo.DiskNumber) {
                Write-LogMessage "REFUSING to wipe the disk containing Windows!" -IsError
                return
            }

            Set-Status "Wiping disk $targetDiskNumber..."
            Write-LogMessage "Clearing all data from Disk $targetDiskNumber..."

            try {
                Clear-Disk -Number $targetDiskNumber -RemoveData -RemoveOEM -Confirm:$false -ErrorAction Stop
                Write-LogMessage "Disk cleared successfully."
            }
            catch {
                Write-LogMessage "Clear-Disk failed: $_" -IsError
                Write-LogMessage "Trying diskpart fallback..."

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
                    Write-LogMessage "Diskpart wipe also failed!" -IsError
                    Write-LogMessage $resultString -IsError
                    return
                }
                Write-LogMessage "Disk wiped via diskpart."
            }

            Start-Sleep -Seconds 2

            try {
                $diskStatus = Get-Disk -Number $targetDiskNumber
                if ($diskStatus.PartitionStyle -ne "GPT") {
                    Initialize-Disk -Number $targetDiskNumber -PartitionStyle GPT -ErrorAction Stop
                    Write-LogMessage "Disk initialized as GPT."
                }
            } catch {
                Write-LogMessage "Note: GPT initialization: $_"
            }

            Start-Sleep -Seconds 2

            # Create boot partition (LINUX_LIVE, size from the disk plan)
            Write-LogMessage "Creating $($bootPartSizeGB) GB boot partition..."
            Set-Status "Creating boot partition..."
            try {
                $bootPartition = New-Partition -DiskNumber $targetDiskNumber `
                    -Size ([int64]($bootPartSizeGB * 1GB)) `
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

                Write-LogMessage "Boot partition created as ${driveLetter}: (LINUX_LIVE)"
                $script:NewDrive = "${driveLetter}:"
                $script:VolumeLabel = "LINUX_LIVE"
            }
            catch {
                Write-LogMessage "Failed to create boot partition: $_" -IsError
                return
            }

            # Create RefindPlus partition if enabled (wipe_disk)
            if ($useRefind) {
                Start-Sleep -Seconds 2
                $bootPartInfo = Get-Partition -DiskNumber $targetDiskNumber |
                    Where-Object { $_.DriveLetter -eq $driveLetter } | Select-Object -First 1
                $refindAfterOffset = $bootPartInfo.Offset + $bootPartInfo.Size
                $script:RefindPlusDriveLetter = New-RefindPartition -DiskNumber $targetDiskNumber -AfterOffset $refindAfterOffset
                if (-not $script:RefindPlusDriveLetter) {
                    Write-LogMessage "Warning: RefindPlus partition creation failed. Continuing without RefindPlus." -IsError
                    $useRefind = $false
                }
            }

            Start-Sleep -Seconds 2
            $diskAfter = Get-Disk -Number $targetDiskNumber
            $partsAfter = Get-Partition -DiskNumber $targetDiskNumber | Sort-Object Offset
            $usedBytes = [int64]0
            foreach ($p in $partsAfter) { $usedBytes += $p.Size }
            $unallocGB = [math]::Round(($diskAfter.Size - $usedBytes) / 1GB, 1)

            Write-LogMessage ""
            Write-LogMessage "Disk $targetDiskNumber wiped and reformatted successfully:"
            Write-LogMessage "  Partition 1: LINUX_LIVE ($($bootPartSizeGB) GB, ${driveLetter}:)"
            if ($useRefind -and $script:RefindPlusDriveLetter) {
                Write-LogMessage "  Partition 2: REFIND ($($script:RefindPlusSizeMB) MB, $($script:RefindPlusDriveLetter):)"
            }
            if ($isLiveOnly) {
                Write-LogMessage "  Unallocated: ~$unallocGB GB (unused - live-only mode)"
            } else {
                Write-LogMessage "  Unallocated: ~$unallocGB GB (for Linux installer)"
            }
            Write-LogMessage ""
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
            Write-LogMessage "Shrinking ${otherDriveShrinkLetter}: partition by $otherDriveShrinkAmountGB GB..."
            Write-LogMessage "This will create space for Linux: $linuxSizeGB GB and boot partition: $($bootPartSizeGB) GB..."

            if (-not (Shrink-UllPartition -DriveLetter $otherDriveShrinkLetter -ShrinkAmountGB $otherDriveShrinkAmountGB -TargetDiskNumber $targetDiskNumber)) {
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

            if ($isLiveOnly) {
                # The freed gap must clear the gap scan's boot size + 16 MiB
                # buffer + 1 MiB alignment, so shrink 1 GB more than needed.
                $shrinkAmountGB = $totalNeededGB + 1
            } else {
                $shrinkAmountGB = if ($selectedStrategy -eq "use_free_boot") { $linuxSizeGB } else { $totalNeededGB }
            }

            Set-Status "Shrinking ${shrinkDriveLetter}: partition..."
            Write-LogMessage "Shrinking ${shrinkDriveLetter}: partition by $shrinkAmountGB GB..."

            if ($selectedStrategy -eq "shrink_all") {
                $bootSizeGB = $bootPartSizeGB
                if ($isLiveOnly) {
                    Write-LogMessage "This will free space for the $bootSizeGB GB live boot partition..."
                } else {
                    Write-LogMessage "This will create space for Linux: $linuxSizeGB GB and boot partition: $bootSizeGB GB..."
                }
            } else {
                if ($isLiveOnly) {
                    Write-LogMessage "This will free space for the live boot partition..."
                    Write-LogMessage "The $($bootPartSizeGB) GB boot partition will use existing unallocated space."
                } else {
                    Write-LogMessage "This will create $linuxSizeGB GB of space for Linux installation..."
                    Write-LogMessage "The $($bootPartSizeGB) GB boot partition will use existing unallocated space."
                }
            }

            if (-not (Shrink-UllPartition -DriveLetter $shrinkDriveLetter -ShrinkAmountGB $shrinkAmountGB -TargetDiskNumber $targetDiskNumber)) {
                return
            }

            Start-Sleep -Seconds 5
        } else {
            if ($isOtherDrive) {
                Write-LogMessage "Skipping C: partition shrink - installing to a separate disk (Disk $targetDiskNumber)."
            } else {
                Write-LogMessage "Skipping C: partition shrink - using existing unallocated space."
            }
        }

        # Create boot partition (skip for wipe_disk -- already created above)
        if ($selectedStrategy -ne "wipe_disk") {
        Set-Status "Creating boot partition..."
        Write-LogMessage "Creating $($bootPartSizeGB) GB boot partition on Disk $targetDiskNumber..."

        try {
            Start-Sleep -Seconds 2
            $partitions = Get-Partition -DiskNumber $targetDiskNumber | Sort-Object Offset

            # Fetch the disk object fresh (after any shrink above) so the gap
            # scan can measure the trailing unallocated space against the real
            # disk size. Without this, $disk is $null in this function's scope
            # and the trailing gap is never added to the gap list, so a disk
            # whose free space is at the end fails with "No unallocated gap
            # large enough for the ... boot partition".
            $disk = Get-Disk -Number $targetDiskNumber -ErrorAction Stop

            if (-not $isOtherDrive) {
                $shrinkPartition = Get-Partition -DriveLetter $shrinkDriveLetter -ErrorAction SilentlyContinue
                if ($shrinkPartition) {
                    $anchorEnd = $shrinkPartition.Offset + $shrinkPartition.Size
                } else {
                    $cPartition = Get-Partition -DriveLetter C
                    $anchorEnd = $cPartition.Offset + $cPartition.Size
                }
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

            $bootPartitionSize = [int64]($bootPartSizeGB * 1GB)
            $alignmentSize = [int64](1MB)
            $refindReserve = if ($useRefind) { [int64]($script:RefindPlusSizeMB * 1MB) } else { [int64]0 }
            $bufferSize = [int64](16MB) + $refindReserve
            $minGapRequired = $bootPartitionSize + $bufferSize + $alignmentSize

            Write-LogMessage "Scanning disk for unallocated gaps..."
            foreach ($gap in $gaps) {
                $gapGB = [math]::Round($gap.Size / 1GB, 2)
                $gapStartGB = [math]::Round($gap.Start / 1GB, 2)
                Write-LogMessage "  Gap at $gapStartGB GB: $gapGB GB"
            }

            $usableGaps = $gaps | Where-Object { $_.Size -ge $minGapRequired }

            if (-not $usableGaps) {
                throw "No unallocated gap large enough for the $($bootPartSizeGB) GB boot partition"
            }

            $anchorGap = $usableGaps | Where-Object {
                $_.Start -ge ($anchorEnd - 1MB) -and $_.Start -le ($anchorEnd + 1MB)
            } | Select-Object -First 1

            $chosenGap = if ($anchorGap) { $anchorGap }
                         else { $usableGaps | Sort-Object Size -Descending | Select-Object -First 1 }

            $chosenGapGB = [math]::Round($chosenGap.Size / 1GB, 2)
            $chosenStartGB = [math]::Round($chosenGap.Start / 1GB, 2)
            Write-LogMessage "Selected gap for boot partition: $chosenGapGB GB starting at $chosenStartGB GB"

            $bootPartitionEndOffset = $chosenGap.End - $bufferSize
            $bootPartitionOffset = $bootPartitionEndOffset - $bootPartitionSize
            $bootPartitionOffset = [int64]([Math]::Floor($bootPartitionOffset / $alignmentSize)) * $alignmentSize

            if ($bootPartitionOffset -lt ($chosenGap.Start + $alignmentSize)) {
                throw "Selected gap ($chosenGapGB GB) is too small after alignment for the boot partition"
            }

            $linuxSpace = $bootPartitionOffset - $chosenGap.Start
            $linuxSpaceGB = [math]::Round($linuxSpace / 1GB, 2)

            Write-LogMessage "Unallocated space starts at: $chosenStartGB GB"
            Write-LogMessage "Boot partition will start at: $([math]::Round($bootPartitionOffset / 1GB, 2)) GB"
            Write-LogMessage "Gap ends at: $([math]::Round($chosenGap.End / 1GB, 2)) GB"
            Write-LogMessage "Linux will have $linuxSpaceGB GB of unallocated space"

            Write-LogMessage "Creating boot partition..."

            $bootPartitionSize = [int64]($bootPartSizeGB * 1GB)
            $offsetMB = [int64]([Math]::Floor($bootPartitionOffset / 1MB))
            $sizeMB = [int64]($bootPartSizeGB * 1024)

            if ($offsetMB -lt 0 -or $bootPartitionOffset -gt $disk.Size) {
                throw "Invalid offset calculated: $offsetMB MB (from $bootPartitionOffset bytes)"
            }

            Write-LogMessage "Attempting to create partition at offset: $([math]::Round($bootPartitionOffset / 1GB, 2)) GB - $offsetMB MB"

            $partitionCreated = $false
            $newPartition = $null

            try {
                Write-LogMessage "Attempting PowerShell method with specific offset..."
                $newPartition = New-Partition -DiskNumber $targetDiskNumber `
                    -Offset $bootPartitionOffset `
                    -Size $bootPartitionSize `
                    -AssignDriveLetter `
                    -ErrorAction Stop

                $partitionCreated = $true
                $driveLetter = $newPartition.DriveLetter
                Write-LogMessage "Success! Partition created using PowerShell method"
            }
            catch {
                Write-LogMessage "PowerShell method failed: $_"
                Write-LogMessage "Trying diskpart method..."

                $attempts = @(
                    @{Offset = $offsetMB; Description = "Calculated position"},
                    @{Offset = [int64]($offsetMB - 1024); Description = "1GB before calculated position"},
                    @{Offset = [int64]($offsetMB - 2048); Description = "2GB before calculated position"},
                    @{Offset = [int64]($offsetMB - 5120); Description = "5GB before calculated position"}
                )

                $attempts = $attempts | Where-Object { $_.Offset -gt 0 }

                foreach ($attempt in $attempts) {
                    Write-LogMessage "Attempt: $($attempt.Description)"
                    Write-LogMessage "Trying offset: $([math]::Round($attempt.Offset * 1MB / 1GB, 2)) GB"

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
                        Write-LogMessage "Success! Boot partition created at offset $([math]::Round($attempt.Offset * 1MB / 1GB, 2)) GB"
                        $partitionCreated = $true
                        break
                    } else {
                        if ($resultString -match "not enough usable space") {
                            Write-LogMessage "Not enough space at this offset, trying next position..."
                        } else {
                            Write-LogMessage "Failed with error: $($resultString | Select-String -Pattern 'error' -SimpleMatch)"
                        }
                    }
                }
            }

            if (-not $partitionCreated -and -not $isOtherDrive) {
                Write-LogMessage "Offset-based creation failed. Trying alternative approach..."

                $currentPartitions = Get-Partition -DiskNumber $targetDiskNumber | Sort-Object Offset
                $cPartition = $currentPartitions | Where-Object { $_.DriveLetter -eq 'C' }
                $cEndOffset = $cPartition.Offset + $cPartition.Size

                $recoveryPartition = $currentPartitions | Where-Object {
                    $_.Type -eq "Recovery" -or $_.GptType -match "de94bba4"
                } | Sort-Object Offset | Select-Object -First 1

                if ($recoveryPartition) {
                    $gapSize = $recoveryPartition.Offset - $cEndOffset
                    $gapSizeGB = [math]::Round($gapSize / 1GB, 2)
                    Write-LogMessage "Gap between C: and Recovery: $gapSizeGB GB"

                    $fillerSize = [int64]($gapSize - ($bootPartSizeGB * 1GB) - (1GB))
                    $fillerSizeGB = [math]::Round($fillerSize / 1GB, 2)

                    if ($fillerSize -gt 0) {
                        Write-LogMessage "Attempting workaround: Creating filler partition of $fillerSizeGB GB"

                        try {
                            $fillerPartition = New-Partition -DiskNumber $targetDiskNumber `
                                -Size $fillerSize `
                                -ErrorAction Stop

                            Write-LogMessage "Filler partition created. Now creating boot partition..."

                            $bootPartition = New-Partition -DiskNumber $targetDiskNumber `
                                -Size ($bootPartSizeGB * 1GB) `
                                -AssignDriveLetter `
                                -ErrorAction Stop

                            Write-LogMessage "Removing filler partition..."
                            Remove-Partition -DiskNumber $targetDiskNumber `
                                -PartitionNumber $fillerPartition.PartitionNumber `
                                -Confirm:$false `
                                -ErrorAction Stop

                            Write-LogMessage "Filler partition removed. Boot partition should now be at end."
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
                            Write-LogMessage "Workaround failed: $_" -IsError
                        }
                    }
                }
            }

            if (-not $partitionCreated) {
                Write-LogMessage "All offset methods failed. Creating partition without specific offset..."
                try {
                    $newPartition = New-Partition -DiskNumber $targetDiskNumber `
                        -Size ($bootPartSizeGB * 1GB) `
                        -AssignDriveLetter `
                        -ErrorAction Stop

                    $driveLetter = $newPartition.DriveLetter
                    $partitionCreated = $true
                    Write-LogMessage "Boot partition created using standard method"
                }
                catch {
                    throw "All partition creation methods failed: $_"
                }
            }

            if ($partitionCreated -and -not $driveLetter) {
                Start-Sleep -Seconds 3

                $targetSize = [int64]($bootPartSizeGB * 1GB)
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

            Write-LogMessage "Formatting boot partition as FAT32..."

            $volumeLabel = "LINUX_LIVE"

            Format-Volume -DriveLetter $driveLetter `
                -FileSystem FAT32 `
                -NewFileSystemLabel $volumeLabel `
                -Confirm:$false `
                -ErrorAction Stop

            Write-LogMessage "Boot partition created and assigned to ${driveLetter}:"
            $script:NewDrive = "${driveLetter}:"
            $script:VolumeLabel = $volumeLabel

            Write-LogMessage ""
            Write-LogMessage "=== Final Disk Layout (Disk $targetDiskNumber) ==="
            $finalPartitions = Get-Partition -DiskNumber $targetDiskNumber | Sort-Object Offset

            $previousEnd = [int64]0
            foreach ($part in $finalPartitions) {
                $sizeGB = [math]::Round($part.Size / 1GB, 2)
                $offsetGB = [math]::Round($part.Offset / 1GB, 2)
                $endGB = [math]::Round(($part.Offset + $part.Size) / 1GB, 2)

                if ($part.Offset -gt ($previousEnd + 1MB)) {
                    $gapSize = [math]::Round(($part.Offset - $previousEnd) / 1GB, 2)
                    if ($gapSize -gt 0.1) {
                        Write-LogMessage "[Unallocated: $gapSize GB]"
                    }
                }

                $label = if ($part.DriveLetter) { "Drive $($part.DriveLetter):" }
                        elseif ($part.Type -eq "Recovery" -or $part.GptType -match "de94bba4") { "(Recovery)" }
                        elseif ($part.IsSystem) { "(System)" }
                        else { "(No letter)" }

                Write-LogMessage "Partition $($part.PartitionNumber): $label - Size: $sizeGB GB - Location: $offsetGB-$endGB GB"

                $previousEnd = [int64]($part.Offset + $part.Size)
            }

            if ($disk.Size -gt ($previousEnd + 1MB)) {
                $trailingGap = [math]::Round(($disk.Size - $previousEnd) / 1GB, 2)
                if ($trailingGap -gt 0.1) {
                    Write-LogMessage "[Unallocated: $trailingGap GB]"
                }
            }

            Write-LogMessage ""
            Write-LogMessage "Boot partition successfully created!"

            # Create RefindPlus partition if enabled (non-wipe strategies)
            if ($useRefind) {
                $bootPartInfo = Get-Partition -DiskNumber $targetDiskNumber |
                    Where-Object { $_.DriveLetter -eq $driveLetter } | Select-Object -First 1
                if ($bootPartInfo) {
                    $refindAfterOffset = $bootPartInfo.Offset + $bootPartInfo.Size
                    $script:RefindPlusDriveLetter = New-RefindPartition -DiskNumber $targetDiskNumber -AfterOffset $refindAfterOffset
                    if (-not $script:RefindPlusDriveLetter) {
                        Write-LogMessage "Warning: RefindPlus partition creation failed. Continuing without RefindPlus." -IsError
                        $useRefind = $false
                    }
                } else {
                    Write-LogMessage "Warning: Could not find boot partition to place RefindPlus after." -IsError
                    $useRefind = $false
                }
            }

            Write-LogMessage "Linux can use the unallocated space for installation"

        }
        catch {
            Write-LogMessage "Failed to create boot partition: $_" -IsError
            return
        }
        } # end if ($selectedStrategy -ne "wipe_disk")

        # Mount ISO
        Set-Status "Mounting ISO..."
        Write-LogMessage "Mounting ISO..."

        try {
            if (-not (Test-Path $script:IsoPath)) {
                Write-LogMessage "ISO file not found at: $script:IsoPath" -IsError
                return
            }

            $mountResult = Mount-DiskImage -ImagePath $script:IsoPath -StorageType ISO -PassThru -ErrorAction Stop
            Start-Sleep -Seconds 2

            $isoVolume = Get-Volume -DiskImage $mountResult -ErrorAction Stop | Select-Object -First 1

            if (-not $isoVolume) {
                Write-LogMessage "Failed to get volume information from mounted ISO" -IsError
                Dismount-DiskImage -ImagePath $script:IsoPath -ErrorAction SilentlyContinue
                return
            }

            $sourceDrive = "$($isoVolume.DriveLetter):"
            Write-LogMessage "ISO mounted at $sourceDrive"

            if (-not $customRadio.Checked) {
                $validationFile = "$sourceDrive\$($distro.ValidationFile)"

                if (-not (Test-Path $validationFile)) {
                    Write-LogMessage "Warning: ISO may not be a valid $distroName image (missing expected files)" -IsError

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
                Write-LogMessage "Custom ISO mounted. Skipping validation."
            }
        }
        catch {
            Write-LogMessage "Failed to mount ISO: $_" -IsError

            $response = [System.Windows.Forms.MessageBox]::Show(
                "Failed to mount the ISO file. It may be corrupted. Would you like to delete it and re-download?",
                "Mount Failed",
                [System.Windows.Forms.MessageBoxButtons]::YesNo,
                [System.Windows.Forms.MessageBoxIcon]::Error
            )

            if ($response -eq [System.Windows.Forms.DialogResult]::Yes -and -not $customRadio.Checked) {
                try {
                    Remove-Item $script:IsoPath -Force
                    Write-LogMessage "Deleted corrupted ISO"

                    Set-Status "Re-downloading $distroName ISO..."
                    if (Save-LinuxISO -Destination $script:IsoPath) {
                        Start-Installation
                        return
                    }
                } catch {
                    Write-LogMessage "Error handling corrupted ISO: $_" -IsError
                }
            }
            return
        }

        # Copy files
        Set-Status "Copying files..."
        Write-LogMessage "Copying $distroName files to $script:NewDrive..."
        Write-LogMessage "This may take 10-20 minutes..."

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
                Write-LogMessage "Failed to copy files! Exit code: $LASTEXITCODE" -IsError
                return
            }

            Write-LogMessage "Files copied successfully!"

            Write-LogMessage "Removing read-only attributes..."
            Set-Status "Removing read-only attributes..."
            try {
                Get-ChildItem -Path $script:NewDrive -Recurse -Force -ErrorAction SilentlyContinue |
                    Where-Object { $_.Attributes -band [System.IO.FileAttributes]::ReadOnly } |
                    ForEach-Object {
                        $_.Attributes = $_.Attributes -band (-bnot [System.IO.FileAttributes]::ReadOnly)
                    }
            } catch {
                Write-LogMessage "Warning: Could not remove all read-only attributes: $_" -IsError
            }
        }
        catch {
            Write-LogMessage "Error during file copy: $_" -IsError
            return
        }
        finally {
            Dismount-DiskImage -ImagePath $script:IsoPath
        }

        # Fedora-specific: fix volume label in GRUB and isolinux configs
        if ($distro.Keyword -eq "Fedora") {
            Set-Status "Fixing Fedora boot labels..."
            Write-LogMessage "Fixing Fedora volume label references in boot configs..."

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
                Write-LogMessage "Warning: No boot config files found to patch" -IsError
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
                            Write-LogMessage "  Patched: $(Split-Path -Leaf $cfgFile)"
                            $patchedCount++
                        } else {
                            Write-LogMessage "  No label references in: $(Split-Path -Leaf $cfgFile)"
                        }
                    }
                    catch {
                        Write-LogMessage "  Warning: Could not patch $($cfgFile): $_" -IsError
                    }
                }

                if ($patchedCount -gt 0) {
                    Write-LogMessage "Patched $patchedCount boot config file(s) with label '$fedoraLabel'"
                } else {
                    Write-LogMessage "Warning: No LABEL references found to patch. Fedora may not boot correctly." -IsError
                    Write-LogMessage "You may need to manually edit EFI\BOOT\grub.cfg and replace the LABEL= value with '$fedoraLabel'" -IsError
                }
            }
        }

        # CachyOS/Arch-specific: fix archisolabel in GRUB, syslinux, and loader configs
        if ($distro.Keyword -eq "CachyOS") {
            Set-Status "Fixing CachyOS boot labels..."
            Write-LogMessage "Fixing CachyOS volume label references in boot configs..."

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
                Write-LogMessage "Warning: No boot config files found to patch" -IsError
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
                            Write-LogMessage "  Patched: $(Split-Path -Leaf $cfgFile)"
                            $patchedCount++
                        } else {
                            Write-LogMessage "  No label references in: $(Split-Path -Leaf $cfgFile)"
                        }
                    }
                    catch {
                        Write-LogMessage "  Warning: Could not patch $($cfgFile): $_" -IsError
                    }
                }

                if ($patchedCount -gt 0) {
                    Write-LogMessage "Patched $patchedCount boot config file(s) with label '$cachyLabel'"
                } else {
                    Write-LogMessage "Warning: No archisolabel references found to patch. CachyOS may not boot correctly." -IsError
                    Write-LogMessage "You may need to manually edit the boot config files and replace archisolabel= with '$cachyLabel'" -IsError
                }
            }
        }

        # RAM boot: append the distro's RAM-boot parameter to every boot
        # config on the LINUX_LIVE partition. This covers the direct boot
        # path (GRUB), syslinux/isolinux and systemd-boot entries. It is
        # controlled by the per-distro ToRam switch, so it is a no-op for
        # distros with ToRam = $false.
        $ramParam = Get-RamBootParameter -Distro $distro
        if ($ramParam) {
            $ramPatched = Add-RamBootToBootConfig -RootPath $script:NewDrive -Distro $distro
            if ($ramPatched -gt 0) {
                Write-LogMessage "RAM boot enabled: added '$ramParam' to $ramPatched boot config file(s)"
            } else {
                Write-LogMessage "Warning: RAM boot is enabled for $($distro.Name) but no boot config files were patched." -IsError
            }
        }

        # Install RefindPlus if enabled
        if ($useRefind -and $script:RefindPlusDriveLetter) {
            $bootDriveLetter = $script:NewDrive.TrimEnd(':')
            $refindInstalled = Install-RefindPlus `
                -RefindPlusDriveLetter $script:RefindPlusDriveLetter `
                -BootDriveLetter $bootDriveLetter `
                -DistroLabel $distroName `
                -RamBootParam (Get-RamBootParameter -Distro $distro)
            if (-not $refindInstalled) {
                Write-LogMessage "Warning: RefindPlus installation failed. Falling back to direct boot." -IsError
                $useRefind = $false
            }
        }

        # Create boot configuration
        Set-Status "Creating boot configuration..."
        Write-LogMessage "Creating boot configuration..."

        $efiPath = $script:NewDrive + "\EFI\BOOT"
        if (-not (Test-Path $efiPath)) {
            New-Item -Path $efiPath -ItemType Directory -Force
        }

        # For wipe_disk on a secondary drive: install the bootloader into the
        # Windows ESP so the firmware can boot it. (Skip if RefindPlus handles booting.)
        $script:WipeBootInstalled = $false
        if ($selectedStrategy -eq "wipe_disk" -and -not ($useRefind -and $script:RefindPlusDriveLetter)) {
            try {
                Write-LogMessage "Installing bootloader into Windows ESP..."
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
                Write-LogMessage "Windows ESP mounted at $winEspDrive"

                $safeName = ($distroName -replace '[^a-zA-Z0-9]', '').Trim()
                if (-not $safeName) { $safeName = "Linux" }
                $script:WipeEspDistroDir = "\EFI\$safeName"
                $distroEspDir = "$winEspDrive$($script:WipeEspDistroDir)"
                New-Item -Path $distroEspDir -ItemType Directory -Force | Out-Null

                $sourceEfi = $script:NewDrive + "\EFI\BOOT"
                if (Test-Path $sourceEfi) {
                    robocopy $sourceEfi $distroEspDir /E /R:2 /W:2 /NP /NFL /NDL | Out-Null
                    Write-LogMessage "EFI\BOOT directory copied to $distroEspDir"
                } else {
                    throw "No EFI\BOOT directory found on $($script:NewDrive)"
                }

                foreach ($grubDir in @("boot\grub", "boot\grub2")) {
                    $srcGrub = Join-Path $script:NewDrive $grubDir
                    if (Test-Path $srcGrub) {
                        $dstGrub = Join-Path $distroEspDir $grubDir
                        New-Item -Path $dstGrub -ItemType Directory -Force | Out-Null
                        robocopy $srcGrub $dstGrub /E /R:2 /W:2 /NP /NFL /NDL | Out-Null
                        Write-LogMessage "Copied $grubDir to ESP"
                    }
                }

                $liveLabel = $script:VolumeLabel
                Write-LogMessage "Patching boot configs in ESP to use label '$liveLabel'..."

                $cfgFiles = Get-ChildItem -Path $distroEspDir -Recurse -Include "*.cfg","*.conf" -ErrorAction SilentlyContinue
                $patchedCount = 0
                $espRamParam = Get-RamBootParameter -Distro $distro
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

                        # RAM boot: append the parameter to kernel command lines
                        $content = Add-RamBootParameterToContent -Content $content -RamParam $espRamParam

                        if ($content -ne $original) {
                            Set-Content -Path $cfgFile.FullName -Value $content -Encoding UTF8 -Force
                            $patchedCount++
                        }
                    } catch {
                        Write-LogMessage "  Warning: Could not patch $($cfgFile.Name): $_" -IsError
                    }
                }
                Write-LogMessage "Patched $patchedCount config file(s) in ESP"

                $script:WipeEfiName = "BOOTx64.EFI"
                foreach ($candidate in @("shimx64.efi", "grubx64.efi")) {
                    if (Test-Path "$distroEspDir\$candidate") {
                        $script:WipeEfiName = $candidate
                        break
                    }
                }
                Write-LogMessage "Boot binary: $($script:WipeEfiName)"

                $script:WipeWinEspDrive = $winEspDrive
                $script:WipeBootInstalled = $true

                if ($removeLetter -and $winEspLetter) {
                    $script:WipeEspRemoveLetter = $true
                    $script:WipeEspLetter = $winEspLetter
                    $script:WipeEspPartition = $winEspPart
                } else {
                    $script:WipeEspRemoveLetter = $false
                }

                Write-LogMessage "Bootloader installed to Windows ESP at $($script:WipeEspDistroDir)"
            } catch {
                Write-LogMessage "Failed to install bootloader to Windows ESP: $_" -IsError
                Write-LogMessage "You may need to configure boot manually in UEFI/BIOS settings" -IsError
            }
        }

        # Configure UEFI boot entry. This runs independently of the
        # auto-restart checkbox: boot configuration must always be attempted so
        # readiness reflects the actual outcome, not the restart setting.
        Write-LogMessage "Configuring UEFI boot priority..."
        Set-Status "Configuring UEFI boot priority..."

        try {
            $script:uefiBootReady = $false

            $bootDeviceDrive = $script:NewDrive

            $bootOutcome = Configure-UelliBootEntry `
                -DistroName $distroName `
                -BootDriveLetter $bootDeviceDrive.TrimEnd(':') `
                -EfiPath "\EFI\BOOT\BOOTx64.EFI" `
                -UseRefind:[$useRefind] `
                -RefindDriveLetter $script:RefindPlusDriveLetter

            $script:uefiBootReady = [bool]$bootOutcome.Ready
            $useRefind = [bool]$bootOutcome.UseRefind
        }
        catch {
            $script:uefiBootReady = $false
            Write-LogMessage "Error configuring UEFI boot: $_" -IsError
            Write-LogMessage "You may need to set boot priority manually in UEFI/BIOS settings" -IsError
        }

        # Success
        Write-LogMessage "====================================="
        if ($script:uefiBootReady) {
            Write-LogMessage "Installation Complete!"
        } else {
            Write-LogMessage "Installation finished, but the UEFI boot entry was not created automatically." -IsError
            Write-LogMessage "Files were copied to $script:NewDrive, but the installation is NOT yet bootable." -IsError
            Write-LogMessage "You will need to set boot priority manually in UEFI/BIOS settings" -IsError
            Write-LogMessage "Or use the one-time boot menu (usually F12) to select the $distroName partition" -IsError
        }
        Write-LogMessage "====================================="
        Write-LogMessage "$distroName boot partition created at drive $script:NewDrive"
        if ($customRadio.Checked) {
            Write-LogMessage "ISO used: $(Split-Path -Leaf $script:CustomIsoPath)"
        }
        Write-LogMessage ""
        Write-LogMessage "*** DISK LAYOUT ***"
        $finalPartitions = Get-Partition -DiskNumber $targetDiskNumber | Sort-Object Offset
        foreach ($part in $finalPartitions) {
            $sizeGB = [math]::Round($part.Size / 1GB, 2)
            $label = if ($part.DriveLetter) { "Drive $($part.DriveLetter)" }
                    elseif ($part.Type -eq "Recovery" -or $part.GptType -match "de94bba4") { "Recovery" }
                    elseif ($part.IsSystem) { "System" }
                    else { "No letter" }
            Write-LogMessage "- ${label}: $sizeGB GB"
        }
        Write-LogMessage ""
        if ($isLiveOnly) {
            Write-LogMessage "Live-only setup: nothing was installed to your disk."
            Write-LogMessage "Boot the ""$distroName"" UEFI entry (or use your firmware boot menu) to run the live environment."
        } else {
            Write-LogMessage "The unallocated space is ready for $distroName installation."
            Write-LogMessage "The installer will automatically detect and use this space."
        }
        Write-LogMessage ""

        if ($useRefind -and $script:RefindPlusDriveLetter) {
            Write-LogMessage ""
            Write-LogMessage "RefindPlus boot manager has been installed and configured as the firmware default."
            Write-LogMessage ""
        }

        if ($script:uefiBootReady) {
            Write-LogMessage "*** BOOT CONFIGURATION COMPLETE ***"
            Write-LogMessage "The UEFI boot default has been configured to the new boot entry."
            Write-LogMessage "The firmware should boot this entry on the next restart; this is not guaranteed."
            Write-LogMessage "If it does not, use the firmware boot menu to select the entry manually."
            if ($autoRestartCheck.Checked) {
                Write-LogMessage "The system will restart automatically."
            } else {
                Write-LogMessage "Restart your computer to boot into the installed system."
            }
            if ($useRefind -and $script:RefindPlusDriveLetter) {
                Write-LogMessage "After restart, RefindPlus should appear as the firmware default and show `"$distroName`"."
            } else {
                Write-LogMessage "After restart, the system should boot into $distroName; if it does not, use the firmware boot menu."
            }
            Write-LogMessage ""
        } else {
            Write-LogMessage "*** BOOT CONFIGURATION NOT COMPLETE ***" -IsError
            Write-LogMessage "Files were copied to $script:NewDrive, but the UEFI boot entry was not created automatically." -IsError
            Write-LogMessage "The installation is NOT yet bootable." -IsError
            Write-LogMessage "You will need to set boot priority manually in UEFI/BIOS settings" -IsError
            Write-LogMessage "Or use the one-time boot menu (usually F12) to select the $distroName partition" -IsError
            Write-LogMessage ""
            if ($useRefind -and $script:RefindPlusDriveLetter) {
                Write-LogMessage "To boot ${distroName}:"
                Write-LogMessage "1. Restart your computer"
                Write-LogMessage "2. Enter UEFI/BIOS (F2/F10/F12/DEL)"
                Write-LogMessage "3. Select 'RefindPlus - ULLI' from the boot menu"
                Write-LogMessage "4. If RefindPlus does not appear, select the $distroName partition directly"
                Write-LogMessage "5. Disable Secure Boot if needed"
            } else {
                Write-LogMessage "To boot $distroName, use the UEFI boot menu:"
                Write-LogMessage "1. Restart your computer"
                Write-LogMessage "2. Press F2, F10, F12, DEL, or ESC during startup"
                Write-LogMessage "3. Select the $distroName entry"
                Write-LogMessage "4. Make sure Secure Boot is disabled"
            }
            Write-LogMessage ""
        }

        Set-Status (Get-UelliBootStatusText -BootReady $script:uefiBootReady)

        # Delete ISO if requested
        if ($deleteIsoCheck.Checked -and -not $customRadio.Checked) {
            try {
                Remove-Item $script:IsoPath -Force
                Write-LogMessage "ISO file deleted."
            }
            catch {
                Write-LogMessage "Could not delete ISO file."
            }
        }

        # Auto-restart only when enabled AND the current installation's required
        # boot configuration succeeded. Never restart into an unverified boot path.
        if (Test-UelliBootRestartEligible -AutoRestart $autoRestartCheck.Checked -BootReady $script:uefiBootReady) {
            Write-LogMessage ""
            Write-LogMessage "Preparing for automatic restart..."

            $countdownForm = New-Object System.Windows.Forms.Form
            $countdownForm.Text = "System Restart"
            $countdownForm.Size = New-Object System.Drawing.Size(420, 210)
            $countdownForm.StartPosition = "CenterScreen"
            $countdownForm.FormBorderStyle = "FixedDialog"
            $countdownForm.MaximizeBox = $false
            $countdownForm.MinimizeBox = $false

            $countdownLabel = New-Object System.Windows.Forms.Label
            $restartMsg = if ($useRefind -and $script:RefindPlusDriveLetter) {
                "The firmware boot default has been configured to RefindPlus.`nRefindPlus should appear on restart and show `"$distroName`"; use the firmware boot menu if it does not."
            } else {
                "The firmware boot default has been configured.`nThe system should boot into $distroName on restart; if it does not, use the firmware boot menu."
            }
            $countdownLabel.Text = "System will restart in 30 seconds...`n`n$restartMsg"
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
            $script:CountdownSeconds = 30

            $timer.Add_Tick({
                $script:CountdownSeconds--
                $countdownLabel.Text = "System will restart in $script:CountdownSeconds seconds...`n`n$restartMsg"

                if ($script:CountdownSeconds -le 0) {
                    $timer.Stop()
                    $countdownForm.Close()
                }
            })

            $timer.Start()
            $countdownForm.ShowDialog()
            $timer.Stop()

            if (-not $script:CancelRestart) {
                Write-LogMessage "Restarting system..."
                Start-Sleep -Seconds 2
                Restart-Computer -Force
            } else {
                Write-LogMessage "Restart cancelled by user"
                Write-LogMessage "You can restart manually when ready"
            }
        } else {
            if ($autoRestartCheck.Checked -and -not $script:uefiBootReady) {
                Write-LogMessage "Automatic restart skipped: UEFI boot configuration was not successful." -IsError
                Write-LogMessage "Restart manually and configure boot priority in UEFI/BIOS settings when ready." -IsError
            } elseif ($useRefind -and $script:RefindPlusDriveLetter) {
                Write-LogMessage "Restart manually and select 'RefindPlus - ULLI' from the firmware boot menu."
            }
        }
    }
    catch {
        # Any exception escaping after boot configuration set readiness to true
        # must not leave a successful-readiness state behind. Reset it here so
        # status text and restart eligibility reflect the failure, not a stale
        # success from this or a previous run.
        $script:uefiBootReady = $false
        Write-LogMessage "Installation error: $_" -IsError
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
        Write-LogMessage "Selected ISO: $(Split-Path -Leaf $script:CustomIsoPath)"
        Write-LogMessage "File size: $fileSizeGB GB"
    }
})

# Initialize
Update-DiskInfo

# Show form
$form.ShowDialog() | Out-Null
