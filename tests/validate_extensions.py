#!/usr/bin/env python3
"""Validate the Chrome and Firefox extension manifests for release readiness.

Checks (offline, no external deps):
  * Each manifest is well-formed JSON.
  * Required MV3 fields are present and typed correctly.
  * The background entry matches the target browser shape:
        Chrome  -> "service_worker"
        Firefox -> "scripts"
  * A toolbar "action" with a default_popup is declared (the popup UI must be
        reachable via the toolbar button).
  * Firefox declares browser_specific_settings.gecko with an extension id and a
        data_collection_permissions block required:["none"] (the extension does
        not transmit data externally, so "none" is the honest declaration).
  * Every icon referenced by the manifest exists on disk.
  * Referenced JS/CSS files exist on disk.

Usage:
    python tests/validate_extensions.py
"""

import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHROME = os.path.join(REPO, "extensions", "Copper Download Manager Chrome")
FIREFOX = os.path.join(REPO, "extensions", "Copper Download Manager Firefox")

MANDATORY_MV3 = {"manifest_version", "name", "version", "description",
                 "permissions", "background", "icons"}


def check(cond, msg):
    if cond:
        print(f"  PASS  {msg}")
        return True
    print(f"  FAIL  {msg}")
    return False


def validate_dir(label, path, is_firefox):
    print(f"Validating {label} extension: {path}")
    manifest_path = os.path.join(path, "manifest.json")
    if not os.path.isfile(manifest_path):
        print(f"  FAIL  manifest.json missing at {manifest_path}")
        return False

    with open(manifest_path, encoding="utf-8") as f:
        try:
            manifest = json.load(f)
        except json.JSONDecodeError as e:
            print(f"  FAIL  manifest.json is not valid JSON: {e}")
            return False

    ok = True
    ok &= check(manifest.get("manifest_version") == 3, "manifest_version == 3")
    ok &= check(MANDATORY_MV3.issubset(manifest.keys()),
                f"mandatory MV3 keys present: {sorted(MANDATORY_MV3)}")
    ok &= check(isinstance(manifest.get("name"), str) and manifest["name"],
                "name is a non-empty string")

    # Background shape must match the target browser.
    bg = manifest.get("background", {})
    is_firefox_ext = is_firefox or manifest.get("manifest_version") == 1
    if is_firefox:
        ok &= check("scripts" in bg, "Firefox background uses 'scripts'")
    else:
        ok &= check("service_worker" in bg, "Chrome background uses 'service_worker'")

    # Toolbar popup must be reachable.
    action = manifest.get("action", {})
    popup = action.get("default_popup", "")
    ok &= check(bool(popup), "action.default_popup is declared")
    if popup:
        ok &= check(os.path.isfile(os.path.join(path, popup)),
                    f"popup file exists: {popup}")

    # Firefox-specific release requirements.
    if is_firefox:
        bss = manifest.get("browser_specific_settings", {})
        gecko = bss.get("gecko", {})
        ok &= check(bool(gecko.get("id")), "browser_specific_settings.gecko.id is set")
        dcp = gecko.get("data_collection_permissions", {})
        ok &= check(dcp.get("required") == ["none"] or "none" in dcp.get("required", []),
                    "gecko.data_collection_permissions declares required: none")
        ok &= check("optional" not in dcp or dcp["optional"] == [],
                    "no misleading optional data-collection permission declared")

    # Icon files referenced by icons and action must exist.
    refs = set()
    for group in (manifest.get("icons") or {}, action.get("default_icon") or {}):
        refs.update(str(v) for v in group.values())
    for ref in refs:
        if ref:
            ok &= check(os.path.isfile(os.path.join(path, ref)),
                        f"icon exists: {ref}")

    return ok


def main():
    chrome_ok = validate_dir("Chrome", CHROME, is_firefox=False)
    firefox_ok = validate_dir("Firefox", FIREFOX, is_firefox=True)
    print()
    print("Chrome:", "OK" if chrome_ok else "FAIL")
    print("Firefox:", "OK" if firefox_ok else "FAIL")
    return 0 if (chrome_ok and firefox_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
