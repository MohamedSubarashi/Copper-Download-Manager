# Implementation Report — Copper Download Manager 0.4.3

Patch release on top of 0.4.2. This is **not** a rewrite and not a feature
release: the architecture, interfaces and feature set are preserved. The change
fixes browser-injected downloads still failing with `401 Unauthorized` on
Windows by addressing the real root cause (0.4.2 corrected manifest *contents*
but not how Windows *locates* the host), and adds a one-time in-app
authorization so a sideloaded/unpacked extension can authenticate.

Statuses use the required vocabulary: **PASS / FAIL / NOT RUN / BLOCKED**.
"PASS" means the behavior was executed and asserted by a check listed under
*Evidence*; anything not executed is reported as NOT RUN, never inferred.

---

## 1. Root cause

Since 0.4.0 the app's local HTTP API is token-gated. The browser extension must
read the install-scoped token from the app over **native messaging** (the
`getToken` action, served from `copper_host_config.json`) and send it as
`X-Copper-Token` on `/api/download`. That handshake requires the browser to find
the app's native-messaging host manifest and for the manifest to list the
extension's real ID.

**D1 — the manifest was written where Windows browsers do not look.** On Windows,
Chrome/Edge/Firefox resolve a native-messaging host through the registry:

```
HKCU\Software\Google\Chrome\NativeMessagingHosts\com.copper.dm
HKCU\Software\Microsoft\Edge\NativeMessagingHosts\com.copper.dm
HKCU\Software\Mozilla\NativeMessagingHosts\com.copper.dm
```

The key's **default value** is the path to the host manifest. The app wrote the
manifest JSON under a browser's `User Data\NativeMessagingHosts\` directory (a
macOS/Linux convention) and never touched the registry. Observed on the reporting
machine: Chrome and Firefox keys were `(value not set)`; Edge pointed at a file
that did not exist. Result: *"Specified native messaging host not found"*, no
token, `401` on every injected download. The app log confirmed 454 tokenless
rejections and 15 rejected `POST /api/download` from origin
`chrome-extension://jbdpbkclajmfbdlonmmgjdgpikabhdoh`.

**D2 — the running extension's ID was not authorized.** The user runs an
**unpacked** Chrome build (`location=4`, `from_webstore=False`) whose ID is
derived from the install path (`jbdpbkclajmfbdlonmmgjdgpikabhdoh`). It is none of
the published store IDs, so it would never appear in `allowed_origins` even with
correct registration.

0.4.2 addressed the manifest *contents* (store IDs, host path) but not D1, so the
401 persisted.

## 2. What changed

### 2.1 Native-messaging registration (`src/utils/NativeMessaging.cpp`, `.h`)

- New stable per-user manifest location, outside any browser profile:
  `%LOCALAPPDATA%\Copper\Copper Download Manager\NativeMessagingHosts\<browser>\com.copper.dm.json`
  (`windowsManifestDir()`, `manifestTargetPath()`).
- `installManifest()` now writes that manifest **and** the HKCU host key default
  value (native separators) via `RegCreateKeyExW`/`RegSetValueExW`
  (`writeRegistryDefaultString()`, `registryHostKeys()`).
- Under `QStandardPaths::isTestModeEnabled()` the key prefix is redirected to
  `Software\Copper\TestNativeMessagingHosts\`, so tests never re-point the
  developer's real browsers.
- New `isExtensionAllowed(browser, id)` used by the local server to tell a known
  ID from an unknown one.

### 2.2 Self-registration (`src/core/LocalServer.cpp`, `.h`, `src/ui/MainWindow.cpp`)

- New tokenless `POST /api/register`, reachable only from an allowed extension
  origin (`parseExtensionOrigin()` accepts `chrome-extension://<32×[a-p]>` and
  `moz-extension://<uuid>`). It runs before the global token check because the
  caller has no token yet.
- If the ID is already authorized → returns the API token immediately, no prompt.
- If unknown → calls a `RegistrationConfirm` callback; on approval persists the
  ID (and, for a Chromium origin, both the chrome and edge manifests) and returns
  the token; on denial returns `403`.
