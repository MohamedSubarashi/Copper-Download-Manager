# Copper Download Manager 0.5.0

Localization release. The app and both browser extensions now ship in **English,
French, Arabic, Spanish and Brazilian Portuguese**. No features were removed and
behaviour is unchanged apart from the new language selector — but the strings
themselves carry a caveat (see *Known limitations*).

## What's new

### The app speaks your language

- Every user-facing string — main window, context menus, settings, the add-URL
  and file-conflict dialogs, tool/status text, error messages — now goes through
  Qt Linguist catalogs (`translations/copper_{en,fr,ar,es,pt_BR}.ts`, **457
  messages** each).
- The compiled catalogs are **embedded in the executable**
  (`:/translations/copper_<code>.qm`), so every packaging format (Windows
  installer, portable zip, `.deb`, `.pkg.tar.zst`, `.dmg`) ships translations
  with no extra files and no chance of a stray `.qm` going missing.
- Qt's own `qtbase_*.qm` translations are now deployed next to the exe
  (`windeployqt` is no longer run with `--no-translations`), so the **built-in
  Qt dialogs** — file picker, confirmation message boxes, standard buttons — are
  localized too, instead of always appearing in English.
- New **Settings → General → Language** selector: *Auto (system)*, English,
  Français, العربية, Español, Português (Brasil). The choice is persisted, and
  switching language offers to **restart the app** to apply it (there is no live
  retranslate, by design).
- Arabic sets the layout to **right-to-left** (menus, dialogs, window layout).

### Browser extensions localized (0.1.3 → 0.1.4)

- Standard WebExtension localization: `_locales/{en,fr,ar,es,pt_BR}/messages.json`
  (**85 messages**), `default_locale: "en"`, and `__MSG_*__` manifest tokens for
  the extension name/description/tooltip. Both the Chrome and Firefox builds are
  affected and stay byte-identical apart from the manifest wording.
- Localized surfaces: the context-menu titles, the floating media-bar labels and
  tooltips, the failure notifications, and the status/command-center page
  (settings, download rows, capture toggle).
- The status page rewrites its `data-i18n` hooks at load and sets
  `<html lang>` / `dir`, switching to **RTL for Arabic**.

## Verification

- `copper_tests`: **26 passed, 0 failed** — new case
  `translations_areEmbeddedAndLocalized` loads the embedded catalog from the
  binary and asserts a known message resolves to the translated text (including
  the right-to-left Arabic string), so a catalog that silently failed to build
  cannot fall back to English unnoticed.
- Integration suite: **117 passed, 0 failed** (no behaviour change expected or
  observed).
- `tests/validate_extensions.py`: Chrome OK, Firefox OK — now also validates
  `default_locale`, per-locale key parity, `$1` placeholder parity and that every
  `__MSG_*__` reference resolves in the `en` catalog.
- `tools/check_translations.py` (**new**): all 5 catalogs complete (457/457
  messages), nothing `unfinished`, no `%1` placeholder drift, `en` carries no
  stale copy of its own text, the app and both extensions expose the same
  language set, and `CMakeLists.txt` compiles exactly the catalogs on disk.
- `tools/build_extension_zips.py` (**new**): both tracked extension zips rebuilt
  with their `_locales`; `--check` fails if a zip goes stale.
- `tools/check_version_consistency.py`: version `0.5.0` consistent everywhere.

## Install

- **Windows installer**: `CopperDownloadManager-0.5.0-setup.exe`
- **Windows portable**: extract `CopperDownloadManager-portable-0.5.0.zip`
  anywhere and run `CopperDownloadManager.exe`
- **Linux**: `copper-download-manager-0.5.0-Linux.deb` (Debian/Ubuntu) or
  `copper-download-manager-0.5.0-1-x86_64.pkg.tar.zst` (Arch)
- **macOS**: `CopperDownloadManager.dmg`
- `SHA256SUMS` lists the Windows assets; the full set is below.

### SHA-256 of the published assets

```
33915e16da654a21dcbbedfa08fe0b525e2b0a42a112dfa5df66cf0553976c98  CopperDownloadManager-0.5.0-setup.exe
72e0a02957d0ef542d249ebc6db67fa5532b2b5286dc8abbe47dd7d045b20b73  CopperDownloadManager-portable-0.5.0.zip
```

The Linux and macOS artifacts are built by CI from the tag; their hashes are
appended to `SHA256SUMS` once they are published.

## Known limitations

- **The translations are machine-assisted.** The French, Arabic, Spanish and
  Brazilian Portuguese strings were authored by an AI assistant from the English
  source and have **not yet been reviewed by a native speaker**. They are
  expected to be correct and complete (the automated checks guarantee full
  coverage and intact placeholders), but phrasing, register or regional
  vocabulary may occasionally be off — Brazilian Portuguese in particular uses
  different terms from European Portuguese. Corrections are very welcome; every
  catalog lives in `translations/*.ts` and the extension's in
  `extensions/*/_locales/*/messages.json`, and
  `tools/check_translations.py` will catch an incomplete edit.
- **Arabic needs a visual RTL pass.** The right-to-left layout is implemented
  (Qt's layout direction plus `dir="rtl"` on the extension page) but it was not
  possible to run the app in this development environment and *look* at it. No
  visual check of mirroring, clipping or bidi text ordering has been made on a
  real machine, so please report RTL rendering problems.
- **The Inno Setup installer itself is still English-only.** The installer's
  language list was left unchanged (deferred); the *installed app* picks its own
  language at runtime, so only the setup wizard remains English.
- Untranslated by design: URLs, file-format tokens (MP4/MP3), stored status
  values, file-extension filters, log output, the User-Agent, and the product
  name "Copper Download Manager".
- Binaries are not code-signed (no signing certificate available in this cycle);
  Windows SmartScreen may warn on first run.
- The add-ons already published in the Chrome Web Store / Edge Add-ons / AMO are
  older than this tree. The localized **0.1.4** extension takes effect once it is
  republished to the stores — a separate manual step.
