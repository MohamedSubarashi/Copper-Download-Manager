# Implementation Report — Copper Download Manager 0.5.0

Localization release on top of 0.4.3. This is **not** a rewrite: the
architecture, interfaces and feature set are preserved. The change adds full
internationalization to the desktop app (Qt Linguist) and to both browser
extensions (WebExtension `_locales`), exposing a new language selector that
persists and prompts for a restart.

Statuses use the required vocabulary: **PASS / FAIL / NOT RUN / BLOCKED**.
"PASS" means the behavior was executed and asserted by a check listed under
*Evidence*; anything not executed is reported as NOT RUN, never inferred.

---

## 1. Scope

- **Languages**: `en` (source) plus `fr`, `ar`, `es`, `pt_BR` — five catalogs,
  identical in shape on both sides (app and extensions).
- **Authoring**: all translations were authored by an AI assistant (best
  effort). Automated checks guarantee *completeness* and *placeholder integrity*,
  not native-speaker quality; the caveat is carried into the release notes.
- **Apply model**: a language change persists and prompts for a **restart**
  rather than live-retranslating a running window (deliberate: no partially
  translated UI during a session).
- **Deliberately untranslated**: URLs, stored logic values (download status
  strings, file-format tokens), file-extension filter lists, `Logger` output,
  registry values, external tool names, date-format specifiers and the brand
  name.

## 2. What changed

### 2.1 App i18n pipeline (`CMakeLists.txt`, `translations/`)

- Catalogs `translations/copper_{en,fr,ar,es,pt_BR}.ts`; `lupdate` extracted
  **457 messages across 15 contexts**.
- The `.ts` files are compiled with `lrelease` and the resulting `.qm` are
  **embedded into the executable** through a generated resource file, exposed as
  `:/translations/copper_<code>.qm` — no deploy step, identical for every
  packaging format.
- `windeployqt` is now run **without `--no-translations`**, so Qt's own
  `qtbase_*.qm` files land in `translations/` beside the exe and the built-in
  Qt dialogs are localized too.
- The whole block is guarded so a Qt install without the Linguist tools
  configures and builds cleanly (translations simply stay English).

### 2.2 Translation manager + language selector

- New `TranslationManager` (single load-at-startup instance): loads the app
  catalog and the matching `qtbase` translator, probes the app dir's
  `translations/`, the macOS bundle `Resources/translations/` and Qt's own
  translation path, and flips the application to **right-to-left** for Arabic.
- New **Settings → General → Language** combo (*Auto (system)* + the five
  languages), persisted under the `language` key. Changing it saves the setting
  and offers a restart; the previous selection is restored if the dialog is
  cancelled.
- `main.cpp` applies the persisted choice before the first window is shown.

### 2.3 String wrapping (source change only)

- All UI (main window, dialogs, context menus, status/tray text, settings) and
  the user-visible payloads of core/utils signals now go through `tr()` /
  `QCoreApplication::translate`. No behaviour was altered — only string
  literals were replaced, and the wrapped strings were translated afterwards.
- Download status values stay **raw in storage and logic** and are translated
  for display only, so database rows and API responses are untouched.

### 2.4 Browser extension i18n (`extensions/Chrome`, `extensions/Firefox`)

- New `_locales/{en,fr,ar,es,pt_BR}/messages.json` (**85 messages** each) and
  `default_locale: "en"`; manifests use `__MSG_*__` for name/description/tooltip
  (with the two wordings kept as separate keys, since only the manifest differs
  between the browsers).
- Status page: every user-facing string carries a `data-i18n` /
  `data-i18n-title` hook rewritten at load by a localizer that also sets
  `<html lang>` and `dir="rtl"` for Arabic. English text remains in the markup
  as the fallback, and a missing message never blanks a label.
- `copper-bar.js`, `copper.js` and `background.js` read their strings through
  `i18n.getMessage` (context-menu titles, failure notifications, media-bar
  labels, playlist detection, capture toggle).
- Extension version `0.1.3 → 0.1.4`; both builds stay byte-identical apart from
  the manifest.

### 2.5 Tooling, CI and version single-source

- New `tools/check_translations.py`: fails on an incomplete or unsafe catalog
  (missing/`unfinished` message, empty translation, dropped `%1` placeholder, an
  `en` catalog carrying its own stale text, a language on disk the build never
  compiles, or an app/extension locale-set mismatch).
- New `tools/build_extension_zips.py`: rebuilds the two tracked zips
  deterministically (root layout, forward slashes, `_locales` included) and
  `--check` fails when a zip is stale (line-ending-normalized comparison, so a
  Windows-built zip passes a Linux CI checkout).
- `tests/validate_extensions.py` gained an i18n pass (default_locale, catalog
  parity, placeholder parity, `__MSG_*__` resolution, `data-i18n` hooks present).
- `.github/workflows/ci.yml` runs both new checks in the extension-lint job.
- Version `0.4.3 → 0.5.0`, mirrored into `app.rc`,
  `installer/CopperDownloadManager.iss`, `installer/arch/PKGBUILD`,
  `THIRD-PARTY-NOTICES.txt` and the CI step label.

## 3. Evidence

### 3.1 Build

