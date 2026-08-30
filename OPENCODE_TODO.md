# OpenCode Handoff Todo List

Project: Copper Download Manager
Type: Qt 6 desktop app + browser extension (Chrome/Firefox)
Current version: 0.3.5

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
- [x] Add automated regression tests for critical flows: launch with URL, already-running instance, minimized startup, and protocol link handling. (`tests/integration_test.py`, 14 cases)

## Priority 2: Download engine and queue management

- [x] Review the core download manager for queue ordering, retry logic, pause/resume correctness, and cancellation edge cases. (yt-dlp pause/resume/cancel now drive the process; no more duplicate/orphaned yt-dlp; pause no longer corrupts the file; resume of non-range servers restarts; per-chunk hang timeout 90s)
- [x] Improve progress reporting accuracy during chunked downloads and multi-part transfers. (drains trailing bytes before completion; unknown-length downloads now reach 100% via final byte count; DB progress writes throttled to 500ms)
- [x] Validate resume behavior across interruptions, file corruption, and partial download states. (byte-exact completion + truncated-drop detection verified by new integration tests; truncated connections no longer marked Completed)
- [x] Verify speed limiter implementation and ensure it does not cause CPU or UI stalls. (real token-bucket throttle in `ChunkedDownloader` + hysteresis in `DownloadManager::processSpeedLimit`, commit `873e16f`)
- [x] Review database persistence for download state and ensure recovery is consistent after app restart. (parent_id now persisted/migrated; restart marks stale as Failed by design)

## Priority 3: Browser extension integration

- [x] Confirm Chrome and Firefox extension behavior is aligned for interception, URL capture, and native app communication. (background/popup/content JS byte-identical; only intentional MV3 manifest differences)
- [x] Review permission usage in both extensions and remove any unnecessary access. (contextMenus/storage/tabs/notifications + `<all_urls>` all in use)
- [x] Validate the desktop app receives intercepted link data reliably from the browser extension and handles it consistently. (copper:// flow verified: `encodeURIComponent` on extension side, `decodeOnce()` in `parseCopperLink()`)
- [x] Improve error handling when the desktop app is not running or when the browser cannot reach it. (`pingCopper()` + `notifyCopperUnreachable()` in both `background.js`, commit `7794a2c`)
- [x] Add manifest and compatibility checks for Firefox and Chrome release requirements. (added `tests/validate_extensions.py` wired into CI: MV3 shape per browser, toolbar `action`+`default_popup` wiring, Firefox `gecko.id` + honest `data_collection_permissions required:["none"]`, icon presence; wired the previously-unreachable popup to the toolbar button via `action`)

## Priority 4: Protocol and desktop integration

- [x] Formalize and test the copper:// protocol flow for open, create-download, and URL forwarding scenarios. (end-to-end test added: extension-format `copper://download?url=&filename=&path=` (encodeURIComponent) forwarded to the running app produces a byte-exact HTTP download; `copper://open` and main.cpp/MainWindow intake verified consistent; integration suite now 22 cases)
- [x] Register and validate the desktop protocol association on Windows/macOS/Linux where appropriate. (`.desktop` for Linux + `MsMimeType`/`x-scheme-handler`; Windows registry registration validated; `autoUpdateRegistryPath()` now repairable — `copper` base key + command always refreshed on startup, heals partial/moved installs even when the app was never formally registered as default)
- [x] Ensure command-line arguments and protocol calls populate the same download intake path. (both `main.cpp` and `MainWindow::onArgumentForwarded` route magnet/http/copper through the identical `DownloadManager` intake + `DownloadManagerDialog` where selection is needed)
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

- [ ] App builds successfully in release mode
- [ ] Core download flows work reliably without data loss
- [ ] Browser extension to desktop integration is stable
- [ ] Torrent/media tools install and run without critical failures
- [ ] Settings and state recovery survive reopen/restart
- [ ] A basic automated regression suite exists for critical flows

## Suggested first task for OpenCode

Start by auditing the startup and argument forwarding flow in the main entry point and the browser extension message path, because these are the most likely sources of user-facing instability and broken integrations.
