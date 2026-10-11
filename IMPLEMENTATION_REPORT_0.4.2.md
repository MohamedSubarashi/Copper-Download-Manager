# Implementation Report — Copper Download Manager 0.4.2

Patch release on top of 0.4.1. This is **not** a rewrite and not a feature
release: the architecture, interfaces and feature set are preserved. The change
fixes browser-injected downloads failing with `401 Unauthorized`, restores the
installer's install-mode choice, and carries the related browser-extension fixes
found while root-causing the 401.

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
extension's real ID in `allowed_origins` (Chrome/Edge) / `allowed_extensions`
(Firefox).

Machine evidence, before the fix:

- Chrome
  (`%LOCALAPPDATA%\Google\Chrome\User Data\NativeMessagingHosts\com.copper.dm.json`)
  listed only `chrome-extension://abcdefghijklmnopabcdefghijklmnop/` (a
  placeholder — no such extension exists) and its `path` pointed at a stale
  `…\installer\release\0.3.5\copper_native_host.exe`.
- The Edge manifest did not exist.
- The Firefox manifest listed `copper-download-manager@copper`, but the add-on's
  real AMO GUID is `CopperDownloadManager@mohamedsubarashi`
  (`addons.mozilla.org/api/v5/addons/addon/copper-download-manager` → `guid`),
  and its `path` pointed at a non-existent `C:\Program Files\…` host.
- `%LOCALAPPDATA%\Copper\copper_host_config.json` (which holds the token) did
  not exist.

Because the extension's real ID was not authorized, `getToken` was rejected and
the extension's tokenless fallback to `/api/download` returned 401 — the
reported "unauthorized".

The code defects:

- `NativeMessaging::defaultChromeExtensionId()` returned an empty string, so the
  startup registration only ever wrote the Firefox manifest (with the wrong
  gecko ID).
- `NativeMessaging::installManifest()` **overwrote** the file with only the IDs
  passed in, so registering one ID dropped another, and it never refreshed the
  `path` of an existing manifest after an install moved.
- `--register-native-extension` / the `register` handshake was the only way a
  Chrome/Edge ID ever got written, and the shipped store build never called it.

Separately, the installer had lost its install-mode choice: `git` history shows
`PrivilegesRequired=lowest` + `PrivilegesRequiredOverridesAllowed=dialog`
existed, were removed when the `.iss` was recreated, and the recreated file did
not bring them back — so the wizard no longer offered all-users vs just-me.

---

## 2. Changes

| Area | Change |
|---|---|
| `include/utils/NativeMessaging.h` | Added `edgeExtensionId()`; documented the merge semantics and the stable-ID contract. |
| `src/utils/NativeMessaging.cpp` | `defaultChromeExtensionId()` → the Chrome Web Store ID; added `edgeExtensionId()`; corrected `firefoxExtensionId()` to the AMO GUID; `installManifest()` now **merges** existing allowed IDs (dedup) with the passed ones, validates Chromium IDs (`^[a-p]{32}$`), rewrites `path` to the current exe, and falls back to the browser's store ID when empty. |
| `main.cpp` | Startup now registers the firefox, chrome **and** edge manifests with the stable store IDs (was: Firefox only). |
| `extensions/*/copper.js` | The command-center / options page authenticates its local-API calls (`X-Copper-Token`), refreshing once on 401, instead of fetching tokenless. |
| `extensions/*/background.js` | `ensureApiToken()` no longer caches an empty token; added `refreshApiToken()` and a `getToken` message action for the options page. |
| `extensions/*/manifest.json` | Version `0.1.0` → `0.1.1` (the published `0.1.0` predates the HTTP-first rewrite). |
| `extensions/{Chrome,Firefox}/*.zip` | Rebuilt from the updated sources (root layout, forward-slash `icons/`). |
| `installer/CopperDownloadManager.iss` | Restored `PrivilegesRequired=lowest` + `PrivilegesRequiredOverridesAllowed=dialog`; version + deploy path → `0.4.2`. |
| `tests/unit_tests.cpp` | 2 new cases (21 → 23): stable store IDs; manifest merge + reject-invalid. |
| `tests/integration_test.py` | 5 new checks: Chrome/Edge/Firefox host manifests are written with the store IDs and a correct host path. |
| `tests/validate_extensions.py` | New checks: background sends `X-Copper-Token` and exposes `getToken`; `copper.js` sends `X-Copper-Token`. |
| mirrors | `CMakeLists.txt`, `app.rc`, `installer/arch/PKGBUILD`, `THIRD-PARTY-NOTICES.txt`, CI step label, `RELEASE.md`. |

No existing behavior was removed.

---

## 3. Requirements coverage

