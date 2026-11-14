#!/usr/bin/env python3
"""
Automated validation script for the ULLI-Next website.
Validates:
1. File existence of all required docs/ deliverables.
2. Internal link and asset reference resolution (no broken relative paths).
3. Internal anchor references (href='#id' must resolve to an existing id).
4. Duplicate HTML id attributes.
5. JSON catalogue schema, data synchronization, and hash/URL validity.
6. URL schemes (only http:// and https:// allowed).
7. CSS and JS file presence and non-emptiness.
8. No unwanted leakage (personal email / UPI details).
"""

import html.parser
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DOCS_DIR = REPO_ROOT / "docs"

# Deployment base path (GitHub Pages project site). Root-absolute URLs in
# 404.html use this prefix so they resolve from nested URLs.
DEPLOY_BASE = "/ulli-next"

REQUIRED_FILES = [
    DOCS_DIR / "index.html",
    DOCS_DIR / "404.html",
    DOCS_DIR / "robots.txt",
    DOCS_DIR / "sitemap.xml",
    DOCS_DIR / "README.md",
    DOCS_DIR / "data" / "distros.json",
    DOCS_DIR / "assets" / "css" / "style.css",
    DOCS_DIR / "assets" / "js" / "main.js",
    DOCS_DIR / "assets" / "brand" / "ULLI-Next-Logo.png",
    DOCS_DIR / "assets" / "brand" / "ULLI-Next-Social-Preview.png",
    DOCS_DIR / "assets" / "brand" / "illustration-workflow.svg",
    DOCS_DIR / "assets" / "brand" / "illustration-comparison.svg",
]


class AssetLinkExtractor(html.parser.HTMLParser):
    def __init__(self, base_file: Path):
        super().__init__()
        self.base_file = base_file
        self.assets = []
        self.internal_anchors = []
        self.ids = []
        self.url_schemes = []

    def handle_starttag(self, tag, attrs):
        attr_dict = dict(attrs)
        # Check href (link, a)
        if "href" in attr_dict:
            href = attr_dict["href"]
            if href.startswith("#"):
                self.internal_anchors.append(href)
            elif not href.startswith(("http://", "https://", "mailto:", "upi:", "javascript:")):
                self.assets.append(href.split("#")[0])
            else:
                self.url_schemes.append(href.split(":")[0])

        # Check src (img, script)
        if "src" in attr_dict:
            src = attr_dict["src"]
            if not src.startswith(("http://", "https://")):
                self.assets.append(src)
            else:
                self.url_schemes.append(src.split(":")[0])

        # Collect id attributes
        if "id" in attr_dict:
            self.ids.append(attr_dict["id"])


def check_files_exist():
    print("Checking required files...")
    all_ok = True
    for path in REQUIRED_FILES:
        if not path.is_file():
            print(f"  ✗ Missing required file: {path.relative_to(REPO_ROOT)}")
            all_ok = False
        else:
            size = path.stat().st_size
            if size == 0:
                print(f"  ✗ Empty file: {path.relative_to(REPO_ROOT)}")
                all_ok = False
            else:
                print(f"  ✓ {path.relative_to(REPO_ROOT)} ({size:,} bytes)")
    return all_ok


def check_asset_references():
    print("\nValidating relative asset and link resolution in HTML files...")
    all_ok = True
    for html_file in [DOCS_DIR / "index.html", DOCS_DIR / "404.html"]:
        content = html_file.read_text(encoding="utf-8")
        parser = AssetLinkExtractor(html_file)
        parser.feed(content)

        print(f"  Checking {html_file.name} references ({len(parser.assets)} assets found)...")
        for asset_ref in set(parser.assets):
            if not asset_ref or asset_ref == "./":
                continue
            # Handle root-absolute URLs that use the deployment base path
            # (e.g. /ulli-next/assets/... -> docs/assets/...)
            if asset_ref.startswith("/"):
                if asset_ref.startswith(DEPLOY_BASE):
                    asset_ref = asset_ref[len(DEPLOY_BASE):].lstrip("/")
                else:
                    asset_ref = asset_ref.lstrip("/")
            # Resolve relative to html_file's parent (docs/)
            target_path = (html_file.parent / asset_ref).resolve()
            if not target_path.exists():
                print(f"    ✗ Broken asset reference in {html_file.name}: '{asset_ref}' -> {target_path}")
                all_ok = False
            else:
                print(f"    ✓ Resolved: '{asset_ref}'")
    return all_ok


def check_internal_anchors():
    print("\nValidating internal anchor references (href='#id' must resolve to an id)...")
    all_ok = True
    for html_file in [DOCS_DIR / "index.html", DOCS_DIR / "404.html"]:
        content = html_file.read_text(encoding="utf-8")
        parser = AssetLinkExtractor(html_file)
        parser.feed(content)
        defined_ids = set(parser.ids)
        print(f"  Checking {html_file.name} ({len(parser.internal_anchors)} internal anchors)...")
        for anchor in parser.internal_anchors:
            anchor_id = anchor[1:]
            if anchor_id not in defined_ids:
                print(f"    ✗ Unresolved anchor in {html_file.name}: '{anchor}' -> no element with id='{anchor_id}'")
                all_ok = False
            else:
                print(f"    ✓ Resolved: '{anchor}' -> id='{anchor_id}'")
    return all_ok


def check_duplicate_ids():
    print("\nChecking for duplicate HTML id attributes...")
    all_ok = True
    for html_file in [DOCS_DIR / "index.html", DOCS_DIR / "404.html"]:
        content = html_file.read_text(encoding="utf-8")
        parser = AssetLinkExtractor(html_file)
        parser.feed(content)
        seen = {}
        for id_val in parser.ids:
            if id_val in seen:
                print(f"    ✗ Duplicate id '{id_val}' in {html_file.name}")
                all_ok = False
            else:
                seen[id_val] = 1
        if seen:
            print(f"  Checked {html_file.name}: {len(seen)} unique ids")
    return all_ok


