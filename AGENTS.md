# ULLI Agent Guidelines

## Project Overview

ULLI (USB-less Linux Installer) is a tool for installing Linux distributions directly to a hard drive without a USB stick. It provides GUI interfaces for both Linux (Python/GTK 3) and Windows (PowerShell/WindowsForms) platforms.

**Supported distributions:** Linux Mint 22.3, Ubuntu 24.04.4, Kubuntu 24.04.4, Debian Live 13.3.0, Fedora 43 KDE

## Running the Project

### Dependencies

**Linux:**
```bash
# Install Python dependencies
pip3 install requests

# Install system dependencies
sudo apt install python3-gi gir1.2-gtk-3.0 gir1.2-vte-2.91 parted btrfs-progs grub-common grub2-common e2fsprogs fdisk util-linux ntfs-3g
```

**Windows:**
```powershell
# Ensure Windows 10/11 with UEFI firmware (Secure Boot can stay on; only required to be off if installing rEFInd)
# No additional pip requirements needed for PowerShell version
```

### Running the Application

**Linux:**
```bash
sudo python3 linux/ulli-linux.py
```

**Windows:**
```powershell
powershell -ExecutionPolicy Bypass -File windows/run-ulli-windows.bat
# Or run as Administrator
```

### Checking Dependencies

**Linux:**
```bash
sudo python3 linux/ulli-linux.py --check-deps
```

**Windows:**
See embedded dependency checks in the script or comments

## Testing

**Note:** No automated tests exist in the codebase. When making modifications:

1. Manual testing: Run the application in a virtual machine first
2. Verify both GUI functionality and command execution
3. Test with different Linux distributions
4. Check error handling for edge cases
5. Test both Linux and Windows versions if applicable

## Code Style Guidelines

### General Principles
- Use consistent code style across both Linux and Windows versions
- Follow Unicode box drawing characters in comments: `# ─── section ───────`
- Include docstrings/doc comments for all public functions/modules
- Maintain descriptive inline comments explaining complex logic
- Use clear, consistent variable and function names

### Linux (Python) Conventions

**Imports:**
- Use absolute imports from pathlib: `from pathlib import Path`
- Group imports by standard library, external libraries, and local modules
- Format as: `import os, sys, subprocess` (multiple imports on one line)
- For specific imports: `from gi.repository import Gtk, Gdk` (use `gi.repository` for GTK)

**Naming:**
- Function naming: `lowercase_with_underscores`
- Class naming: `PascalCase`
- Constants: `UPPERCASE_WITH_UNDERSCORES`

**Code Structure:**
- Line length: Aim for readability (~80-100 characters)
- Keep functions focused and small
- Use `get_` or `run_` prefixes for helper functions
- Group related functions logically (helpers, disk operations, UI components)
- Maintain consistent indentation (4 spaces)

### Windows (PowerShell) Conventions

**Function Naming:**
- Use PowerShell verb prefixes: `Get-*`, `Set-*`, `Start-*`, `Stop-*`, `Add-*`, `Remove-*`, `Invoke-*`
- CamelCase for parameter names

**Variable Naming:**
- Use `$script:variable` for module-scoped variables (preferred) or `$variable` for local
- Use CamelCase for variable names (preferred)

**Data Structures:**
- Use ordered dictionaries `[ordered]@{}` to maintain insertion order
- Consistent spacing around operators

**Code Structure:**
- PowerShell script block formatting with consistent indentation (usually 2 spaces)
- Use `#` for comments
- Keep functions focused on single responsibilities

### Shared Conventions

**Documentation Comments:**
- Use Unicode box drawing characters: `# ─── constant ───────`
- Comment purpose and behavior immediately before code
- Reference specific sections when commenting complex logic

**Error Handling:**
- Always handle exceptions contextually with `try/catch` blocks
- Provide user-friendly error messages
- Log errors to console and UI
- Check return codes before processing output

**Constants:**
- Define module-level constants at the top of script
- Use clear meaningful names like `MIN_BOOT_GB`, `MIN_LINUX_GB`, `DISTROS`
- Group related constants in dictionaries or tables

**Code Execution:**
- Capture and validate command output
- Handle subprocess failures explicitly
- Use helper functions for common operations
- Reference appropriate platform-specific tools (e.g., `parted`, `win32diskmanagement`)

### UI Development

**Linux:**
- Use `Gtk.Application` and `Gtk.ApplicationWindow` base classes
- Follow GTK naming conventions (prefix internal methods with `_`)

**Windows:**
- Use `System.Windows.Forms` classes
- Enable visual styles with `EnableVisualStyles()`
- Follow WindowsForms control patterns

**Shared:**
- Maintain consistent UI layout across platforms
- Use meaningful labels and error messages
- Provide progress feedback for long-running operations