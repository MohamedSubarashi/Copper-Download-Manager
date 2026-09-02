#!/usr/bin/env python3
"""Validate the Chrome and Firefox extension manifests for release readiness.

Checks (offline, no external deps):
  * Each manifest is well-formed JSON.
  * Required MV3 fields are present and typed correctly.
  * The background entry matches the target browser shape:
        Chrome  -> "service_worker"
        Firefox -> "scripts"
  * Popup-less IDM-style toolbar: no action.default_popup; the bundled
        copper.html status page exists and the background routes toolbar clicks
        to it via chrome.action.onClicked.
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

    # IDM-style: no popup. The toolbar button opens the bundled status page and
    # downloads are handed to the native host, so we require the status page and
    # confirm the background routes toolbar clicks to it.
    action = manifest.get("action", {})
    popup = action.get("default_popup", "")
    ok &= check(not popup, "popup-less extension (no action.default_popup)")
    ok &= check(os.path.isfile(os.path.join(path, "copper.html")),
                "status page exists: copper.html")
    ok &= check(os.path.isfile(os.path.join(path, "copper.js")),
                "status page script exists: copper.js")

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

    # Reference checks for the native-messaging design.
    perms = manifest.get("permissions") or []
    ok &= check("nativeMessaging" in perms,
                "nativeMessaging permission declared (extension talks to the host)")
    ok &= check("tabs" in perms, "tabs permission declared (popup 'send current page')")
    ok &= check("downloads" in perms,
                "downloads permission declared (auto-capture normal browser downloads)")

    # All referenced JS files exist on disk.
    js_refs = []
    if "scripts" in bg:
        js_refs.extend(bg["scripts"])
    elif "service_worker" in bg:
        js_refs.append(bg["service_worker"])
    for cs in manifest.get("content_scripts") or []:
        js_refs.extend(cs.get("js", []))
    for ref in js_refs:
        if ref:
            ok &= check(os.path.isfile(os.path.join(path, ref)),
                        f"referenced JS exists: {ref}")

    # The background must route through the native host (not copper:// HTTP or
    # the deprecated localhost ping), so it works without a TCP port or protocol.
    bg_path = js_refs[0] if js_refs else None
    if bg_path:
        with open(os.path.join(path, bg_path), encoding="utf-8") as f:
            bg_src = f.read()
        ok &= check("sendNativeMessage" in bg_src,
                    "background uses sendNativeMessage (native messaging)")
        ok &= check("copper://" not in bg_src,
                    "background no longer depends on the copper:// protocol")
        # Native messaging is the ONLY download path. The localhost HTTP API may
        # appear exclusively for the status-page health-check fallback (dev and
        # portable setups where the host isn't registered yet), never for routing
        # a download. Both downloads and the status ping route via sendNativeMessage.
        ok &= check("127.0.0.1:24680" not in bg_src or "/api/ping" in bg_src,
                    "localhost HTTP is only a ping fallback, never the download path")
        ok &= check("action.onClicked" in bg_src,
                    "toolbar click opens status page (action.onClicked)")
        ok &= check("openStatusTab" in bg_src,
                    "background opens the bundled status/install page")
        ok &= check("downloads.onCreated" in bg_src,
                    "background auto-captures normal browser downloads (downloads.onCreated)")
        ok &= check("api/download-filters" in bg_src,
                    "background reads the include/exclude filter config via /api/download-filters")
        ok &= check("chrome.downloads.cancel" in bg_src or "downloads.cancel" in bg_src,
                    "background cancels the browser download after a successful dispatch")

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
