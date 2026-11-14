# ULLI-Next Website & Documentation

This directory contains the static website for ULLI-Next, published via GitHub Pages from the `main` branch under the `/docs` folder.

Target deployment URL: `https://prakhar-khokhar.github.io/ulli-next/`

## Directory Structure

```
docs/
├── index.html                   # Main single-page landing site
├── 404.html                     # Custom 404 error page
├── robots.txt                   # Search crawler directives
├── sitemap.xml                  # XML sitemap with canonical deployment URL
├── README.md                    # Website architecture & maintenance documentation
├── data/
│   └── distros.json             # Synchronized mirror of root distros.json (do NOT edit manually)
└── assets/
    ├── brand/
    │   ├── favicon.svg          # Retained legacy SVG favicon
    │   ├── logo-mark.svg        # Retained legacy Stack Arrow mark
    │   ├── logo.svg             # Retained legacy full brand identity
    │   ├── logo-monochrome.svg  # Retained legacy single-color brand mark
    │   ├── ULLI-Next-Logo.png   # Active brand logo (blue-to-violet, 320x318)
    │   ├── ULLI-Next-Social-Preview.png  # Active social preview image (1200x630)
    │   ├── illustration-workflow.svg   # 4-stage partition lifecycle diagram
    │   └── illustration-comparison.svg # Comparison between USB and internal staging
    ├── css/
    │   └── style.css            # Storage-instrument aesthetic stylesheet (dark/light themes)
    └── js/
        └── main.js              # Theme manager, mobile navigation, and back-to-top
```

## 404 Page Base Path

The custom 404 page (`docs/404.html`) uses root-absolute asset paths prefixed with
`/ulli-next/` (the GitHub Pages project-site base path). This ensures resources
resolve correctly when the 404 page is served from nested nonexistent URLs.
Portability to a domain root or custom domain is not guaranteed without updating
these paths and the `DEPLOY_BASE` constant in `tools/validate-website.py`.

## Catalogue Synchronization

`distros.json` in the repository root is the catalogue used by the ULLI-Next installers. `docs/data/distros.json` is a deployed mirror of that catalogue, kept in sync so the website stays consistent with the installer builds. Never edit `docs/data/distros.json` by hand.

To sync the website catalogue mirror from the root catalogue:
```bash
python3 tools/sync-distros.py
```

To verify in CI or pre-commit checks that the website catalogue mirror is up to date:
```bash
python3 tools/sync-distros.py --check
```

The script exits with code `0` if the mirror matches the root source of truth, or `1` if drift is detected.

## Local Testing and Preview

The site is designed for the GitHub Pages project-site base path `/ulli-next/`. A plain root-level local preview may not reproduce the deployed path structure because the 404 page and header use root-absolute `/ulli-next/...` asset paths.

To preview locally under the correct base path without changing project files:

```bash
# From a temporary directory outside the repository:
mkdir -p /tmp/ulli-preview/ulli-next
cp -r /path/to/ulli-next/docs/* /tmp/ulli-preview/ulli-next/
cd /tmp/ulli-preview
python3 -m http.server 8080

# Open your browser at:
# http://localhost:8080/ulli-next/
```

The site targets the GitHub Pages project URL:
`https://prakhar-khokhar.github.io/ulli-next/`

The 404 page uses root-absolute paths appropriate to that deployment.
The site is not guaranteed to work unchanged on an arbitrary root-domain or custom-domain deployment without updating the base path.

## Features & Implementation Details

- **Zero Frameworks / Pure Vanilla**: Built with clean HTML5, CSS3, and vanilla JavaScript without external npm dependencies or trackers.
- **Storage-Instrument Design**: Styled with a restrained blue-violet brand palette over deep navy and cool neutral surfaces, inspired by the approved ULLI-Next logo and partition-geometry instrumentation.
- **Dark & Light Themes**: Persistent theme switching with zero flash of unstyled theme (FOUT) on load, respecting OS preferences.
- **Safe Markup**: Distribution cards are static HTML with no dynamic data binding, avoiding any XSS surface from catalogue data.
- **Pre-flight & Safety Notices**: Prominently highlights the risks of disk partitioning and boot manipulation, explicitly clarifying that the Qt/C++ rewrite is experimental and not yet ready for production use.
- **Accessibility**: Includes keyboard skip-to-content links, visible focus rings, and `@media (prefers-reduced-motion: reduce)` overrides.
