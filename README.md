# ULLI-Next

**ULLI stands for USB-Less Linux Installer.**

**Install and boot Linux from an internal drive—without first creating a bootable USB stick.**

ULLI-Next is a community-maintained continuation of [ULLI](https://github.com/rltvty2/ulli), originally created by [rltvty2](https://github.com/rltvty2/). It is maintained by Prakhar Khokhar.

The project preserves the original Linux/Python and Windows/PowerShell installers while developing a newer **Qt 6 / C++20** application. The newer implementation is being built around explicit installation plans, a structured engine, platform-specific backends, and a graphical partition-layout preview.

> [!CAUTION]
> **ULLI-Next can modify disk partitions and UEFI boot configuration.** Mistakes or unexpected system behaviour can cause data loss or leave a computer unable to boot. Back up important data before using any installer, carefully verify every selected disk, and never experiment on a system or drive you cannot afford to recover.

> [!IMPORTANT]
> **The Qt/C++ rewrite is experimental and is not ready for real installations.** Its current buildable development state must not be mistaken for a complete, validated installer. See [Development status](#development-status) before choosing an implementation.

## At a glance

| | Status |
|---|---|
| Original ULLI lineage | Community-maintained continuation |
| Legacy Linux/Python installer | Retained in the repository |
| Legacy Windows/PowerShell installer | Retained in the repository |
| New Qt 6 / C++20 application | Buildable development-stage rewrite |
| Qt rewrite partitioning and end-to-end installation | **Not ready / not validated** |
| License | GNU GPL-3.0 |

## Contents

- [How ULLI works](#how-ulli-works)
- [Implementations and current status](#implementations-and-current-status)
- [Distribution catalogue and custom ISOs](#distribution-catalogue-and-custom-isos)
- [Capabilities and design](#capabilities-and-design)
- [Requirements and safety](#requirements-and-safety)
- [Running the legacy installer](#running-the-legacy-installer)
- [Development status](#development-status)
- [Contributing and reporting issues](#contributing-and-reporting-issues)
- [Credits and license](#credits-and-license)

## How ULLI works

ULLI prepares a Linux live environment on an internal drive, allowing a compatible UEFI computer to boot that environment without first creating a USB installer. The workflow prepares the required storage layout, places the distribution's boot files, and configures a UEFI boot option.

Depending on the implementation and selected installation plan, the workflow may use existing free space, shrink a volume, target another drive, or wipe a specifically selected disk. **These operations are potentially destructive.** The exact strategies available depend on the installer implementation and platform; do not assume every strategy is supported by every version.

## Implementations and current status

ULLI-Next currently contains two generations of the installer.

### Legacy installers

The original-style implementations remain in the repository:

- **Linux:** Python-based installer.
- **Windows:** PowerShell-based installer.

These are distinct from the newer Qt/C++ rewrite. Consult the relevant scripts and their current behaviour before using them; do not assume that every distribution or storage configuration has been tested on your hardware.

### Qt 6 / C++20 rewrite

The newer application is the long-term development direction. It introduces a graphical workflow, installation-plan model, platform backends, distribution catalogue, ISO handling, and a partition-layout preview.

The Windows application has reached a **buildable development stage**, and its basic UI, catalogue, and ISO-download path are being smoke-tested. However, critical installation functionality—including partition execution, disk operations, boot configuration, and end-to-end recovery behaviour—is not complete and validated enough for real installations.

**Do not use the Qt/C++ development build to modify disks containing important data or as a replacement for a proven installer.** Buildability and passing automated tests do not establish that destructive installation paths are safe on real hardware.

## Distribution catalogue and custom ISOs

The built-in distribution metadata is maintained in [distros.json](./distros.json). It is the source of truth for ISO filenames, download pages, mirrors, and checksums; these details are intentionally not duplicated here.

The catalogue currently includes the following entries. A catalogue entry means the distribution is listed in the project metadata—it does **not** mean every installer implementation or release has been validated with it.

| Distribution | Edition | Status / guidance |
|---|---|---|
| Linux Mint 22.3 | Cinnamon | Catalogue entry |
| Ubuntu 26.04 LTS | GNOME | Catalogue entry |
| Kubuntu 26.04 LTS | KDE Plasma 6 | Catalogue entry |
| Linux Lite 8.0 | Xfce | Catalogue entry |
| Debian Live 13.7 | KDE | Catalogue entry |
| CachyOS | Desktop | Catalogue entry |
| Fedora 44 | KDE Plasma Desktop | **Experimental — 6 GB+ RAM recommended; not properly tested. Avoid for now.** |
| Zorin OS | Desktop | **Experimental — 6 GB+ RAM recommended; not properly tested. Avoid for now.** |

Linux Lite and Zorin OS should also be treated cautiously until testing is complete. Fedora and Zorin OS are listed for visibility, not as recommendations for normal use. Check the actual catalogue in distros.json before relying on a distribution being available in your build.

### Custom ISOs

A **Custom ISO** option allows distributions outside the built-in catalogue to be selected. Live ISOs are preferred.

Current limits are **7 GB per ISO** and **4 GB per individual file inside the ISO**. Some images with larger individual files—including some Bazzite images—will not work with these limits.

## Capabilities and design

The project is being developed around these capabilities:

- USB-less Linux live boot and installation workflow.
- Distribution catalogue with download sources and SHA-256 verification.
- Explicit installation planning and pre-flight validation.
- Storage strategies involving existing free space, volume shrinking, secondary drives, and explicitly selected disk wiping, where supported by the implementation.
- **Live Only** planning with an approximately 7 GiB FAT32 staging partition.
- **Full Install** planning with at least approximately 30 GiB of additional Linux space.
- Windows-specific components for disk enumeration, EFI/BCD handling, BitLocker checks, and storage operations.
- Qt GUI components for distribution selection, installation planning, partition preview, progress, and logs.

The newer implementation separates proposed changes into an InstallPlan and validates the plan before destructive operations. It also includes ISO SHA-256 verification, staged installation, cancellation checks, and boot-entry cleanup/rollback attempts. These safeguards are intended to reduce risk; they do **not** eliminate it or guarantee recovery.

Exact support depends on the implementation and platform. Not every path has been validated end-to-end on real hardware.

## Requirements and safety

Before using an installer:

- Use a compatible UEFI system and carefully review every disk and partition choice.
- Back up important files and verify that the backup is accessible.
- Understand the selected installation strategy and which drive it will modify.
- Check BitLocker status; it may need to be suspended or decrypted depending on the scenario.
- Check the bootloader and Secure Boot requirements for the selected boot method.
- Be prepared to select the new Linux boot option manually if firmware does not do so automatically.

A successful run on one computer does not guarantee the same result on another. Firmware behaviour, storage layouts, encryption, filesystems, and distribution-specific boot requirements can all affect the outcome.

### Optional boot-manager enhancement

An optional rEFInd boot-manager path exists in the legacy Windows PowerShell installer. It is **currently untested** and should be treated as an experimental enhancement, not a required part of ULLI's normal workflow.

This feature is intended to be revisited using [RefindPlus](https://github.com/RefindPlusRepo/RefindPlus). RefindPlus support is planned for the legacy PowerShell installer first, with integration into the Qt 6/C++ version considered for a later stage. Neither integration should be considered ready until implemented and tested.

## Running the legacy installer

The legacy implementations remain in the repository.

**Linux**

```bash
sudo python3 ulli-linux.py
```

**Windows**

Extract the Windows package and run run-ulli-windows.bat as Administrator.

The Qt 6/C++ application is a development build, not a supported end-to-end installation path. Refer to the repository's build files and source tree for development work; do not use it for real disk installations yet.

## Development status

The Qt 6/C++ port is the main long-term direction and has reached a **buildable stage on Windows**. Basic UI, catalogue, and ISO-download testing is underway. Critical paths still require implementation and careful validation, including:

- Partition-layout execution and disk operations.
- Boot configuration and firmware interaction.
- Cancellation, cleanup, and rollback behaviour.
- Windows and Linux backend completeness.
- Automated tests, packaging, and real-hardware validation.
- Broader filesystem support.

Support for filesystems such as ext4 and Btrfs, as well as more capable cross-platform partitioning tools, remains future work.

Development follows an iterative process: **problem → hypothesis → implementation → test → diagnosis → modification → retest**. AI coding tools have been used during development, but generated code is not assumed to be correct; changes require review and testing.

For a non-AI version of the original project, see [ULLI-organic](https://github.com/rltvty2/ulli-organic).

## Contributing and reporting issues

Contributions are welcome. Useful ways to help include:

- Testing on different hardware and UEFI firmware configurations.
- Reporting reproducible bugs and unusual disk layouts.
- Reviewing partitioning, disk-identity, and boot-configuration logic.
- Improving documentation and distribution compatibility information.
- Contributing code through issues and pull requests.

When reporting a problem, include:

- Operating system and version.
- Hardware and disk layout.
- UEFI, Secure Boot, and BitLocker state.
- Selected distribution and ISO.
- Selected installation strategy.
- Expected versus actual behaviour.
- Relevant logs and reproducible steps.

Please avoid posting sensitive information, recovery keys, or personal data in public issue reports.

## Credits and license

ULLI-Next is based on [ULLI by rltvty2](https://github.com/rltvty2/ulli) and is maintained as a community continuation. The project is released under the **GNU General Public License v3.0 (GPL-3.0)**.

## Support the project ❤️

There are plenty of ways to support the project without donating: test early versions, report bugs, suggest improvements, contribute code, improve documentation, or share the project.

**UPI ID:** 8979584868@fam  
[Pay via UPI](upi://pay?pa=8979584868%40fam&pn=Prakhar%20Khokhar)

**Contact:** 8066prakhar@gmail.com · personnormal076@gmail.com
