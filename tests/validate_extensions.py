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

  The download path is HTTP-first (the app's local API) with a copper://
  protocol launch fallback - the "normal" IDM-style integration. The local API
  is token-gated (0.4.0), so the background obtains the install-scoped token
  from the native-messaging host (action "getToken") and both the background and
  the copper.js command-center page attach it as X-Copper-Token.

Usage:
    python tests/validate_extensions.py
"""

import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHROME = os.path.join(REPO, "extensions", "Chrome")
FIREFOX = os.path.join(REPO, "extensions", "Firefox")

MANDATORY_MV3 = {"manifest_version", "name", "version", "description",
                 "permissions", "background", "icons"}


def check(cond, msg):
    if cond:
        print(f"  PASS  {msg}")
        return True
    print(f"  FAIL  {msg}")
    return False


# Locales shipped by the extension. Keep in sync with the app's catalogs and
# with the _locales directories.
LOCALES = ["en", "fr", "ar", "es", "pt_BR"]
MSG_REF = re.compile(r"__MSG_([A-Za-z0-9_@]+)__")
PLACEHOLDER = re.compile(r"\$\d")


def _collect_msg_refs(node, out):
    if isinstance(node, dict):
        for v in node.values():
            _collect_msg_refs(v, out)
    elif isinstance(node, list):
        for v in node:
            _collect_msg_refs(v, out)
    elif isinstance(node, str):
        out.update(MSG_REF.findall(node))


def validate_i18n(label, path, manifest):
    """Check the WebExtension _locales catalogs and their wiring.

    Guards the easy-to-get-wrong pieces: a manifest __MSG_*__ token with no
    catalog entry (the browser then refuses to load the extension), a locale
    missing keys the source has (silent English fallback), and dropped $1
    placeholders (a message that renders with a literal "$1").
    """
    print(f"Validating {label} extension i18n: {path}")
    ok = True

    default_locale = manifest.get("default_locale")
    ok &= check(default_locale == "en", "default_locale is 'en'")

    locales_dir = os.path.join(path, "_locales")
    ok &= check(os.path.isdir(locales_dir), "_locales directory exists")

    catalogs = {}
    for loc in LOCALES:
        loc_path = os.path.join(locales_dir, loc, "messages.json")
        if not os.path.isfile(loc_path):
            ok &= check(False, f"catalog exists: _locales/{loc}/messages.json")
            continue
        try:
            with open(loc_path, encoding="utf-8") as f:
                catalogs[loc] = json.load(f)
            ok &= check(True, f"catalog parses: {loc}")
        except (OSError, json.JSONDecodeError) as e:
            ok &= check(False, f"catalog parses: {loc} ({e})")

    base = catalogs.get("en") or {}
    ok &= check(bool(base), "source catalog (en) is non-empty")

    if base:
        for loc, cat in catalogs.items():
            if loc == "en":
                continue
            missing = sorted(set(base) - set(cat))
            extra = sorted(set(cat) - set(base))
            ok &= check(not missing and not extra,
                        f"{loc}: key parity with en "
                        f"(missing={missing}, extra={extra})")
            bad = [k for k in base if k in cat
                   and sorted(PLACEHOLDER.findall(base[k].get("message", "")))
                   != sorted(PLACEHOLDER.findall(cat[k].get("message", "")))]
            ok &= check(not bad, f"{loc}: $1 placeholder parity (drift in {sorted(bad)})")
            empty = sorted(k for k, v in cat.items() if not v.get("message"))
            ok &= check(not empty, f"{loc}: no empty messages ({empty})")

    refs = set()
    _collect_msg_refs(manifest, refs)
    ok &= check(bool(refs), "manifest references catalog messages (__MSG_*__)")
    unresolved = sorted(r for r in refs if r not in base)
    ok &= check(not unresolved,
                f"every __MSG_*__ reference resolves in en ({unresolved})")

    # The status page and background must actually consult the i18n API, and the
    # page must carry the data-i18n hooks the localizer rewrites.
    for rel in ("copper.js", "background.js"):
        src_path = os.path.join(path, rel)
        if os.path.isfile(src_path):
            with open(src_path, encoding="utf-8") as f:
                src = f.read()
            ok &= check("i18n.getMessage" in src, f"{rel} reads localized messages")
    html_path = os.path.join(path, "copper.html")
    if os.path.isfile(html_path):
        with open(html_path, encoding="utf-8") as f:
            html = f.read()
        ok &= check("data-i18n" in html, "copper.html carries data-i18n hooks")

    return ok


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

    # Reference checks for the shipped design: HTTP transport with the API
    # token, where the token (and install/status queries) come from the app
    # via the native messaging host.
    perms = manifest.get("permissions") or []
    ok &= check("nativeMessaging" in perms,
                "nativeMessaging permission declared (token/status via the host)")
    ok &= check("tabs" in perms, "tabs permission declared (popup 'send current page')")
    ok &= check("downloads" in perms,
                "downloads permission declared (auto-capture normal browser downloads)")
    host_perms = manifest.get("host_permissions") or []
    ok &= check(any("127.0.0.1:24680" in str(h) for h in host_perms),
                "host_permissions grants the app's local HTTP API")

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

    # The background must fetch the API token from the app through the native
    # messaging host (action "getToken"), then route downloads through the
    # app's local HTTP API (http://127.0.0.1:24680/api/download) - the normal
    # integration path - and launch/raise the app via the registered copper://
    # protocol when it is not running.
    bg_path = js_refs[0] if js_refs else None
    if bg_path:
        with open(os.path.join(path, bg_path), encoding="utf-8") as f:
            bg_src = f.read()
        ok &= check("api/download" in bg_src,
                    "background posts downloads to /api/download (local HTTP API)")
        ok &= check("http://127.0.0.1:24680" in bg_src,
                    "background targets the app's local HTTP API")
        ok &= check("copper://open" in bg_src,
                    "background launches the app via the copper:// protocol")
        ok &= check("sendNativeMessage" in bg_src,
                    "background fetches the API token via the native messaging host")
        ok &= check("X-Copper-Token" in bg_src,
                    "background attaches the API token header (X-Copper-Token)")
        ok &= check("getToken" in bg_src,
                    "background exposes a getToken action for the status page")
        ok &= check("/api/register" in bg_src,
                    "background self-registers with /api/register when the host "
                    "cannot supply a token (0.4.3)")
        ok &= check("sendNativeMessage" in bg_src and "registerWithApp" in bg_src,
                    "background falls back from native getToken to app registration")
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
        # Context-menu builds must be serialized: two overlapping removeAll+create
        # sequences make the second create() fail with "Cannot create item with
        # duplicate id copper-link" (the MV3 service worker restarts and both the
        # boot path and onInstalled try to build the menus).
        ok &= check("menusBuildChain" in bg_src and "MENU_DEFS" in bg_src,
                    "context-menu builds are serialized through one promise chain "
                    "(no duplicate-id race)")
        ok &= check("runtime.lastError" in bg_src,
                    "context-menu create callbacks read runtime.lastError "
                    "(no unchecked runtime.lastError)")

    # The command-center page (copper.js) runs its own fetches against the local
    # API, so it must carry the token too - otherwise its download list is always
    # "not reachable" while the rest of the extension works.
    copper_path = os.path.join(path, "copper.js")
    if os.path.isfile(copper_path):
        with open(copper_path, encoding="utf-8") as f:
            copper_src = f.read()
        ok &= check("X-Copper-Token" in copper_src,
                    "status page sends the API token (X-Copper-Token)")

    ok &= validate_i18n(label, path, manifest)

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