| Check | Status | Evidence |
| --- | --- | --- |
| Windows Release build + `windeployqt` to `installer/release/0.5.0/` | **PASS** | Qt 6.12.0 / MinGW 13.1; 58 files incl. `CopperDownloadManager.exe`, `copper_native_host.exe`, Qt DLLs + plugins |
| exe reports version `0.5.0` | **PASS** | `VersionInfo` → FileVersion/ProductVersion `0.5.0` |
| `.qm` really embedded | **PASS** | `copper_tests::translations_areEmbeddedAndLocalized` loads `:/translations/copper_<code>.qm` from the binary and asserts the translated (and RTL Arabic) text |
| Qt dialog translations shipped | **PASS** | `installer/release/0.5.0/translations/` holds 33 `qt_*.qm` (the 0.4.3 folder held none) |

### 3.2 Tests

| Check | Status | Evidence |
| --- | --- | --- |
| Unit suite | **PASS** | `copper_tests` → `26 passed, 0 failed` (new: `translations_areEmbeddedAndLocalized`) |
| Integration suite | **PASS** | `117 passed, 0 failed`, process exit code `0` |
| Extension validation | **PASS** | `tests/validate_extensions.py` → Chrome OK, Firefox OK, incl. the new i18n pass |
| Translation catalogs | **PASS** | `tools/check_translations.py` → 5 catalogs × 457 messages complete, 0 unfinished, 0 placeholder drift |
| Translation checker detects defects | **PASS** | Negative test: a mutated `copper_fr.ts` (emptied translation + dropped `%1`) → exit 1 with 3 findings; restored afterwards |
| Extension zips fresh | **PASS** | `tools/build_extension_zips.py --check` → both match their sources; negative test (touched `copper.js`) → exit 1 |
| Version consistency | **PASS** | `tools/check_version_consistency.py` → "VERSION CHECK PASSED (version 0.5.0 consistent everywhere)" |

### 3.3 Packaging (local pre-tag build)

| Check | Status | Evidence |
| --- | --- | --- |
| Windows installer built | **PASS** | ISCC 6.7.3 → `installer/CopperDownloadManager-0.5.0-setup.exe`, copies `release\0.5.0\*` incl. the `translations/` folder |
| Windows portable zip built | **PASS** | `installer/release/CopperDownloadManager-portable-0.5.0.zip` (58 entries, 33 `.qm`) |
| `releases/0.5.0/` staged | **PASS** | both artifacts + `RELEASE-NOTES.md` + `SHA256SUMS` (`sha256 33915e16…`, `72e0a029…`) |

The published assets are the ones CI builds from the tag (same flow as 0.4.3);
the local pre-tag build is not byte-reproducible, so `releases/0.5.0/` is
reconciled to the published artifacts afterwards.

### 3.4 Not executed (reported honestly)

| Check | Status | Why |
| --- | --- | --- |
| Visual QA of any language, incl. **Arabic RTL layout** | **NOT RUN** | Requires a display; the app cannot be *looked at* in this environment. RTL is implemented and unit-asserted at the string level only |
| Native-speaker review of the translations | **NOT RUN** | No reviewer available this cycle; the strings are assistant-authored |
| Interactive installer install/uninstall | **NOT RUN** | Needs UAC / a disposable VM |
| Real browser E2E with the localized extension | **NOT RUN** | Not automatable headlessly here; extension i18n is covered by the offline validation |
| Inno Setup wizard localization | **NOT RUN** | Deferred by decision: the installer language list is unchanged (English only) |
| Opt-in real-playlist suite (`COPPER_REAL_PLAYLIST=1`) | **NOT RUN** | Optional; requires yt-dlp/ffmpeg and network |
| Code signing | **BLOCKED** | No certificate available this cycle |

## 4. Honest limitations

- **Quality, not coverage, is the risk.** Every message is present and its
  placeholders intact (asserted in CI), but phrasing/terminology is unreviewed —
  notably European-vs-Brazilian Portuguese differences.
- **Arabic is unverified visually.** `dir="rtl"` / `LayoutDirection` is set and
  the strings render, but no human has inspected mirroring, clipping or bidi
  ordering on a real machine.
- A translation that *is* missing falls back to English text (never a raw key or
  an empty label), because the English string stays in the markup/source as the
  default.
- The Inno Setup wizard remains English-only; only the installed app is
  localized.
- No interactive installer run, no real-browser automation, no code signing
  (see 3.4).

## 5. Release

| Item | Status | Evidence |
| --- | --- | --- |
| Release commit + tag | *pending* | to be filled once pushed |
| CI green on the tag | *pending* | |
| GitHub release published | *pending* | https://github.com/MohamedSubarashi/Copper-Download-Manager/releases/tag/v0.5.0 |
| Curated release notes applied | *pending* | `gh release edit v0.5.0 --notes-file releases/0.5.0/RELEASE-NOTES.md` |

## 6. Decisions / deviations

- **`qt_add_translations` was not used.** Qt's macro failed on this tree (a
  self-referential `SOURCCE`-genex `file(GENERATE)` "written multiple times"
  error, and it mishandled paths containing spaces), so the `lrelease` → `rcc` →
  `target_sources` pipeline in `CMakeLists.txt` is explicit instead.
- **`PREVIOUS_RELEASES` was left at `("0.3.1", "0.4.2")`.** Its comment suggests
  appending the superseded version, but 0.4.3 is intentionally referenced
  throughout the source as feature history (native-host self-registration, the
  `/api/register` fallback), exactly as 0.4.0 was in the 0.4.1 report, so
  appending `"0.4.3"` would raise false positives rather than catch stale
  mirrors. The mirrors that must move are all exact-match enforced and verified.
- **Restart instead of live retranslate**, per scope.
- The translation scaffolding used to author the catalogs (raw `lupdate` dumps
  and a throwaway apply script) was **not** committed; only the resulting `.ts`
  catalogs and the permanent checkers are part of the tree.
