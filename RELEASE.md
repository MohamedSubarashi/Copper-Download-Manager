# Copper Download Manager — Release Guide & QA Checklist

This document describes how to produce and validate a release build before
publishing a new version. Follow it top to bottom.

## 1. Bump the version

1. Update `project(... VERSION X.Y.Z ...)` in `CMakeLists.txt`.
2. Update `APP_VERSION` (if referenced) and the version resource / installer
   metadata.
3. Update the header version in `THIRD-PARTY-NOTICES.txt` and verify Qt/tool
   versions match what will be deployed.

## 2. Build (Windows Release)

```powershell
# From the build tree (space-free path preferred)
cmd /c "set PATH=C:\Qt\Tools\mingw1310_64\bin;%PATH% && C:\Qt\Tools\Ninja\ninja.exe -C rel"
```

Notes:

- MinGW bin must be on `PATH` before compiling, otherwise `cc1plus` fails with
  `STATUS_DLL_NOT_FOUND (0xC0000135)`.
- `windeployqt --release --no-translations --no-opengl-sw` runs as part of the
  install step and lays out Qt DLLs + plugin folders into
  `installer/release/<version>/`.
- `THIRD-PARTY-NOTICES.txt` is copied into the release folder automatically.
- `copper_native_host.exe` (the browser native-messaging host) is built and
  copied into the release folder automatically as a second CMake target.

After a successful build, deploy the freshly built exe(s) over the release copy:

```powershell
Copy-Item -Force "rel\CopperDownloadManager.exe" "installer\release\<version>\CopperDownloadManager.exe"
Copy-Item -Force "rel\copper_native_host.exe" "installer\release\<version>\copper_native_host.exe"
```

## 3. Automated regression suite

Run the full integration suite against the deployed exe:

```powershell
python tests\integration_test.py "installer\release\<version>\CopperDownloadManager.exe"
```

Expected result: `N passed, 0 failed` (35 cases as of v0.3.7). The suite covers
launch/intake, single-instance, protocol forwarding, chunked/truncated/
unknown-length downloads, the copper:// flow, .torrent injection, the
native-messaging host -> named-pipe injection (ping + byte-exact download), and
auto-resume of an interrupted HTTP download after an app restart.

Extension manifest validation (offline, run in CI and locally):

```powershell
python tests\validate_extensions.py
```
Validates the MV3 shape, popup wiring, Firefox gecko.id + data-collection
declaration, and that both extensions use `sendNativeMessage` (no copper:// or
localhost ping).

## 4. Manual QA checklist

Run through these before publishing.

### Native-messaging injection (extension → desktop)
- [ ] Firefox (registered on app startup using stable gecko.id
      `copper-download-manager@copper`): context-menu "Download link with Copper"
      sends the URL to the app over the named pipe and starts a download.
- [ ] Chrome (dev/unpacked): register the loaded extension's `runtime.id` via
      `--register-native-extension chrome <id>` or the in-extension register
      handshake, then confirm the host manifest `allowed_origins` includes it and
      injection works.
- [ ] When Copper is closed, the host launches it automatically and the link is
      still injected.
- [ ] `copper_native_host.exe` sits in `<version>/` next to the app exe and the
      Firefox manifest `path` points at it.


### Core downloads
- [ ] Add a plain HTTP download; confirm chunked multi-connection progress and
      a byte-exact result.
- [ ] Pause, resume, and cancel a download; confirm the partial file is not
      miscounted as Completed and cancel leaves the item as Cancelled.
- [ ] Kill the network mid-download; confirm truncated transfer is detected and
      the item ends Failed (not Completed).
- [ ] Download a file with no Content-Length; confirm it reaches 100%.
- [ ] Restart the app; confirm download state is persisted and recovery is
      consistent.

### Video / yt-dlp / ffmpeg
- [ ] Video URL with yt-dlp present: fetch file list, select entries, download.
- [ ] Video URL with yt-dlp missing: dialog shows a clear install hint, and a
      missing-tool error points to Settings > Tools.
- [ ] mp3/mp4 (merge) formats: confirm ffmpeg pre-flight check runs before the
      merge/extract step.
- [ ] Playlist: confirm track-number naming (001, 002...) works.

### Torrents / aria2c
- [ ] .torrent file: parse file list and start download.
- [ ] magnet link: starts and polls without stale polling or orphaned process.
- [ ] Torrent Details: stats labels (peers, seeds, download/upload, ratio) and
      the peer table update while live.

### Browser extension → desktop
- [ ] Chrome: toolbar popup opens; intercepted links reach the app via the host.
- [ ] Firefox: same flow works (add-on is signed / identified by gecko.id).

### Settings & protocol
- [ ] Change theme (Light/Dark/System) and confirm it applies immediately.
- [ ] Speed limiter applies immediately and does not stall the UI.
- [ ] `copper://` base key + shell command rebuild on startup even after a
      moved/partial install.

### Accessibility (basic smoke)
- [ ] Tab order is logical in Settings, Download Manager, and Torrent Details.
- [ ] Labels are associated with their controls (buddy + accessible name) so a
      screen reader announces them correctly.
- [ ] Dialogs can be dismissed/confirmed via keyboard (OK default, Esc cancel).

## 5. Packaging checks

- [ ] `installer/release/<version>/` contains the exe, `copper_native_host.exe`,
      all Qt DLLs and plugin folders (platforms, imageformats, iconengines,
      styles, sqldrivers, tls, networkinformation), and
      `THIRD-PARTY-NOTICES.txt`.
- [ ] MinGW runtime DLLs (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`,
      `libwinpthread-1.dll`) are present and covered by the notices file.
- [ ] Create the portable zip and confirm CI packaging smoke check passes.
- [ ] `THIRD-PARTY-NOTICES.txt` lists every bundled third-party component with
      license, copyright, and source.

## 6. Publish

1. Commit all changes, tag the release (`git tag vX.Y.Z`), and push.
2. Let CI run the matrix (Qt 6.6.3 Release/Debug) and the `extension-lint` job;
   confirm all green.
3. Attach the portable artifact from CI (or the locally built zip) to the
   release.
4. Update the in-app changelog/version and the README if the feature set
   changed.