- `MainWindow` installs the callback as a `QMessageBox::question` naming the
  extension ID (Yes/No, default No).

### 2.3 Browser extension (0.1.3) (`extensions/Chrome`, `extensions/Firefox`)

- Background: when the native `getToken` fails, `registerWithApp()` calls
  `/api/register` with a 60 s timeout (the user has to click) and caches the
  token; a denial is remembered for 60 s in memory (`registrationDeniedUntil`).
- `httpApiOnce()` gained a timeout parameter. Chrome and Firefox backgrounds kept
  byte-identical. Both manifests bumped `0.1.1 → 0.1.2`.
- **Follow-up (0.1.3): the context-menu build is serialized.** The MV3 service
  worker restarts constantly and menus are rebuilt from both the boot path and
  `onInstalled`; each build was `removeAll`-then-`create`, so two overlapping
  builds made the second `create()` fail with *"Cannot create item with duplicate
  id copper-link"* (surfaced as an unchecked `runtime.lastError`). `buildMenus()`
  now runs through a single promise chain (`menusBuildChain`) over a `MENU_DEFS`
  list, and every `create`/`removeAll` callback reads `chrome.runtime.lastError`,
  so neither the duplicate nor the unchecked-error warning can occur. The same
  serialization covers the disable path (`rebuildMenus(false)`).

### 2.4 Installer (`installer/CopperDownloadManager.iss`)

- `[Registry]` removes the three HKCU host keys on uninstall
  (`uninsdeletekey`); `[UninstallDelete]` removes the per-user manifest dir and
  host config. Version → `0.4.3`.

### 2.5 Version single-source

`CMakeLists.txt` 0.4.3, mirrored into `app.rc`,
`installer/CopperDownloadManager.iss`, `THIRD-PARTY-NOTICES.txt`,
`installer/arch/PKGBUILD`, `.github/workflows/ci.yml`; `0.4.2` added to
`PREVIOUS_RELEASES` and the two new docs added to `HISTORY_DOCS` in
`tools/check_version_consistency.py`.

## 3. Evidence

### 3.1 Build

| Check | Status | Evidence |
| --- | --- | --- |
| Windows Release build + `windeployqt` to `installer/release/0.4.3/` | **PASS** | Qt 6.12.0 / MinGW 13.1; `CopperDownloadManager.exe`, `copper_native_host.exe`, Qt DLLs + plugins, `THIRD-PARTY-NOTICES.txt` |
| exe reports version `0.4.3` | **PASS** | `Get-Item … | % VersionInfo` → FileVersion/ProductVersion `0.4.3` |

### 3.2 Tests

