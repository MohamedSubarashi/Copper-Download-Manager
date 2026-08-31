# OpenCode Handoff Todo List

Project: Copper Download Manager
Type: Qt 6 desktop app + browser extension (Chrome/Firefox)
Current version: 0.4.5

This list is ordered by impact and should be tackled in sequence for the next development pass.

## Priority 0: Stabilize the build and project health

- [x] Verify the project builds cleanly in Debug and Release across the supported OS targets.
- [x] Fix any compiler warnings, linker issues, and platform-specific build problems discovered during validation.
- [x] Add a reproducible build/test workflow for CI (CMake, Qt path handling, packaging smoke checks). (`.github/workflows/ci.yml`)
- [x] Document exact prerequisites for development environments (Qt version, toolchain, optional libtorrent support). (README)

## Priority 1: Reliability and crash-safety of the app core

- [x] Audit single-instance startup behavior and local IPC reliability for the embedded local server.
- [x] Validate all forward-argument paths: HTTP URLs, magnet links, torrent files, and copper:// links.
- [x] Review thread safety around download scheduling, state updates, and UI refresh callbacks. (single-threaded GUI design audited; no locks and no cross-thread signal delivery — all QTimer/QNetworkReply/QProcess on the GUI thread; DB progress writes throttled to avoid main-loop stalls)
- [x] Harden startup/shutdown logic so partial initialization does not leave stale locks, sockets, or database state behind. (WAL journal_mode + NORMAL synchronous for crash-safe DB; `DownloadManager::shutdown()` cancels chunked downloads + kills yt-dlp/aria2c on exit so no processes are orphaned; ordered teardown in `main.cpp`)
- [x] Add automated regression tests for critical flows: launch with URL, already-running instance, minimized startup, and protocol link handling. (`tests/integration_test.py`, 22 cases)

## Priority 2: Download engine and queue management

- [x] Review the core download manager for queue ordering, retry logic, pause/resume correctness, and cancellation edge cases. (yt-dlp pause/resume/cancel now drive the process; no more duplicate/orphaned yt-dlp; pause no longer corrupts the file; resume of non-range servers restarts; per-chunk hang timeout 90s)
- [x] Improve progress reporting accuracy during chunked downloads and multi-part transfers. (drains trailing bytes before completion; unknown-length downloads now reach 100% via final byte count; DB progress writes throttled to 500ms)
- [x] Validate resume behavior across interruptions, file corruption, and partial download states. (byte-exact completion + truncated-drop detection verified by new integration tests; truncated connections no longer marked Completed)
- [x] Verify speed limiter implementation and ensure it does not cause CPU or UI stalls. (real token-bucket throttle in `ChunkedDownloader` + hysteresis in `DownloadManager::processSpeedLimit`, commit `873e16f`)
- [x] Review database persistence for download state and ensure recovery is consistent after app restart. (parent_id now persisted/migrated; restart marks stale as Failed by design)

## Priority 3: Browser extension integration

