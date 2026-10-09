# Copper Download Manager 0.4.1

Patch release. Fixes YouTube (and other yt-dlp) downloads that produced a
video-only file plus a separate audio-only file instead of one merged file,
plus the related output-format bugs found while fixing it. No features were
removed and the interface is unchanged.

## What was wrong

Copper installs its own copy of ffmpeg into its private tools folder and
relies on it to mux the separate video and audio streams that yt-dlp
downloads. The tools folder is not on `PATH`, and Copper never told yt-dlp
where its ffmpeg was, so yt-dlp could not find it. It then did the only
thing it can do without ffmpeg: it skipped the merge and left **two files**
on disk. It still exited with success, so the download showed as completed.

## Fixes

- **yt-dlp is now told where ffmpeg is.** Every yt-dlp job passes
  `--ffmpeg-location <tools>/ffmpeg.exe`, and the tools folder is also added
  to the child process `PATH`. The separate video and audio streams are muxed
  into a single file as intended.
- **A missed merge is no longer silent.** If yt-dlp still reports that it
  cannot find ffmpeg, the download is failed with a clear message telling the
  user to install/reinstall ffmpeg in Settings → Tools, instead of being
  reported as a successful download that left two files behind.
- **MKV output actually produces MKV.** The "MKV (Video)" choice in the Add
  URL dialog was never mapped, so it silently produced mp4.
- **The chosen format is honored for single videos.** Adding a single video
  (or a `copper://` link) dropped the format selection and always used mp4.
- **`best` and `mkv` are accepted again.** The ffmpeg pre-flight check
  reported those two formats as "ffmpeg not required" and then rejected them
  outright, so they could never be downloaded even with ffmpeg installed.
- **ffprobe is retained on Windows.** The ffmpeg installer only kept
  `ffmpeg.exe`; `ffprobe.exe` is now kept too (yt-dlp uses it for stream
  inspection).
- **Linux/macOS ffmpeg install extracts again.** That path previously deleted
  the downloaded archive without extracting anything.

## Verification

- One real YouTube download through the app produced a single
  `Me at the zoo.mp4` whose `ffprobe` stream list is `video, audio` — not a
  separate video-only and audio-only file.
- `copper_tests`: 21 passed, 0 failed (4 new cases lock the yt-dlp argument
  building and the merge-format mapping).
- Integration suite: 99 passed, 0 failed.
- `tools/check_version_consistency.py`: version `0.4.1` consistent everywhere.

## Install

- **Installer**: `CopperDownloadManager-0.4.1-setup.exe`
- **Portable**: extract `CopperDownloadManager-portable-0.4.1.zip`
  anywhere and run `CopperDownloadManager.exe`
- Verify downloads with `SHA256SUMS` (`sha256sum -c SHA256SUMS`).

## Known limitations

- Binaries are not code-signed (no signing certificate available in this
  cycle); Windows SmartScreen may warn on first run.
- The macOS download from evermeet.cx ships only `ffmpeg` (no `ffprobe`);
  merging works, stream inspection falls back to ffmpeg.