| Check | Status | Evidence |
| --- | --- | --- |
| Unit suite | **PASS** | `copper_tests` → `25 passed, 0 failed` (incl. new `nativeMessaging_isExtensionAllowed`, `nativeMessaging_registryRegistration`) |
| Integration suite | **PASS** | `117 passed, 0 failed`, process exit code `0` |
| New integration: HKCU host key points at its manifest (chrome/edge/firefox) | **PASS** | 9 new checks under the `Software\Copper\TestNativeMessagingHosts\` prefix |
| New integration: `POST /api/register` semantics | **PASS** | 3 checks: authorized origin → 200 + token; website origin → 403; no Origin → 403 |
| Extension layout/behavior validation | **PASS** | `tests/validate_extensions.py` → Chrome OK, Firefox OK (incl. new `/api/register` fallback assertion) |
| Version consistency | **PASS** | `tools/check_version_consistency.py` → "VERSION CHECK PASSED (version 0.4.3 consistent everywhere)" |

### 3.3 Live (non-test) on a real Windows machine

| Check | Status | Evidence |
| --- | --- | --- |
| Headless registration writes the HKCU key + manifest | **PASS** | `--register-native-extension chrome jbdpbkclajmfbdlonmmgjdgpikabhdoh` → log "registered host for chrome at `HKCU\Software\Google\Chrome\NativeMessagingHosts\com.copper.dm`"; `reg query … /ve` now shows the manifest path |
| Manifest lists the unpacked dev ID | **PASS** | `allowed_origins` = dev ID + Edge store ID + Chrome Web Store ID |
| Edge + Firefox registration | **PASS** | both HKCU keys written and pointing at existing manifests |

### 3.4 Packaging

The **published** assets are the ones CI built from the tag (the same flow as
0.4.2); the local pre-tag build is not byte-reproducible because a zip carries
file mtimes and the CI Inno Setup version differs, so `releases/0.4.3/` was
reconciled to the published artifacts.

| Check | Status | Evidence |
| --- | --- | --- |
| Windows installer built | **PASS** | `releases/0.4.3/CopperDownloadManager-0.4.3-setup.exe` (published artifact, `sha256 43899580…`) |
| Windows portable zip built | **PASS** | `releases/0.4.3/CopperDownloadManager-portable-0.4.3.zip` (published artifact, `sha256 69977818…`) |
| SHA256SUMS describes the shipped assets | **PASS** | `releases/0.4.3/SHA256SUMS`; both hashes re-computed from freshly downloaded release assets and matched the GitHub `digest` field |
| Extension zips rebuilt | **PASS** | `extensions/*/Copper Download Manager *.zip` at 0.1.3 (root layout, forward-slash `icons/`); `validate_extensions.py` fails if the menu build is not serialized |

### 3.5 Not executed (reported honestly)

| Check | Status | Why |
| --- | --- | --- |
| Interactive installer install **and uninstall** (registry cleanup) | **NOT RUN** | Needs UAC / a disposable VM; the `[Registry]`/`[UninstallDelete]` entries compile but the removal was not exercised end-to-end |
| Real browser E2E (drive Chrome with the unpacked extension) | **NOT RUN** | Not automatable headlessly here; the native-host → pipe → token path is covered by the integration suite, the registry/manifest path by the live check above |
| Republished store-extension smoke | **NOT RUN** | Republishing the 0.1.3 add-ons to the stores is a separate manual step |
| Opt-in real-playlist suite (`COPPER_REAL_PLAYLIST=1`) | **NOT RUN** | Optional; requires yt-dlp/ffmpeg and network |
| Code signing | **BLOCKED** | No certificate available this cycle |

## 4. Honest limitations

- The live registry fixes were applied on the reporting machine; the app-side fix
  is what restores injection for **already-published** store extensions.
- Chrome caches the host manifest; the very first registration of a new ID may
  require a browser restart for the `getToken` path, but `/api/register` returns
  the token immediately so downloads still work.
- No interactive installer/uninstall run, no real-browser automation, no code
  signing (see 3.5).

## 5. Release

| Item | Status | Evidence |
| --- | --- | --- |
| Release commit + tag | **PASS** | `1a88e27` on `main`, annotated tag `v0.4.3` |
| CI green on `main` | **PASS** | run `38372062906` → `success` |
| CI green on `v0.4.3` (incl. Publish) | **PASS** | run `38372066643` → all jobs success; "Publish GitHub release" ok |
| GitHub release published | **PASS** | https://github.com/MohamedSubarashi/Copper-Download-Manager/releases/tag/v0.4.3 (6 assets) |
| Curated release notes applied | **PASS** | `gh release edit v0.4.3 --notes-file releases/0.4.3/RELEASE-NOTES.md` |

Published assets:

```
copper-download-manager-0.4.3-1-x86_64.pkg.tar.zst             5dff5753…   (Arch)
copper-download-manager-0.4.3-Linux.deb                        d1ac979f…   (Debian/Ubuntu)
copper-download-manager-debug-0.4.3-1-x86_64.pkg.tar.zst       80325fca…   (Arch debug)
CopperDownloadManager-0.4.3-setup.exe                          43899580…   (Windows)
CopperDownloadManager-portable-0.4.3.zip                       69977818…   (Windows)
CopperDownloadManager.dmg                                      175abb13…   (macOS)
```

## 6. Supersedes 0.4.2

0.4.2's approach (adding store IDs to the JSON manifest under `User Data\`) was
ineffective on Windows because the browser never reads that path. 0.4.3 keeps
those IDs **and** registers them where Windows actually looks.
