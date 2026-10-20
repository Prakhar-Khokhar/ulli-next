# ULLI-Next

**ULLI stands for USB-less Linux Installer.**

**Install and boot Linux from an internal drive—without first creating a bootable USB stick.**

ULLI-Next is a community-maintained fork of [ULLI](https://github.com/rltvty2/ulli), originally created by [rltvty2](https://github.com/rltvty2/). It is maintained by Prakhar Khokhar.

The project retains the legacy Python/Linux and PowerShell/Windows installer while developing a newer **Qt 6 / C++20** application with explicit installation plans, a structured engine, platform backends, and a graphical partition-layout preview.

> [!WARNING]
> **Beta software; the newer Qt/C++ implementation is experimental.** ULLI-Next changes disk partitions and UEFI boot configuration. Errors can cause data loss or leave a computer unbootable. Back up important data before use, and do not use it on a system you cannot afford to recover.

## How it works

ULLI-Next prepares a Linux live environment on an internal drive, creates the required partition layout, configures distribution-specific boot files, and adds a UEFI boot option. Depending on the selected plan, it can use existing free space, shrink a volume, target another drive, or wipe a specifically selected disk.

The newer installer separates the proposed changes into an `InstallPlan` and validates the plan before destructive operations. It includes ISO SHA-256 verification, staged installation, cancellation checks, and boot-entry cleanup/rollback attempts. These safeguards reduce risk; they do **not** replace backups or guarantee recovery.

## Distribution support

The built-in catalog metadata is maintained in [`distros.json`](./distros.json). That file is the source of truth for ISO filenames, download pages, mirrors, and checksums—these details are intentionally not duplicated here.

| Distribution | Edition | Status / guidance |
|---|---|---|
| Linux Mint 22.3 | Cinnamon | Catalog entry |
| Ubuntu 26.04 LTS | GNOME | Catalog entry |
| Kubuntu 26.04 LTS | KDE Plasma 6 | Catalog entry |
| Linux Lite 8.0 | Xfce | Catalog entry |
| Debian Live 13.7 | KDE | Catalog entry |
| CachyOS | Desktop | Catalog entry |
| Fedora 44 | KDE Plasma Desktop | **Experimental — 6 GB+ RAM recommended; not properly tested. Avoid for now.** |
| Zorin OS | Desktop | **Experimental — 6 GB+ RAM recommended; not properly tested. Avoid for now.** |

**Important:** Linux Lite and Zorin OS support should be treated cautiously until testing is complete. Fedora and Zorin OS are listed for visibility, not as recommendations for normal use. Prefer a better-tested option for now. Check the actual catalog in `distros.json` before relying on a distro being available in your build.

### Custom ISOs

A **Custom ISO** option allows use of distributions outside the built-in catalog. Live ISOs are preferred. Current limits are **7 GB per ISO** and **4 GB per individual file inside the ISO**, so some images (including Bazzite images with files beyond these limits) will not work yet.

## Key capabilities

- USB-less Linux live boot and installation workflow.
- Distribution catalog with download sources and SHA-256 verification.
- Installation planning and pre-flight validation.
- Storage strategies for existing free space, volume shrinking, secondary drives, and explicit disk wiping.
- **Live Only** mode with an approximately 7 GiB FAT32 staging partition.
- **Full Install** planning with at least approximately 30 GiB of additional Linux space.
- Direct UEFI boot configuration in the legacy Windows PowerShell installer, including an optional rEFInd boot-manager path.
- Windows-specific components for disk enumeration, EFI/BCD handling, BitLocker checks, and storage operations.
- Qt GUI components for distro selection, installation planning, partition preview, progress, and logs.

Exact support depends on the implementation and platform; not every path has been validated end-to-end on real hardware.

## Requirements and cautions

- Use a compatible UEFI system and review every disk/partition choice carefully.
- Back up important files before installation.
- BitLocker may need to be suspended or decrypted, depending on the scenario.
- Secure Boot requirements depend on the bootloader and installation path. The legacy Windows PowerShell installer's optional rEFInd mode requires Secure Boot to be disabled.
- Firmware may not automatically select the new Linux boot option.
- Successful operation on one computer does not guarantee the same result on another.

## Legacy Windows PowerShell bootloader: rEFInd

The legacy `windows/ulli-windows.ps1` installer includes an **optional upstream rEFInd 0.14.2** path. When selected, the script plans for an additional 100 MB FAT32 partition labeled `REFIND`, placed after the `LINUX_LIVE` boot partition. It downloads the rEFInd archive from SourceForge, installs the EFI loader and supporting filesystem drivers/icons, detects the Linux image's boot-file layout, and generates a `refind.conf` menu entry using the distro's kernel arguments where available. It then creates a UEFI boot entry named `rEFInd - ULLI`; when automatic restart is enabled, the script attempts to make that entry the default. The existing Windows EFI System Partition is not used as the rEFInd installation destination. If the optional rEFInd setup fails, the script is intended to continue with direct boot where possible.

**This is the existing rEFInd implementation in the legacy PowerShell script—not RefindPlus.** RefindPlus has **not** been implemented in the newer Qt 6/C++ Windows installer, and work on that integration has not started. Do not assume RefindPlus will behave like this existing rEFInd path or share its Secure Boot limitations; any future integration needs its own implementation and hardware testing.

## Running the legacy installer

The legacy implementation remains in the repository.

**Linux**
```bash
sudo python3 ulli-linux.py
```

**Windows**

Extract the Windows package and run `run-ulli-windows.bat` as Administrator.

For the newer Qt/C++ application, see the repository's build files and source tree. The Windows Qt 6/C++ version has reached a **buildable development stage** and produces a CI test package; its UI, distro catalog, and ISO-download path are now being smoke-tested. However, **partitioning and other critical installation operations are not yet complete or validated enough for real installations**. Do not use this build to modify disks containing important data, and do not treat it as a replacement for the legacy installer.

## Development status

The Qt 6/C++ port is the main long-term direction and has reached a **buildable stage on Windows**. Basic UI, catalog, and ISO-download testing is underway, but this is not yet a functional, validated end-to-end installer: partition-layout execution, disk operations, boot configuration, cancellation/rollback, and other critical paths still require implementation work and careful testing. Do not rely on this version for real installations yet. Work continues on Windows/Linux backend completeness, partitioning safety, recovery behavior, automated tests, packaging, and broader filesystem support. Support for filesystems such as ext4 and Btrfs and more capable cross-platform partitioning tools remains future work.

Development follows an iterative process: **problem → hypothesis → implementation → test → diagnosis → modification → retest**. AI coding tools have been used, but generated code is not assumed correct; changes need review and testing. For a non-AI version of the original project, see [ULLI-organic](https://github.com/rltvty2/ulli-organic).

## Contributing and reporting issues

Testing on different hardware, checking unusual disk layouts and UEFI firmware, reporting reproducible bugs, reviewing partitioning logic, and improving documentation are all useful contributions.

When reporting a problem, include your OS/version, hardware and disk layout, UEFI/Secure Boot and BitLocker state, selected distro/ISO, installation strategy, expected versus actual behavior, logs, and reproduction steps. GitHub Issues and Pull Requests are welcome.

## Credits and license

ULLI-Next is based on [ULLI by rltvty2](https://github.com/rltvty2/ulli) and is released under the **GNU General Public License v3.0 (GPL-3.0)**.

## Support the project ❤️

Small donations help, but there are plenty of ways to contribute without donating: test early versions, report bugs, suggest improvements, contribute code, improve documentation, or share the project.

**UPI ID:** `8979584868@fam`  
[Pay via UPI](upi://pay?pa=8979584868%40fam&pn=Prakhar%20Khokhar)

**Contact:** 8066prakhar@gmail.com · personnormal076@gmail.com