| Requirement | Status | Evidence |
|---|---|---|
| Browser-injected download no longer fails "unauthorized" | **PASS** | Live: repair verified on the real machine — Chrome manifest now lists `chrome-extension://cmokehdedogdpbfhlamopeifgogaihni/`, Edge lists `…fnganciafgifhelimkoeiiapdcdmnnlb/`, Firefox lists `CopperDownloadManager@mohamedsubarashi`; host `getToken` returns the same 64-hex token the API enforces; tokenless `/api/downloads` → 401, token-carrying → 200 |
| Both injection paths land a byte-exact file | **PASS** | Live: native-host `download` action (shipped store-extension path) and authenticated `POST /api/download` (updated-extension path) each completed byte-exact (10/10 checks) |
| The host path self-heals after an install moves | **PASS** | Live: a manifest left pointing at `…\release\0.3.5\copper_native_host.exe` was rewritten to `…\release\0.4.2\copper_native_host.exe` on startup |
| Registration never clobbers a sideloaded dev ID | **PASS** | Unit `nativeMessaging_manifestMergesAllowedIds` (dev id + store id both survive; invalid id rejected) |
| Installer always shows all-users / just-me | **PASS** (script) / **NOT RUN** (runtime) | `.iss` carries `PrivilegesRequired=lowest` + `PrivilegesRequiredOverridesAllowed=dialog`; ISCC compiles. The interactive page needs a real install (UAC) to click through |
| Firefox gecko ID matches the published add-on | **PASS** | Unit `nativeMessaging_stableStoreIds` asserts the AMO GUID; matches `extensions/Firefox/manifest.json` |
| Extension status page stops reporting "not reachable" | **PASS** (code) | `copper.js` now sends `X-Copper-Token`; `validate_extensions.py` asserts it. Live browser smoke of the *republished* 0.1.1 is NOT RUN (store update is a separate step) |
| Version `0.4.2` single-sourced; no stale mirrors | **PASS** | `tools/check_version_consistency.py` → "VERSION CHECK PASSED (version 0.4.2 consistent everywhere)" |
| No regressions | **PASS** | Unit 23/0; integration 104/0; version checker PASSED |

---

## 4. Test status

| Test layer | Result |
|---|---|
| Unit tests — `copper_tests` (23 cases; +2 native-messaging cases) | **PASS — 23 passed, 0 failed** |
| Integration suite — `tests/integration_test.py` (104 checks) | **PASS — 104 passed, 0 failed** (99 prior + 5 new manifest checks) |
| Live browser-integration verification (token handshake + both injection paths) | **PASS** — 10/10 on the real machine |
| Extension lint — `tests/validate_extensions.py` | **PASS — Chrome OK, Firefox OK** |
| Version consistency — `tools/check_version_consistency.py` | **PASS** |
| Interactive installer install/uninstall | **NOT RUN** — needs UAC elevation / a clean VM |
| Republished store extension smoke (Chrome/Edge/AMO 0.1.1) | **NOT RUN** — republishing to the stores is a manual step outside the repo |
| Opt-in real-playlist suite (`COPPER_IT_REAL_PLAYLIST=1`) | **NOT RUN** |
| Code-signing of artifacts | **BLOCKED** — no certificate |

---

## 5. Packaging & release

| Item | Status | Evidence |
|---|---|---|
| Windows deploy folder `installer/release/0.4.2/` | **PASS** | Built with Qt 6.12.0 MinGW 13.1 (the local Qt 6.11.2 was superseded on this machine); `windeployqt` laid out Qt DLLs + plugins; `copper_native_host.exe` and notices present; exe version resource reads `0.4.2` |
| Installer compiled | **PASS** (compiled) / **NOT RUN** (runtime install) | `CopperDownloadManager-0.4.2-setup.exe` built with ISCC (17.0 MB); running it needs UAC elevation |
| Portable zip + `SHA256SUMS` + notes in `releases/0.4.2/` | **PASS** | `releases/0.4.2/` holds the portable zip (24 entries, root layout, CI-identical), the setup exe, `SHA256SUMS` (coreutils format, both verify) and the notes |
| CI matrix | **NOT RUN** | To be recorded once the `v0.4.2` tag run completes (expected 7/7: Windows Release + Debug with unit tests, Linux `.deb`, macOS `.dmg`, Arch `.pkg`, extension lint, Publish) |
| Published GitHub release `v0.4.2` | **NOT RUN** | To be published after the tag run from `releases/0.4.2/RELEASE-NOTES.md` |

### Commits

| SHA | Description |
|---|---|
| `(recorded below after the release commit)` | fix(auth): register the browser host manifests with the real store IDs (0.4.2) |

---

## 6. Notes / deviations

- The local build used Qt **6.12.0** / MinGW 13.1. The 0.4.1 build used Qt
  6.11.2, which is no longer installed on this machine; the source is unchanged
  and CI still builds its pinned Qt version. The produced deploy folder and both
  bundled DLL sets were re-verified by the suites and the live check.
- `tools/check_version_consistency.py`'s `PREVIOUS_RELEASES` was left at
  `("0.3.1",)`, as in 0.4.0/0.4.1: appending `0.4.0`/`0.4.1` would flag the
  intentional feature-history references in source and `RELEASE.md`. The mirrors
  that must move are all exact-match enforced and verified.
- The **app-side** fix is what unblocks the published store extensions (their
  native path needs no token). The new `0.1.1` extension (authenticated options
  page) only takes effect after it is republished to the Chrome Web Store /
  Edge Add-ons / AMO; that is a manual store step, reported NOT RUN above.
- `installManifest()` preserves a syntactically valid but unknown ID already in
  the file (e.g. the old Chrome placeholder). It is harmless and is kept so a
  dev registration is never dropped; validation only prevents *writing* a
  malformed ID.
