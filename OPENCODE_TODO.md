# OpenCode Handoff Todo List

Project: Copper Download Manager
Type: Qt 6 desktop app + browser extension (Chrome/Firefox)
Current version: 0.3.5

This list is ordered by impact and should be tackled in sequence for the next development pass.

## Priority 0: Stabilize the build and project health

- [ ] Verify the project builds cleanly in Debug and Release across the supported OS targets.
- [ ] Fix any compiler warnings, linker issues, and platform-specific build problems discovered during validation.
- [ ] Add a reproducible build/test workflow for CI (CMake, Qt path handling, packaging smoke checks).
- [ ] Document exact prerequisites for development environments (Qt version, toolchain, optional libtorrent support).

## Priority 1: Reliability and crash-safety of the app core

- [ ] Audit single-instance startup behavior and local IPC reliability for the embedded local server.
- [ ] Validate all forward-argument paths: HTTP URLs, magnet links, torrent files, and copper:// links.
- [ ] Review thread safety around download scheduling, state updates, and UI refresh callbacks.
- [ ] Harden startup/shutdown logic so partial initialization does not leave stale locks, sockets, or database state behind.
- [ ] Add automated regression tests for critical flows: launch with URL, already-running instance, minimized startup, and protocol link handling.

## Priority 2: Download engine and queue management

- [ ] Review the core download manager for queue ordering, retry logic, pause/resume correctness, and cancellation edge cases.
- [ ] Improve progress reporting accuracy during chunked downloads and multi-part transfers.
- [ ] Validate resume behavior across interruptions, file corruption, and partial download states.
- [ ] Verify speed limiter implementation and ensure it does not cause CPU or UI stalls.
- [ ] Review database persistence for download state and ensure recovery is consistent after app restart.

## Priority 3: Browser extension integration

- [ ] Confirm Chrome and Firefox extension behavior is aligned for interception, URL capture, and native app communication.
- [ ] Review permission usage in both extensions and remove any unnecessary access.
- [ ] Validate the desktop app receives intercepted link data reliably from the browser extension and handles it consistently.
- [ ] Improve error handling when the desktop app is not running or when the browser cannot reach it.
- [ ] Add manifest and compatibility checks for Firefox and Chrome release requirements.

## Priority 4: Protocol and desktop integration

- [ ] Formalize and test the copper:// protocol flow for open, create-download, and URL forwarding scenarios.
- [ ] Register and validate the desktop protocol association on Windows/macOS/Linux where appropriate.
- [ ] Ensure command-line arguments and protocol calls populate the same download intake path.
- [ ] Review default download directory selection and filename sanitization for user-provided URLs and custom save paths.

## Priority 5: Torrent and media features

- [ ] Validate torrent ingestion, magnet handling, and metadata parsing for real-world edge cases.
- [ ] Review aria2c integration and fallback behavior when tools are missing or fail to initialize.
- [ ] Audit yt-dlp integration for format selection, progress parsing, and error recovery.
- [ ] Review FFmpeg use for conversion/transcoding pipelines and failure handling.
- [ ] Improve user feedback for tool install/download progress and failed dependency setup.

## Priority 6: Data model and settings

- [ ] Audit DatabaseManager schema/versioning and migration behavior for upgrades.
- [ ] Review settings persistence and default values for theme, speed limits, local server port, and tool paths.
- [ ] Ensure settings changes apply immediately without leaving stale or invalid values in memory.
- [ ] Add validation for malformed or corrupted database rows and partial configuration values.

## Priority 7: UI/UX and product polish

- [ ] Review the main window, dialogs, and tray behavior for responsiveness and crash recovery.
- [ ] Improve the theme system and ensure dark/light/system modes are consistent.
- [ ] Validate Add URL, Download Manager, Settings, and About flows from a user perspective.
- [ ] Clean up inconsistent dialogs and status messaging for failed downloads, tool installation, and config changes.
- [ ] Improve accessibility and keyboard navigation in key dialogs and actions.

## Priority 8: Packaging, distribution, and release readiness

- [ ] Finalize release packaging for Windows and other target platforms.
- [ ] Validate installer/portable output structure and dependency deployment.
- [ ] Ensure licensing and third-party notices are included correctly in every release package.
- [ ] Add a release checklist for QA before publishing new versions.

## Priority 9: Hardening and security review

- [ ] Audit remote URL handling to reduce risks from untrusted sources and malformed data.
- [ ] Review browser extension messaging to ensure safe parameter validation and minimal trust assumptions.
- [ ] Check local HTTP server exposure and API contract for unexpected requests or abuse.
- [ ] Review downloaded tool management and external process execution for command injection or path issues.

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
