#!/usr/bin/env python3
"""Build the shippable Chrome and Firefox extension zips.

The zips are tracked in the repository (force-added past the *.zip gitignore
rule) so a release can attach them directly. The layout matches what the
browsers expect and what previous releases shipped:

  * entries sit at the zip root (manifest.json first);
  * paths use forward slashes;
  * the _locales/<lang>/messages.json catalogs are included so the localized
    UI (fr, ar, es, pt_BR) ships with the extension.

Entries are written with a fixed timestamp and sorted order, so rebuilding an
unchanged extension produces an identical archive.

Usage:
    python tools/build_extension_zips.py
"""

import os
import sys
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TARGETS = [
    ("Chrome", os.path.join(REPO, "extensions", "Chrome"),
     "Copper Download Manager Chrome.zip"),
    ("Firefox", os.path.join(REPO, "extensions", "Firefox"),
     "Copper Download Manager Firefox.zip"),
]

# Top-level files in the order browsers like to see them.
TOP_ORDER = ["manifest.json", "background.js", "copper-bar.js", "copper.css",
             "copper.html", "copper.js"]

# Fixed timestamp keeps rebuilds byte-stable.
FIXED_TIME = (2020, 1, 1, 0, 0, 0)


def iter_members(root):
    """Archive names (forward slashes) in a stable, browser-friendly order."""
    names = []
    for top in TOP_ORDER:
        if os.path.isfile(os.path.join(root, top)):
            names.append(top)
    icons = os.path.join(root, "icons")
    if os.path.isdir(icons):
        for n in sorted(os.listdir(icons)):
            if os.path.isfile(os.path.join(icons, n)):
                names.append("icons/" + n)
    locales = os.path.join(root, "_locales")
    if os.path.isdir(locales):
        for loc in sorted(os.listdir(locales)):
            messages = os.path.join(locales, loc, "messages.json")
            if os.path.isfile(messages):
                names.append("_locales/%s/messages.json" % loc)
    return names


def build(label, root, zip_name):
    members = iter_members(root)
    if "manifest.json" not in members:
        print("FAIL: %s has no manifest.json" % root)
        return False
    out = os.path.join(root, zip_name)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name in members:
            info = zipfile.ZipInfo(name, date_time=FIXED_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(os.path.join(root, *name.split("/")), "rb") as f:
                z.writestr(info, f.read())
    print("built %s (%d entries)" % (out, len(members)))
    for name in members:
        print("    " + name)
    return True


def main():
    ok = True
    for label, root, zip_name in TARGETS:
        ok &= build(label, root, zip_name)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