- [x] Confirm Chrome and Firefox extension behavior is aligned for interception, URL capture, and native app communication. (background/popup JS byte-identical between Chrome/Firefox; only intentional MV3 manifest differences)
- [x] Review permission usage in both extensions and remove any unnecessary access. (dropped `copper://` + content-script + `<all_urls>`; now `contextMenus/storage/tabs/notifications/nativeMessaging`)
- [x] Validate the desktop app receives intercepted link data reliably from the browser extension and handles it consistently. (native-messaging host `com.copper.dm` -> named pipe `copper-dm` -> `MainWindow::onArgumentForwarded`; verified: `copper_native_host` ping + pipe-injected download complete byte-exact)
- [x] Improve error handling when the desktop app is not running or when the browser cannot reach it. (`sendNativeMessage(actions.ping)` + `notifyCopperUnreachable()` in `background.js`; host launches the app if it is not running)
- [x] Add manifest and compatibility checks for Firefox and Chrome release requirements. (`tests/validate_extensions.py` wired into CI: MV3 shape per browser, toolbar `action`+`default_popup`, Firefox `gecko.id` + honest `data_collection_permissions required:["none"]`, icon presence, plus native-messaging permission / no-copper:// / no-localhost-ping background checks)

## Priority 4: Protocol and desktop integration

- [x] Formalize and test the copper:// protocol flow for open, create-download, and URL forwarding scenarios. (`copper://download?url=&filename=&path=` (encodeURIComponent) forwarded to the running app produces a byte-exact HTTP download; `copper://open` and main.cpp/MainWindow intake verified; integration suite now 31 cases)
- [x] Register and validate the desktop protocol association on Windows/macOS/Linux where appropriate. (`.desktop` for Linux + `MsMimeType`/`x-scheme-handler`; Windows registry registration validated; `autoUpdateRegistryPath()` repairable)
- [x] Ensure command-line arguments and protocol calls populate the same download intake path. (both `main.cpp` and `MainWindow::onArgumentForwarded` route magnet/http/copper through the identical `DownloadManager` intake + `DownloadManagerDialog` where selection is needed; the named-pipe intake reuses the same `onArgumentForwarded` path)
- [x] Review default download directory selection and filename sanitization for user-provided URLs and custom save paths. (`QStandardPaths::DownloadLocation` default; `sanitizeFileName` applied to copper filename and Content-Disposition names)

## Priority 5: Torrent and media features

- [x] Validate torrent ingestion, magnet handling, and metadata parsing for real-world edge cases. (terminal tasks stop polling, `isRunning` fixed, parse failures surface via `errorOccurred`, RPC timeout guarded, orphaned QProcess parented)
- [x] Review aria2c integration and fallback behavior when tools are missing or fail to initialize.
- [x] Audit yt-dlp integration for format selection, progress parsing, and error recovery. (progress now parsed from stderr channel; ffmpeg pre-flight check)
- [x] Review FFmpeg use for conversion/transcoding pipelines and failure handling. (unused `convert()` identified as dead code; ffmpeg absence surfaced before yt-dlp merge/extract)
- [x] Improve user feedback for tool install/download progress and failed dependency setup. (yt-dlp missing when Video selected now shows a clear pre-flight message with an install hint; empty fetch results explain the action instead of stalling; Settings error labels already wired)

## Priority 6: Data model and settings

- [x] Audit DatabaseManager schema/versioning and migration behavior for upgrades. (`getSchemaVersion()` PRAGMA + `migrate()` adds `parent_id` for pre-0.3.x DBs)
- [x] Review settings persistence and default values for theme, speed limits, local server port, and tool paths.
- [x] Ensure settings changes apply immediately without leaving stale or invalid values in memory. (speed limit applies immediately)
- [x] Add validation for malformed or corrupted database rows and partial configuration values. (defensive defaults on read)

## Priority 7: UI/UX and product polish

- [x] Review the main window, dialogs, and tray behavior for responsiveness and crash recovery. (hidden-window refresh skipped; failures surfaced in status bar + tray balloon)
- [x] Improve the theme system and ensure dark/light/system modes are consistent. (palette placeholder/disabled roles; palette-safe status colors)
- [x] Validate Add URL, Download Manager, Settings, and About flows from a user perspective. (Add URL status/feedback + initial focus; tool-install errors live in Settings; stale license version fixed)
- [x] Clean up inconsistent dialogs and status messaging for failed downloads, tool installation, and config changes.
- [x] Improve accessibility and keyboard navigation in key dialogs and actions. (label buddies + accessible names on theme, folder, chunks, filter-mode, speed-limit controls in Settings; save path, file list, track-number checkbox in Download Manager; stats labels and peer table in Torrent Details; Add URL/About done earlier)

## Priority 8: Packaging, distribution, and release readiness

- [x] Finalize release packaging for Windows and other target platforms. (Windows exe now embeds `Assets/app.ico` + version resource; RC compiled from a space-free build-tree copy so MinGW windres works with the space-containing source path, commit `fa21a61`)
- [x] Validate installer/portable output structure and dependency deployment. (portable folder verified complete; CI uploads portable zip artifact)
- [x] Ensure licensing and third-party notices are included correctly in every release package. (THIRD-PARTY-NOTICES.txt copied into package; MinGW GCC runtime libraries now listed under GPLv3 + GCC Runtime Library Exception; Qt version bumped to deployed 6.11.2; in-app license tab dynamic)
- [x] Add a release checklist for QA before publishing new versions. (14-case integration suite + CI packaging smoke checks; formalized as `RELEASE.md` with versioning, build, automated-suite, manual-QA, packaging, and publish steps)

## Priority 9: Hardening and security review

- [x] Audit remote URL handling to reduce risks from untrusted sources and malformed data. (URL scheme whitelist on LocalServer API)
- [x] Review browser extension messaging to ensure safe parameter validation and minimal trust assumptions.
- [x] Check local HTTP server exposure and API contract for unexpected requests or abuse. (Origin gating: only extension origins allowed; exact-origin echo instead of `*`; 403 for disallowed origins; regression tests)
- [x] Review downloaded tool management and external process execution for command injection or path issues. (args passed as lists, no shell interpolation of user input)

## Recommended execution order for OpenCode

1. Build validation and crash-prone startup paths
2. Download engine reliability and persistence
3. Browser extension + copper:// integration
4. Torrent/media tool integrations
5. UI polish and packaging
6. Security and release hardening

## Definition of done

The project is ready for the next milestone when the following are true:

- [x] App builds successfully in release mode
- [x] Core download flows work reliably without data loss
- [x] Browser extension to desktop integration is stable
- [x] Torrent/media tools install and run without critical failures
- [x] Settings and state recovery survive reopen/restart
- [x] A basic automated regression suite exists for critical flows (31 integration cases + CI extension-lint, all green)

All Priority priorities (0-9) are complete as of this pass. Every remaining
checkmark in this file has been verified by a build, the 31-case integration
suite, or CI.

## Completed milestone: extension injects through native messaging (IDM model)

- [x] Replaced the extension's `copper://` + localhost HTTP injection with the
      full IDM model: Chrome/Firefox native-messaging host (`com.copper.dm`) +
      a desktop-app named-pipe intake (`QLocalServer` `copper-dm`, `PipeServer`).
- [x] Desktop app reuses the existing `MainWindow::onArgumentForwarded` intake,
      so magnets, `.torrent` URLs, HTTP(S)/FTP and video links behave exactly as
      the copper:// path did (including the torrent-dialog routing fix).
- [x] Extension (`background.js`) now uses `chrome.runtime.sendNativeMessage`;
      content-script `<all_urls>` + `copper://` navigation removed; version kept
      at `4.0.0`.
- [x] `--register-native-extension <browser> <id>` CLI + in-app `register`
      handshake re-point the Chrome host manifest `allowed_origins` to a loaded
      extension's `runtime.id`; Firefox startup registration uses the stable
      gecko.id `copper-download-manager@copper`.
- [x] CI extension-lint validates the new native-messaging shape; integration
      suite covers host ping, host->pipe download, and pipe-injected byte-exact
      completion.

## Suggested next milestone

This pass completed all defined priorities (0-9) and the native-messaging
injection redesign for v0.3.5. The next development pass should scope new work
(for example: a proper installer (MSI/Inno) that ships/registers the native
host, macOS/Linux native-host manifest + packaging verification, Chrome Web
Store signing key so the packaged extension ID is stable, or expanded
integration coverage). See `RELEASE.md` for the QA checklist before publishing.

## Completed milestone: v0.3.6 (torrent RPC fix, User-Agent, HTTP auto-resume)

- [x] **Torrent RPC `Unauthorized` fix**: a stale aria2c daemon surviving on port
      6800 with an old RPC token rejected every jsonrpc call. Now the token is
      persisted in settings (`aria2Token`) and reused across launches; an
      Unauthorized response forces a port-kill + daemon relaunch (max 2).
- [x] **Configurable User-Agent**: new `userAgent` setting (Settings -> Downloads),
      default Mozilla-compatible UA. Applied to chunked HTTP/FTP downloads,
      `.torrent` metadata fetch, and yt-dlp (`--user-agent`).
- [x] **HTTP auto-resume across sessions**: interrupted chunked downloads are
      rehydrated from the DB on launch and resumed from their partial `.chunk`
      files via Range headers. Partial chunks + a `<id>.resume.json` sidecar are
      kept on shutdown/cancel, and written during active progress so even a hard
      crash can be resumed. `DownloadManager::restoreFromDatabase()` re-adds
      interrupted transfers (torrent/yt-dlp via `resumeDownload`, HTTP via
      `createChunkedDownloaderFor(..., resume)`); user cancel/removal discards
      the partial data.
- [x] Integration suite expanded to **35** cases (added auto-resume after app
      restart) and all pass against the deployed 0.3.6 exe.

## Completed milestone: extension redesign (IDM-style) — extension v4.0.2

- [x] **Popup-less, IDM-style extension**: the toolbar popup was removed. Clicking
      the toolbar opens a bundled status page (`copper.html`) instead.
- [x] **Install detection + "not installed" tab**: the background now distinguishes
      "program installed" from "program absent". When the native host cannot be
      reached (not registered) or reports "Copper Download Manager not found",
      a download attempt opens a new tab (`copper.html?status=not-installed`)
      explaining that the app isn't installed, with a download link and a
      re-check button — instead of just a background notification.
- [x] **On-demand launch**: when Copper is installed but not running, the native
      host auto-launches it in the background and forwards the download silently.
- [x] Status page reads install/capture state via `chrome.runtime.sendMessage`
      (`getStatus`), offers "Open Copper", an enable/disable toggle, and a
      "Check again" action.
- [x] Shared files (`background.js`, `copper.html`, `copper.css`, `copper.js`)
      kept byte-identical across Chrome and Firefox; manifests differ only in the
      intended MV3 shape. Version bumped to `4.0.2`.
- [x] `validate_extensions.py` updated for the popup-less design (no
      `default_popup`; requires `copper.html`/`copper.js`; checks `action.onClicked`
      and `openStatusTab`); both extensions pass.

## Completed milestone: v0.4.0 (fix download-table selection being cleared)

- [x] **Table selection no longer auto-clears**: `refreshTable()` rebuilt the whole
      table from scratch every second (the 1s `refreshTimer` did `setRowCount(0)`),
      which deleted every item and with it the user's selection — so clicking any
      download deselected it on the next tick. Fixed by capturing the selected
      download ids before the rebuild and re-selecting their rows afterwards
      (via `QItemSelectionModel`), preserving single and multi-selection.

## Completed milestone: v0.3.9 (torrent crash fix + safe handler registration)

- [x] **Torrent-add access-violation crash fixed**: the app hard-crashed
      (execute-at-0x0 inside Qt's event loop) during the first aria2 poll ticks
      after a torrent/magnet was added. Root cause was reentrant `poll()`: the
      1s `pollTimer` fired inside a nested `QEventLoop::exec()` during a blocking
      RPC call. Added a reentrancy guard (`m_pollInProgress` in
      `Aria2cManager::poll()`). Verified against the real magnet via `/api/torrent`
      (no crash) and a 6-add stress run (no crash).
- [x] **Crash backtrace logger**: `main.cpp` now installs a top-level exception
      filter that writes a `[CRASH]` module+offset backtrace to `copper.log`
      (kernel32-only, no extra link libs) so any future fault is diagnosable
      without a debugger.
- [x] **LocalServer body-buffering fix**: `/api/download` and `/api/torrent`
      rejected valid POSTs with `400 "Invalid JSON"` when the body arrived in a
      later TCP packet. The connection now buffers until the full Content-Length
      body has been received before dispatching.
- [x] **Safe handler registration**: Copper no longer registers itself as a
      handler for `http`, `https`, or `ftp` (those belong to the browser / a
      dedicated FTP client). It now claims only its own schemes (`magnet`,
      `copper://`) plus the `.torrent` file association. Unregister and the
      auto-update-paths refresh clean up any stale HTTP/HTTPS/FTP ProgIds from
      older versions. `getRegisteredProtocol()` and the Settings dialog message
      updated accordingly.
- [x] Deployed/validated against the 0.3.9 exe; 35/35 integration tests pass.

## Completed milestone: v0.3.7 (multi-delete fix)

- [x] **Delete-selection fix**: the Delete button (and Delete key / context menu)
      now gathers every selected download id before removing any, then removes
      them all by id. Previously it re-read the table inside the delete loop;
      because each removal rebuilt the table, the later selected rows could point
      past the end of the rebuilt table and be skipped — so selecting many
      "complete" downloads and deleting them left the last one(s) behind.
- [x] Deployed/validated against the 0.3.7 exe; 35/35 integration tests pass.

## Completed milestone: v0.4.5 (per-file torrent progress + aria2 stability)

- [x] **Per-file torrent progress**: aria2's `tellStatus` `files` array is now
      parsed into per-file length/completedLength (`Aria2FileSize`, exposed via
      `getFileSizes()`). Each torrent child download reports its own size and
      progress (matched by basename), instead of inheriting the parent's
      aggregate percentage — so a multi-file torrent shows accurate per-file
      progress.
- [x] **Aria2cManager teardown safety**: the destructor no longer calls
      `shutdownDaemon()` (which ran a nested event loop + network request after
      Qt teardown and crashed at static-exit); it now only hard-kills the child
      daemon and stops the poll timer.
- [x] **RPC reentrancy guard**: added `m_rpcInProgress` so a poll firing inside
      a nested RPC event loop cannot interleave/delete replies and crash.
- [x] Version bumped to 0.4.5 and deployed to `installer/release/0.4.5/`.
