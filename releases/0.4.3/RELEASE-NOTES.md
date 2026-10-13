# Copper Download Manager 0.4.3

Patch release. Fixes browser-injected downloads **still** failing with
**"unauthorized"** after 0.4.2, by fixing the real Windows root cause, and adds a
one-time in-app authorization for sideloaded/unpacked extensions. No features
were removed and the interface is unchanged.

## Why 0.4.2 did not fix it

Since 0.4.0 the app's local HTTP API is token-gated: the extension has to read
the install-scoped token from the app over **native messaging** and send it as
`X-Copper-Token`. 0.4.2 corrected the *contents* of the host manifest (real store
IDs, refreshed host path) — but on Windows that is not where a browser looks.

Chrome, Edge and Firefox resolve the native-messaging host through the
**Windows registry**
(`HKCU\Software\Google\Chrome|Microsoft\Edge|Mozilla\NativeMessagingHosts\com.copper.dm`),
whose default value is the path to the manifest. The JSON files the app wrote
under a browser's `User Data\NativeMessagingHosts\` directory are a macOS/Linux
convention and are ignored on Windows.

On the machine where this was reported the registry keys were broken:

- Chrome: `(value not set)` — no host at all;
- Firefox: `(value not set)`;
- Edge: pointed at a manifest path that did not exist.

So the browser reported *"Specified native messaging host not found"*, the host
never ran, the extension never obtained a token, and every browser-injected
download hit the API tokenless and came back `401 Unauthorized`.

A second, independent problem: an **unpacked/dev build** has a path-derived
extension ID (for example `jbdpbkclajmfbdlonmmgjdgpikabhdoh`) that is none of the
published store IDs, so even correct registration would never list it in
`allowed_origins`.

## Fixes

- **Windows registry registration.** `installManifest()` now also writes the HKCU
  host key (Chrome, Edge, Firefox) with the manifest path as its default value,
  and keeps the manifest in a stable per-user location —
  `%LOCALAPPDATA%\Copper\Copper Download Manager\NativeMessagingHosts\<browser>\com.copper.dm.json`
  — outside any browser profile, so it survives profile moves and multi-profile
  setups.
- **One-time self-registration for unknown extensions.** New tokenless,
  extension-origin-gated `POST /api/register`. If the extension's ID is already
  authorized it returns the API token immediately, with no prompt. If it is
  unknown, the app shows a **one-time confirmation dialog naming the extension
  ID**; on approval it persists the ID to the host manifest and returns the
  token, so a sideloaded build works with no browser restart and no manual
  command. A denial is not repeatedly nagged.
- **Browser extension (0.1.2).** When the native host cannot hand over a token,
  the background calls `/api/register` (with a long timeout to allow the
  confirmation prompt) and caches the returned token. Applied to both the Chrome
  and Firefox builds.
- **Installer cleanup.** The uninstaller now removes the HKCU host keys and the
  per-user host manifest/config, so a later reinstall cannot inherit a stale host
  entry.

## Verification

- `copper_tests`: **25 passed, 0 failed** (2 new cases: `isExtensionAllowed`, and
  a Windows test asserting the HKCU host key is written and points at an existing
  manifest).
- Integration suite: **117 passed, 0 failed** (new checks: each browser's
  registry key points at its manifest, and `POST /api/register` hands the token
  to an authorized extension origin while refusing a website / missing Origin).
- Live (non-test) on a real Windows machine: the headless
  `--register-native-extension` path wrote
  `HKCU\Software\Google\Chrome\NativeMessagingHosts\com.copper.dm` →
  `…\NativeMessagingHosts\chrome\com.copper.dm.json` and that manifest lists the
  unpacked dev ID; Edge and Firefox keys were written the same way.
- `tests/validate_extensions.py`: Chrome OK, Firefox OK; both zips rebuilt at
  0.1.2.
- `tools/check_version_consistency.py`: version `0.4.3` consistent everywhere.

## Install

- **Installer**: `CopperDownloadManager-0.4.3-setup.exe`
- **Portable**: extract `CopperDownloadManager-portable-0.4.3.zip`
  anywhere and run `CopperDownloadManager.exe`
- Verify downloads with `SHA256SUMS` (`sha256sum -c SHA256SUMS`).

## Known limitations

- Binaries are not code-signed (no signing certificate available in this
  cycle); Windows SmartScreen may warn on first run.
- The browser add-ons already published in the Chrome Web Store / Edge Add-ons /
  AMO are older than this tree. The **app-side** fix above is what restores
  injection with those shipped builds. The updated **0.1.2** extension (the
  `/api/register` fallback) only takes effect after it is republished to the
  stores; that is a separate manual step.
- After the very first registration of a brand-new extension ID, Chrome may not
  re-read the host manifest until it restarts. The `/api/register` response
  returns the token immediately, so the download works regardless; a restart only
  matters for the native-messaging `getToken` path.
