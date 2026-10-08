# Copper Download Manager — Release Guide & QA Checklist

This document describes how to produce and validate a release build before
publishing a new version. Follow it top to bottom.

## 1. Bump the version

1. Update `project(... VERSION X.Y.Z ...)` in `CMakeLists.txt`. This is the
   single source: the generated `copper_version.h`, the User-Agent and
   `setApplicationVersion` all derive from it.
2. Mirror the new version into `app.rc`,
   `installer/CopperDownloadManager.iss` and the `THIRD-PARTY-NOTICES.txt`
   header, then run the consistency checker — it fails (exit 1) on any stale
   version literal or hardcoded UA anywhere in the tracked tree:

   ```powershell
   python tools\check_version_consistency.py
   ```

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

Expected result: `88 passed, 0 failed` (as of v0.4.0). The suite covers
launch/intake, single-instance, protocol forwarding, chunked/truncated/
unknown-length downloads, the copper:// flow, .torrent injection, the
native-messaging host -> named-pipe injection (ping + byte-exact download),
auto-resume of an interrupted HTTP download after an app restart, and the
playlist job model (one folder job per playlist, one row per item carrying its
playlist position and video id, pause/resume/retry of the job) together with the
selection contract: the ticked positions are stored on the job, nothing outside
the selection is requested, an empty selection refuses to start, and files are
numbered over the selection. From 0.4.0 it additionally covers the
local-API token (401 without / 200 with, stable across restart), the
browser-origin allowlist, `GET /api/diagnostics`, byte-exact resume after
an interrupt, and crash-free relaunch.

Unit tests (QtTest) — run before the suite:

```powershell
& build\copper_tests.exe -o unit_tests.log,txt
Get-Content unit_tests.log
```

Expected result: `17 passed, 0 failed` (as of v0.4.0). The binary is
isolated from the real profile (QStandardPaths test mode plus
`APPDATA`/`LOCALAPPDATA` overrides), so it can run beside the app.

One case is opt-in, because the rest of the suite is offline and this one
downloads real videos from YouTube (about 140 MB):

```powershell
$env:COPPER_IT_REAL_PLAYLIST = "1"
python tests\integration_test.py "installer\release\<version>\CopperDownloadManager.exe"
```

Expected result: `95 passed, 0 failed` (88 offline checks + the 7
real-playlist checks; not executed during the 0.4.0 validation cycle).
It needs yt-dlp and ffmpeg in the app's
tools folder (`%APPDATA%\Copper\Copper Download Manager\tools\`) and network
access. It fetches a fixed playlist, ticks two of its items (positions 11 and
18), and asserts that exactly two files land on disk, named `001.` and `002.` in
tick order, with the rows pointing at them. Earlier runs leave their own job for
that playlist behind - the app resumes interrupted jobs at startup - so the case
parks those jobs first; two jobs writing one folder would make the result
unreadable.

Note: the suite leaves rows in the app database between runs. Tests that look a
job up by URL use a unique playlist id per run, so a leftover row from an
earlier run cannot be mistaken for a new one. The real-playlist cases look their
job up by id recorded before the request, plus the selection it carries, for the
same reason.

Extension manifest validation (offline, run in CI and locally):

```powershell
python tests\validate_extensions.py
```
Validates the MV3 shape, toolbar `action` + popup wiring, Firefox gecko.id +
data-collection declaration, the required `downloads` permission, the
`nativeMessaging` permission and `getToken` handshake that supply the local
API token, and that both extensions dispatch over
`http://127.0.0.1:24680/api/download` (with the `copper://` launch path).

## 4. Manual QA checklist

Run through these before publishing.

### Native-messaging injection (extension → desktop)
- [ ] Chrome: right-click "Download link with Copper" posts the URL to
      `http://127.0.0.1:24680/api/download` and a byte-exact download starts.
- [ ] Firefox: the identical HTTP path works (add-on identified by gecko.id).
- [ ] Started fresh (app closed): the extension opens `copper://open` to launch
      the app, retries the API call, and the download still lands.
- [ ] `copper_native_host.exe` sits in `<version>/` next to the app exe and the
      app-side pipe tests still pass (the extension itself no longer depends on
      the host).


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
- [ ] Playlist: select a *subset* of items in the picker and confirm only those
      files appear, numbered over the selection (001 = the first item you
      ticked, 002 = the second, ...), then check the files on disk carry the
      same numbers as their rows.
- [ ] Playlist: tick nothing and hit download; the job refuses with "Nothing is
      selected in this playlist" instead of fetching the whole playlist.
- [ ] Playlist: one folder row per item, each row advancing on its own while a
      single yt-dlp process runs (check Task Manager: exactly one yt-dlp).
- [ ] Playlist: pause mid-item, resume, and confirm the finished items stay
      Completed and the partial one continues instead of restarting.
- [ ] Playlist: kill the app mid-playlist, relaunch, and confirm the finished
      items stay Completed and only the remainder is fetched.
- [ ] Playlist: one unavailable item does not cancel the rest of the job.

### Download info window
- [ ] Right-click a row > Download Info (or double-click the row) opens a
      per-download window with live progress, size, speed, and time left.
- [ ] Pause / Resume / Retry / Cancel from that window drive the download.
- [ ] With "Open the download window automatically" enabled in Settings, each new
      download opens its own window; closing a window hides it and it can be
      reopened from the same menu entry.
- [ ] A playlist folder opens a single window listing its items.

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
- [ ] Stage `releases/<version>/`: the portable zip (CI-identical layout),
      the setup exe, `SHA256SUMS` (coreutils format, covering both artifacts)
      and the release notes; verify with `sha256sum -c SHA256SUMS`.
- [ ] Smoke the install/uninstall path on a clean machine or VM — a
      successful ISCC compile alone does not prove it.

## 6. Code signing (preparation)

No signing certificate was available for 0.4.0, so the artifacts are
unsigned and Windows SmartScreen may warn on first run. When a
certificate exists:

1. Register a sign tool with ISCC, e.g.
   `ISCC /S"CopperSign=sign tool /f cert.pfx /p <password> $f" installer\CopperDownloadManager.iss`,
   and uncomment `SignTool=` / `SignedUninstaller=yes` in the `.iss`
   `[Setup]` section.
2. Sign the portable exe separately:
   `signtool sign /f cert.pfx /td sha256 /fd sha256 /tr <timestamp-url> CopperDownloadManager.exe`
   (and `copper_native_host.exe`).
3. Regenerate `SHA256SUMS` after any (re-)signing.

## 7. Publish

1. Commit all changes, tag the release (`git tag vX.Y.Z`), and push.
2. Let CI run the matrix (Qt 6.6.3 Release/Debug) and the `extension-lint` job;
   confirm all green.
3. Attach the portable artifact from CI (or the locally built zip) to the
   release.
4. Update the in-app changelog/version and the README if the feature set
   changed.
