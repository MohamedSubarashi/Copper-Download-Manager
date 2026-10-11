# Copper Download Manager 0.4.2

Patch release. Fixes browser-injected downloads failing with **"unauthorized"**
and restores the installer's all-users / just-me install-mode choice, plus the
related browser-extension fixes found while root-causing it. No features were
removed and the interface is unchanged.

## What was wrong

Downloads sent from the browser extension to the app came back
`401 Unauthorized`, so nothing was captured.

Since 0.4.0 the app's local HTTP API is token-gated: the extension has to read
the install-scoped token from the app over **native messaging**, then send it as
`X-Copper-Token`. That handshake is only allowed if the browser can find the
app's **native-messaging host manifest** *and* the manifest lists the
extension's real ID in `allowed_origins` / `allowed_extensions`.

On a real install:

- the Chrome manifest listed only a placeholder ID
  (`abcdefghijklmnopabcdefghijklmnop`) and pointed at a stale
  `...\release\0.3.5\copper_native_host.exe`;
- the Edge manifest did not exist;
- the Firefox manifest listed the wrong gecko ID
  (`copper-download-manager@copper`) instead of the add-on's AMO GUID
  (`CopperDownloadManager@mohamedsubarashi`).

So the extension could never obtain the token, and its tokenless fallback to
`/api/download` returned 401.

## Fixes

- **The app now registers every browser's host manifest on startup** with the
  stable store IDs: Chrome Web Store `cmokehdedogdpbfhlamopeifgogaihni`,
  Edge Add-ons `fnganciafgifhelimkoeiiapdcdmnnlb`, Firefox AMO
  `CopperDownloadManager@mohamedsubarashi`. Registration **merges** with any
  ID already present (a sideloaded dev build is never clobbered) and validates
  Chromium IDs (`^[a-p]{32}$`) so a junk entry cannot disable the handshake.
- **The host path is rewritten on every run**, repairing a manifest left
  pointing at an older install (e.g. a removed 0.3.5 folder).
- **The Firefox gecko ID was wrong** and is corrected to the add-on's real
  AMO GUID.
- **Installer restores the install-mode page.** `PrivilegesRequired=lowest`
  plus `PrivilegesRequiredOverridesAllowed=dialog` make the wizard always offer
  "Install for all users" (elevates, Program Files) or "Install for me only"
  (per-user, no admin). These directives had been dropped when the script was
  recreated.
- **Browser extension (0.1.1).** The command-center / options page now
  authenticates its own local-API calls with `X-Copper-Token` (it used to fetch
  them tokenless and always showed "not reachable"), and the background no
  longer caches an empty token: it retries once the just-launched app has
  written its config. Both extension zips were rebuilt.

## Verification

- Live end-to-end on a Windows machine (10/10): the repaired manifest lets the
  host hand out a token that matches the API's; a tokenless request gets 401 and
  a token-carrying request gets 200; a download injected through the **native
  host** (the path the shipped store extension uses) and one injected over the
  **authenticated HTTP API** (the path the updated extension uses) both complete
  byte-exact.
- `copper_tests`: **23 passed, 0 failed** (2 new cases lock the stable store IDs
  and the merge/validate behavior of the manifest writer).
- Integration suite: **104 passed, 0 failed** (5 new checks assert the
  Chrome/Edge/Firefox host manifests are written with the store IDs and a
  correct host path).
- `tools/check_version_consistency.py`: version `0.4.2` consistent everywhere.

## Install

- **Installer**: `CopperDownloadManager-0.4.2-setup.exe`
- **Portable**: extract `CopperDownloadManager-portable-0.4.2.zip`
  anywhere and run `CopperDownloadManager.exe`
- Verify downloads with `SHA256SUMS` (`sha256sum -c SHA256SUMS`).

## Known limitations

- Binaries are not code-signed (no signing certificate available in this
  cycle); Windows SmartScreen may warn on first run.
- The browser add-ons already published in the Chrome Web Store / Edge Add-ons /
  AMO are older than this tree. The **app-side** fix above is what restores
  download injection with those shipped builds (their native path needs no
  token). The **browser-side** status page and token-authenticated options calls
  require the updated 0.1.1 extension; republishing it to the stores is a
  separate step.