def check_url_schemes():
    print("\nValidating URL schemes (only http:// and https:// allowed)...")
    all_ok = True
    for html_file in [DOCS_DIR / "index.html", DOCS_DIR / "404.html"]:
        content = html_file.read_text(encoding="utf-8")
        parser = AssetLinkExtractor(html_file)
        parser.feed(content)
        bad_schemes = [s for s in set(parser.url_schemes) if s.lower() not in ("http", "https")]
        if bad_schemes:
            for scheme in bad_schemes:
                print(f"    ✗ Unsafe URL scheme '{scheme}:' in {html_file.name}")
                all_ok = False
        else:
            print(f"  Checked {html_file.name}: all external URLs use http/https")
    return all_ok


def check_catalogue_hashes():
    print("\nValidating catalogue SHA-256 hashes (64-char hex strings)...")
    all_ok = True
    for json_path in [REPO_ROOT / "distros.json", DOCS_DIR / "data" / "distros.json"]:
        if not json_path.is_file():
            continue
        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        print(f"  Checking {json_path.relative_to(REPO_ROOT)}...")
        for key, entry in data.items():
            sha = entry.get("sha256", "")
            if not isinstance(sha, str) or len(sha) != 64 or not all(c in "0123456789abcdef" for c in sha.lower()):
                print(f"    ✗ Invalid SHA-256 for '{key}': '{sha}'")
                all_ok = False
            else:
                print(f"    ✓ '{key}': valid SHA-256")
    return all_ok


def check_catalogue_urls():
    print("\nValidating catalogue URL schemes (http/https only)...")
    all_ok = True
    for json_path in [REPO_ROOT / "distros.json", DOCS_DIR / "data" / "distros.json"]:
        if not json_path.is_file():
            continue
        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        print(f"  Checking {json_path.relative_to(REPO_ROOT)}...")
        for key, entry in data.items():
            url = entry.get("download_page", "")
            if url and not url.startswith(("http://", "https://")):
                print(f"    ✗ Unsafe URL in {key}.download_page: '{url}'")
                all_ok = False
            for mirror in entry.get("mirrors", []):
                if not mirror.startswith(("http://", "https://")):
                    print(f"    ✗ Unsafe mirror URL in {key}: '{mirror}'")
                    all_ok = False
        if all_ok:
            print(f"    ✓ All URLs use http/https")
    return all_ok


def check_catalogue():
    print("\nChecking catalogue synchronization...")
    root_json = REPO_ROOT / "distros.json"
    docs_json = DOCS_DIR / "data" / "distros.json"

    if not root_json.is_file() or not docs_json.is_file():
        print("  ✗ Catalogue file missing!")
        return False

    with open(root_json, "r", encoding="utf-8") as f:
        root_data = json.load(f)
    with open(docs_json, "r", encoding="utf-8") as f:
        docs_data = json.load(f)

    if root_data != docs_data:
        print("  ✗ docs/data/distros.json is out of sync with root distros.json!")
        return False

    print(f"  ✓ Catalogue in sync ({len(root_data)} distributions verified).")
    return True


def check_no_unwanted_leakage():
    print("\nVerifying privacy and compliance (no personal email / UPI details in docs/)...")
    content = ""
    for f in DOCS_DIR.rglob("*"):
        if f.is_file() and f.suffix in [".html", ".js", ".css", ".md"]:
            content += f.read_text(encoding="utf-8", errors="ignore")

    leak_patterns = [
        r"8066prakhar@gmail\.com",
        r"personnormal076@gmail\.com",
        r"8979584868@fam",
        r"upi://pay",
    ]
    all_ok = True
    for pattern in leak_patterns:
        if re.search(pattern, content, re.IGNORECASE):
            print(f"  ✗ Found unwanted sensitive detail matching '{pattern}' in website files!")
            all_ok = False
        else:
            print(f"  ✓ Clean: no matches for '{pattern}'")
    return all_ok


def check_social_preview():
    print("\nValidating social-preview image (1200x630 PNG)...")
    all_ok = True
    path = DOCS_DIR / "assets" / "brand" / "ULLI-Next-Social-Preview.png"
    if not path.is_file():
        print(f"  ✗ Missing social-preview image: {path.relative_to(REPO_ROOT)}")
        return False
    try:
        from PIL import Image
        with Image.open(path) as img:
            w, h = img.size
            fmt = img.format
            if (w, h) != (1200, 630):
                print(f"  ✗ Social-preview size {w}x{h}, expected 1200x630")
                all_ok = False
            else:
                print(f"  ✓ {path.relative_to(REPO_ROOT)} ({w}x{h} {fmt})")
            if fmt != "PNG":
                print(f"  ✗ Social-preview format {fmt}, expected PNG")
                all_ok = False
    except ImportError:
        print(f"  ✓ {path.relative_to(REPO_ROOT)} exists (PIL not available for dimension check)")
    except Exception as e:
        print(f"  ✗ Could not read social-preview image: {e}")
        all_ok = False
    return all_ok


def main():
    results = [
        check_files_exist(),
        check_asset_references(),
        check_internal_anchors(),
        check_duplicate_ids(),
        check_url_schemes(),
        check_catalogue_hashes(),
        check_catalogue_urls(),
        check_catalogue(),
        check_no_unwanted_leakage(),
        check_social_preview(),
    ]
    if all(results):
        print("\nAll website validation checks passed successfully!")
        return 0
    else:
        print("\nSome validation checks failed!", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
