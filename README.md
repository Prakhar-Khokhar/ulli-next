# ULLI-Next

### A community-maintained fork of ULLI

Original code by [rltvty2](https://github.com/rltvty2/)

Improved and currently maintained by **Prakhar Khokhar**

**ULLI-Next** is a USB-less Linux installer: it is designed to install a bootable Linux environment directly onto an internal drive, allowing a user to try or install Linux without first creating a bootable USB stick.

The project is evolving from the original Python/PowerShell implementation into a native **Qt 6 / C++20** application with a more structured installation engine, explicit installation plans, platform backends, stronger validation, and a graphical partition/layout preview.

> **⚠️ BETA / ACTIVE DEVELOPMENT**
>
> ULLI-Next performs real disk-partitioning and UEFI boot operations. A failure or unexpected hardware/firmware behavior can leave a system unbootable or cause data loss.
>
> **Back up important data before using ULLI-Next.**
>
> The newer Qt/C++ implementation is still under active development and should be considered experimental.

---

## Why ULLI?

Installing Linux normally involves downloading an ISO, writing it to a USB drive, rebooting from that USB, and then running the distribution installer.

ULLI takes a different approach.

Instead of requiring a USB stick, ULLI can place the Linux live environment onto a partition on an internal drive and configure the system's UEFI boot path to start it.

This makes it useful when:

- You do not have a USB drive available.
- You want to try Linux without preparing installation media.
- You want a convenient way to start a Linux live environment from an existing Windows installation.
- You want to use existing free disk space instead of a removable drive.
- You want to install Linux on a secondary internal drive.

The original ULLI project established this USB-less installation concept; ULLI-Next keeps that core idea while rebuilding substantial parts of the architecture. citeturn0search0

---

# How ULLI-Next works

At a high level, the installer follows this process:

```text
Choose Linux distribution / ISO
          ↓
Create an installation plan
          ↓
Inspect disks and validate the plan
          ↓
Download / locate the ISO
          ↓
Verify the ISO with SHA-256
          ↓
Resize / select the target storage
          ↓
Create the required partition layout
          ↓
Copy the live Linux environment
          ↓
Apply distribution-specific boot configuration
          ↓
Configure UEFI boot
          ↓
Reboot into Linux
```

The Qt/C++ installation engine now models these stages explicitly. The current engine performs pre-flight validation, ISO resolution/checking, optional partition resizing or disk wiping, partition creation, ISO copying, distro-specific boot configuration, optional rEFInd installation, UEFI boot-entry creation, and cleanup. fileciteturn6file0

The important architectural change is that the UI does not directly perform all of these operations itself. Instead, it builds an `InstallPlan` and passes that plan to an installation engine and platform backend.

That separation makes it much easier to audit and eventually support different operating systems and partitioning backends.

---

# Current project status

ULLI-Next currently contains **two generations of the installer**:

### Legacy implementation

The original Python/Linux and PowerShell/Windows implementation remains in the repository and provides the project's existing USB-less installation workflow.

### Qt/C++ implementation

The main development direction is now the **Qt 6 / C++20 port**.

The Qt port is no longer just a UI prototype. It has a structured core installation engine, distribution catalog, installation-plan model, platform abstraction, Windows backend components, Linux backend components, GUI components, and unit-test infrastructure.

The current CMake configuration targets **C++20** and **Qt 6.5+** and separates core, platform, UI, application, and test sources. fileciteturn1file0

---

# Current features

## 🐧 USB-less Linux installation

The central feature remains the same as the original ULLI:

**Install/boot Linux without first creating a USB installer.**

Linux files are placed on a disk partition and the machine is configured to boot from that environment.

This is particularly useful for quickly trying a Linux distribution or beginning a Linux installation from an existing Windows system.

Custom ISOs: Install distributions outside the built-in catalog using your own Linux ISO, with Live ISOs preferred. Current limits are 7 GB per ISO and 4 GB per individual file; larger-image support is planned.
---

## 💿 Distribution catalog

ULLI-Next maintains a machine-readable distribution catalog in [`distros.json`](./distros.json).

The current catalog contains:

| Distribution | Edition |
|---|---|
| Linux Mint 22.3 "Zena" | Cinnamon |
| Ubuntu 24.04.4 LTS | GNOME |
| Kubuntu 24.04.4 LTS | KDE Plasma |
| Debian Live 13.6.0 | KDE |
| CachyOS | Desktop |
| Fedora 43 | KDE Plasma Desktop |

The catalog has recently been updated with newer distro versions, ISO filenames, download mirrors, official download pages, validation information, and SHA-256 checksums. fileciteturn2file0

Because this information is kept outside the installation engine, distro updates can be made without rewriting the core installer.

---

## 🌐 Multiple ISO mirrors

Supported distributions can have multiple download mirrors.

This makes the downloader less dependent on a single server and allows the catalog to provide alternative sources when a particular mirror is slow or unavailable.

The current catalog contains multiple mirrors for several distributions, including Linux Mint, Ubuntu, Kubuntu, Debian, and Fedora. fileciteturn2file0

---

## 🔐 SHA-256 ISO verification

ULLI-Next records the expected SHA-256 checksum for each supported ISO.

The installer can resolve the selected ISO and verify it before using it.

This helps detect:

- Corrupted downloads
- Incomplete downloads
- Incorrect ISO files
- Mismatched distro versions

The SHA-256 values are maintained alongside the distro metadata rather than being hard-coded throughout the installation logic.

---

# Installation planning

One of the major improvements over the original architecture is the introduction of an explicit **installation plan**.

An `InstallPlan` describes the intended operation before destructive actions begin.

It can contain:

- Selected distribution
- ISO path
- Target disk
- Volume to shrink
- Amount of storage to free
- Linux storage size
- Boot/staging partition size
- Optional rEFInd partition
- Partitioning strategy
- Boot mode
- Allocation mode
- Automatic restart preference
- ISO cleanup preference

The plan is validated before disk mutation and is revalidated at important points before destructive operations. fileciteturn5file0 fileciteturn6file0

This is intended to make the installer safer and easier to reason about than a large collection of directly executed commands.

---

# Partitioning options

The current installation model supports several storage strategies:

### Shrink the existing Windows/system volume

Create space for Linux by shrinking an existing partition.

### Use existing unallocated space

Use free space that already exists without unnecessarily modifying an occupied Windows volume.

### Combine free space and shrinking

Use existing free space while also shrinking a volume when additional space is required.

### Use another drive

Install onto available space on a secondary disk.

### Shrink a volume on another drive

Free space by shrinking a volume on a non-system drive.

### Wipe a target disk

Explicitly wipe a selected disk and create a new layout.

The wipe operation is intentionally represented as a separate, destructive strategy rather than being hidden inside normal installation logic.

---

# Live-only and full-install allocation

ULLI-Next now distinguishes between two broad allocation modes.

### Live Only

Creates the approximately **7 GiB FAT32 staging partition** required for the live environment.

This is intended for users who primarily want to boot and try the Linux environment.

### Full Install

Creates the live/staging area while also providing additional unallocated Linux space intended for a subsequent persistent Linux installation.

The current planning model uses approximately **30 GiB as the minimum full-install Linux allocation**, with the staging partition handled separately. fileciteturn5file0

This is intended to make the distinction between:

> "I only want to boot Linux"

and

> "I want to use this as the starting point for a real Linux installation"

explicit in the installation plan.

---

# Windows integration

The Qt/C++ Windows backend is split into dedicated components rather than putting every Windows operation into one script.

Current Windows-specific components include:

- Disk operations
- WMI disk enumeration
- BCD store handling
- EFI System Partition operations
- BitLocker handling
- SHA-256 verification

These are connected to the platform abstraction used by the core installation engine. fileciteturn1file0

---

# Boot configuration

ULLI-Next can use a direct UEFI boot path or an optional rEFInd-based boot path.

### Direct UEFI boot

The intended direct mode boots from the newly created FAT32 boot/staging partition using its UEFI loader.

### rEFInd

ULLI also retains support for installing **rEFInd**, providing a more traditional graphical boot-manager experience.

**rEFInd requires Secure Boot to be disabled.**

The original ULLI project also provides rEFInd as an optional boot-management path. citeturn0search0

---

# Windows boot recovery and safety

The installer treats boot configuration as something that can fail and therefore keeps rollback information during the installation process.

The current engine also:

- Checks the installation plan before execution.
- Revalidates the plan before destructive operations.
- Revalidates before creating the final layout.
- Can cancel an installation between stages.
- Attempts to roll back boot entries created during a failed/cancelled operation.

These mechanisms are intended to reduce the chance of leaving the Windows boot configuration in a broken state. fileciteturn6file0

**They are not a substitute for backups.**

---

# BitLocker and Secure Boot

Depending on the installation strategy and machine configuration:

- BitLocker may need to be disabled or decrypted.
- Secure Boot may need to be disabled.
- UEFI firmware behavior can vary between manufacturers.

rEFInd specifically requires Secure Boot to be disabled.

The original ULLI documentation similarly warns users about BitLocker, Secure Boot, and firmware-specific boot-order behavior. citeturn0search0

---

# Graphical interface

The Qt port includes a dedicated GUI layer with components for:

- Main application window
- Distribution selection
- Installation planning
- Partition editing
- Partition/layout preview
- Progress reporting
- Installation logs
- Restart countdown

The intention is that the user should be able to understand the proposed disk changes **before** the installer performs them.

---

# Installation engine

The Qt implementation has a dedicated `InstallEngine` that coordinates the installation stages.

The current flow includes:

1. Pre-flight checks
2. ISO resolution and verification
3. Partition resizing when required
4. Optional disk wiping
5. Partition/filesystem creation
6. ISO mounting and file copying
7. Distribution-specific boot configuration
8. Optional rEFInd installation
9. UEFI boot-entry creation
10. Cleanup/completion

Progress is exposed through explicit installation stages, and cancellation is checked throughout the process. fileciteturn6file0

This is one of the biggest architectural differences from the original implementation: the installation process is now represented as a structured engine rather than being primarily a platform script.

---

# Testing

The Qt project has a dedicated test infrastructure through CMake/CTest.

Testing is being expanded alongside the new installation engine and storage logic.

The goal is to test dangerous logic independently wherever possible before relying on real disk operations.

Real-hardware testing is still important because partitioning and UEFI behavior cannot be completely reproduced by unit tests.

---

# Legacy implementation

The original implementation remains available.

## Linux

```bash
sudo python3 ulli-linux.py
```

## Windows

Extract the Windows package and run:

```text
run-ulli-windows.bat
```

as Administrator.

The legacy implementation is retained both as a usable implementation and as an important reference while the Qt/C++ version is developed.

---

# Roadmap

The Qt/C++ port is the primary long-term direction of ULLI-Next.

## Near-term goals

- [ ] Finish the remaining Windows implementation work.
- [ ] Reach a reliably buildable native Windows `.exe`.
- [ ] Complete end-to-end installation testing on real systems.
- [ ] Test every partitioning strategy against different disk layouts.
- [ ] Improve rollback and recovery behavior.
- [ ] Finish the remaining boot-architecture work.
- [ ] Expand automated testing.
- [ ] Improve error reporting and diagnostics.
- [ ] Package the application for normal end users.

---

# External partitioning backend

One of the major planned architectural improvements is to reduce reliance on the default Windows partition-management commands and APIs.

The current Windows implementation necessarily works around Windows' native storage capabilities. In the longer term, ULLI-Next will investigate using **external/native partitioning tooling** where appropriate.

The motivation is broader than simply replacing one command.

A more capable partitioning backend could provide:

- Better filesystem awareness
- More reliable resizing
- Support for filesystems Windows does not natively understand
- Better handling of unusual partition layouts
- More consistent behavior across different storage configurations
- A path toward using similar partitioning logic on Linux

This will be approached cautiously because the partitioning backend is the most safety-critical part of the application.

---

# Broader filesystem support

Another major future goal is expanding filesystem support beyond what Windows can manipulate natively.

Potential future targets include:

- **ext4**
- **Btrfs**
- **XFS**
- Other Linux filesystems where technically practical

The goal is not merely to detect these filesystems. Eventually, ULLI-Next should be able to make sensible installation and partitioning decisions around them.

This is especially important for secondary drives and systems that already contain Linux installations.

---

# More distributions

The distro catalog is deliberately designed to be easy to update.

Future work includes:

- More distributions
- More desktop environments
- Newer release versions
- Better mirror handling
- Better metadata validation
- More robust ISO discovery
- Community-submitted distro definitions

Custom ISO support will also remain an important part of the project where technically possible.

---

# Better cross-platform storage handling

The long-term goal is for the core installation engine to remain largely platform-independent while the actual disk operations are supplied by platform-specific backends.

That architecture should make it possible to have:

```text
                 ULLI-Next Core
                       │
             ┌─────────┴─────────┐
             │                   │
       Windows Backend      Linux Backend
             │                   │
     Windows storage APIs   Linux tooling
     / future partitioner   / future partitioner
```

This is also why external partitioning tooling is being considered: the core should describe **what storage operation is needed**, while the backend decides **how to perform it safely on that operating system**.

---

# Development philosophy

ULLI-Next is being developed with a strong emphasis on testing and iterative diagnosis, especially because this project operates on real storage devices.

The general workflow is:

**problem → hypothesis → implementation → test → failure/diagnosis → modification → retest**

AI coding tools have been used extensively during development, but generated code is not treated as automatically correct. Changes are reviewed, tested, audited, and modified before being considered part of the intended implementation.

---

# AI acknowledgement

AI, particularly Claude and other coding models, has been used during the development of ULLI-Next.

This is disclosed because AI-generated code can have different review and provenance considerations.

For users who specifically want a non-AI version of the original ULLI project, the original developer also maintains **ULLI-organic**:

https://github.com/rltvty2/ulli-organic

---

# Contributing

ULLI-Next is currently under active development, and contributions are welcome.

Especially useful contributions include:

- Finding and reporting bugs
- Testing on different hardware
- Testing unusual partition layouts
- Testing different UEFI implementations
- Testing different Linux distributions
- Finding filesystem compatibility problems
- Reviewing partitioning logic
- Improving the Qt interface
- Improving documentation
- Investigating external partitioning backends
- Improving Linux support
- Improving Windows recovery behavior
- Adding automated tests

When reporting a problem, please include as much information as possible:

- Operating system and version
- Hardware/storage configuration
- UEFI/Secure Boot state
- BitLocker state
- Distribution/ISO
- Intended installation strategy
- What you expected
- What actually happened
- Relevant logs/error messages
- Exact steps needed to reproduce the problem

---

# Issues, feedback and contact

For **issues, feedback, contributing, testing results, pinpointing bugs, or general suggestions**, contact:

- **8066prakhar@gmail.com**
- **personnormal076@gmail.com**

GitHub Issues and Pull Requests are also welcome.

---

# Support the project ❤️

If ULLI-Next is useful to you and you would like to support its development, small donations are appreciated.

**UPI ID:**

```text
8979584868@fam
```

**UPI payment link:**

[Pay via UPI](upi://pay?pa=8979584868%40fam&pn=Prakhar%20Khokhar)

**Recipient:** Prakhar Khokhar

You can also support the project without donating by:

- Testing early versions
- Reporting bugs
- Pinpointing difficult failures
- Suggesting improvements
- Contributing code
- Improving documentation
- Sharing the project

---

# Currently supported distributions

| Distribution | Edition | ISO | SHA-256 |
|---|---|---|---|
| Linux Mint 22.3 "Zena" | Cinnamon | `linuxmint-22.3-cinnamon-64bit.iso` | `a081ab202cfda17f6924128dbd2de8b63518ac0531bcfe3f1a1b88097c459bd4` |
| Ubuntu 24.04.4 LTS | GNOME | `ubuntu-24.04.4-desktop-amd64.iso` | `3a4c9877b483ab46d7c3fbe165a0db275e1ae3cfe56a5657e5a47c2f99a99d1e` |
| Kubuntu 24.04.4 LTS | KDE Plasma | `kubuntu-24.04.4-desktop-amd64.iso` | `02cda2568cb96c090b0438a31a7d2e7b07357fde16217c215e7c3f45263bcc49` |
| Debian Live 13.6.0 | KDE | `debian-live-13.6.0-amd64-kde.iso` | `426984f7edf034f4cd49f6218e706a6086588359d34fa0328676451b4a679639` |
| CachyOS | Desktop | `cachyos-desktop-linux-260809.iso` | `959f6577f45e25ee9fd8c220fd221b08e4ea79412c7315c0f922dd6d86d5e33c` |
| Fedora 43 | KDE Plasma Desktop | `Fedora-KDE-Desktop-Live-43-1.6.x86_64.iso` | `181fe3e265fb5850c929f5afb7bdca91bb433b570ef39ece4a7076187435fdab` |

The authoritative mirror URLs, download pages, filenames, and checksums are maintained in [`distros.json`](./distros.json). fileciteturn2file0

---

### Custom ISO Support

In addition to the distributions available in the built-in distro catalog, ULLI-Next also provides a **Custom ISO** option, allowing you to install your favorite Linux distribution even when it is not yet included in the catalog.

For the best compatibility, we recommend using **Live installation ISOs**, as these are designed to boot into a usable Linux environment and generally fit ULLI-Next's current installation model.

Custom ISO support is currently subject to two limitations:

* The ISO image itself must not exceed **7 GB**.
* No individual file contained inside the ISO may exceed **4 GB**.

Because of these limitations, some modern distributions that ship very large installation images or contain files larger than 4 GB — such as **Bazzite and similar distributions** — cannot currently be installed through ULLI-Next.

These are temporary limitations rather than a fundamental restriction of the project. **Support for larger ISOs and files exceeding the current limits is planned and will be added in a future version**, allowing ULLI-Next to work with a substantially wider range of Linux distributions and installation images.

---

# Important notes

- ULLI-Next is **beta software**.
- Back up important data before using it.
- Partitioning operations can cause permanent data loss.
- BitLocker may need to be disabled or decrypted depending on the installation scenario.
- Secure Boot may need to be disabled depending on the selected boot configuration.
- rEFInd requires Secure Boot to be disabled.
- UEFI firmware behavior varies between systems.
- ULLI attempts to configure Linux as a boot option automatically, but some firmware may require manual boot-order changes.
- Distribution support and filesystem compatibility are still being expanded.
- A successful installation on one computer does not guarantee identical behavior on another computer.

---

# Credits

ULLI-Next is based on the original **ULLI — USB-less Linux Installer** by **rltvty2**.

The original project is available here:

https://github.com/rltvty2/ulli

ULLI-Next is a community-maintained fork focused on extending the original concept and developing a newer Qt/C++ architecture.

---

# License

ULLI-Next is released under the **GNU General Public License v3.0 (GPL-3.0)**.

You are free to use, modify, and redistribute the software under the terms of the license.

