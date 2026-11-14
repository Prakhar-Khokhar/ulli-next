#!/usr/bin/env python3
"""
Sync distribution catalogue from root distros.json to docs/data/distros.json.

Usage:
    python3 tools/sync-distros.py          # Sync root distros.json -> docs/data/distros.json
    python3 tools/sync-distros.py --check  # Verify docs/data/distros.json is in sync (exit 0 if clean, 1 if drift)
"""

import argparse
import json
import sys
from pathlib import Path

# ─── Configuration ────────────────────────────────────────────────────────────

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE_PATH = REPO_ROOT / "distros.json"
TARGET_PATH = REPO_ROOT / "docs" / "data" / "distros.json"

REQUIRED_FIELDS = {
    "label",
    "filename",
    "sha256",
    "mirrors",
    "download_page",
    "download_msg",
    "keyword",
    "validation_file",
    "is_hybrid",
}


# ─── Validation ───────────────────────────────────────────────────────────────

def validate_catalog(data: dict) -> list[str]:
    """Validate that catalogue data follows expected schema."""
    errors = []
    if not isinstance(data, dict):
        return ["Catalogue root must be a JSON object."]
    if not data:
        return ["Catalogue is empty."]

    for key, entry in data.items():
        if not isinstance(entry, dict):
            errors.append(f"Entry '{key}' must be an object.")
            continue
        missing = REQUIRED_FIELDS - entry.keys()
        if missing:
            errors.append(f"Entry '{key}' missing required fields: {', '.join(sorted(missing))}")
        if not isinstance(entry.get("mirrors"), list):
            errors.append(f"Entry '{key}': 'mirrors' must be a list.")
        if not isinstance(entry.get("is_hybrid"), bool):
            errors.append(f"Entry '{key}': 'is_hybrid' must be a boolean.")
        if not isinstance(entry.get("sha256"), str) or len(entry.get("sha256", "")) != 64:
            errors.append(f"Entry '{key}': 'sha256' must be a 64-character hex string.")

    return errors


# ─── Execution ────────────────────────────────────────────────────────────────

def run_sync() -> int:
    """Read source distros.json, validate, format, and write to docs/data/distros.json."""
    if not SOURCE_PATH.is_file():
        print(f"Error: Source catalogue not found at {SOURCE_PATH}", file=sys.stderr)
        return 1

    try:
        with open(SOURCE_PATH, "r", encoding="utf-8") as f:
            raw_content = f.read()
            data = json.loads(raw_content)
    except Exception as e:
        print(f"Error reading {SOURCE_PATH}: {e}", file=sys.stderr)
        return 1

    errors = validate_catalog(data)
    if errors:
        print(f"Error: Validation failed for {SOURCE_PATH}:", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    # Format JSON with 4-space indentation matching root distros.json and trailing newline
    formatted = json.dumps(data, indent=4, ensure_ascii=False) + "\n"

    TARGET_PATH.parent.mkdir(parents=True, exist_ok=True)
    try:
        with open(TARGET_PATH, "w", encoding="utf-8") as f:
            f.write(formatted)
        print(f"✓ Successfully synced {len(data)} distributions from distros.json to {TARGET_PATH.relative_to(REPO_ROOT)}")
        return 0
    except Exception as e:
        print(f"Error writing to {TARGET_PATH}: {e}", file=sys.stderr)
        return 1


def run_check() -> int:
    """Check whether docs/data/distros.json exists and matches distros.json."""
    if not SOURCE_PATH.is_file():
        print(f"Error: Source catalogue not found at {SOURCE_PATH}", file=sys.stderr)
        return 1

    if not TARGET_PATH.is_file():
        print(f"✗ Target catalogue does not exist at {TARGET_PATH.relative_to(REPO_ROOT)}.", file=sys.stderr)
        print("  Run 'python3 tools/sync-distros.py' to generate it.", file=sys.stderr)
        return 1

    try:
        with open(SOURCE_PATH, "r", encoding="utf-8") as f:
            source_data = json.load(f)
        with open(TARGET_PATH, "r", encoding="utf-8") as f:
            target_data = json.load(f)
    except Exception as e:
        print(f"Error reading JSON files during check: {e}", file=sys.stderr)
        return 1

    source_errors = validate_catalog(source_data)
    if source_errors:
        print(f"Error: Source catalogue invalid: {source_errors}", file=sys.stderr)
        return 1

    target_errors = validate_catalog(target_data)
    if target_errors:
        print(f"Error: Target catalogue invalid: {target_errors}", file=sys.stderr)
        return 1

    if source_data != target_data:
        print(f"✗ Catalogue mismatch: {TARGET_PATH.relative_to(REPO_ROOT)} is out of sync with {SOURCE_PATH.name}!", file=sys.stderr)
        print("  Run 'python3 tools/sync-distros.py' to synchronize.", file=sys.stderr)
        return 1

    print(f"✓ docs/data/distros.json is in sync with distros.json ({len(source_data)} distributions verified).")
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Synchronize distribution catalogue between root distros.json and docs/data/distros.json"
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Check whether docs/data/distros.json matches root distros.json without modifying files.",
    )
    args = parser.parse_args()

    if args.check:
        sys.exit(run_check())
    else:
        sys.exit(run_sync())


if __name__ == "__main__":
    main()
