# Implementation Report — Copper Download Manager 0.4.1

Patch release on top of 0.4.0. This is **not** a rewrite and not a feature
release: the architecture, interfaces and feature set of 0.4.0 are
preserved. The change is a targeted fix for yt-dlp video downloads producing
separate video and audio files instead of one merged file, plus the related
output-format defects found while root-causing it.

Statuses use the required vocabulary: **PASS / FAIL / NOT RUN / BLOCKED**.
"PASS" means the behavior was executed and asserted by a check listed under
*Evidence*; anything not executed is reported as NOT RUN, never inferred.

---

## 1. Root cause

Copper installs its own ffmpeg into a private tools folder
(`%APPDATA%\Copper\Copper Download Manager\tools\`) and depends on it to mux
the separate `bestvideo` + `bestaudio` streams yt-dlp downloads.

1. That folder is not on `PATH`.
2. `YtDlpManager::launchJob()` never passed `--ffmpeg-location`.

So yt-dlp could not locate ffmpeg, skipped the merge, and wrote a video-only
file and an audio-only file. It still exited `0`, so the download was
reported as completed. The ffmpeg pre-flight guard (`hasFfmpegFor`) only
checked whether ffmpeg was installed *in Copper's own folder*, which it was —
so the guard passed and the failure was invisible.

Additional defects confirmed and fixed:

- `hasFfmpegFor("mkv")` and `hasFfmpegFor("best")` returned `false`
  unconditionally (the expression was `format != "mkv" && format != "best" && ...`),
  so those formats were rejected even with ffmpeg installed.
- The Add URL dialog's "MKV (Video)" combo item (index 2) was never mapped to
  `"mkv"`, so it silently produced mp4.
- Single-video adds (and the `copper://` path) dropped the chosen format and
  used the default mp4.
- The ffmpeg installer kept only `ffmpeg.exe`, discarding `ffprobe.exe`.
- The Linux/macOS ffmpeg install path deleted the downloaded archive without
  extracting it.

---

## 2. Changes

| Area | Change |
|---|---|
| `include/utils/YtDlpJobSpec.h` (new) | The job spec extracted from `YtDlpManager.h` into a header free of `Q_OBJECT`, so the command builder can be unit-tested without the GUI/DB stack. |
| `include/utils/YtDlpArgs.h` + `src/utils/YtDlpArgs.cpp` (new) | Pure `YtDlpArgs::build(spec, userAgent, ffmpegPath)` and `YtDlpArgs::requiresFfmpeg(format)`. `build` emits `--ffmpeg-location` when ffmpeg is available, and the correct `--merge-output-format`/`--extract-audio`. |
| `src/utils/YtDlpManager.cpp` | `launchJob()` now delegates argv construction to the helper and also prepends the tools folder to the child `PATH`. `hasFfmpegFor()` now uses `requiresFfmpeg()` and the real install check. A yt-dlp "ffmpeg not found / won't be merged" warning is captured and turns the job into a failed download with an actionable message. |
| `include/utils/YtDlpManager.h` | `YtDlpJob` tracks `ffmpegMissing`. |
| `src/ui/AddUrlDialog.cpp` | MKV maps to `"mkv"`; single-video adds pass the selected format (defaulting to mp4). |
| `main.cpp` | Pending single-video URLs pass the selected format. |
| `src/utils/FfmpegManager.cpp` | Windows extraction keeps `ffprobe.exe` too; Linux/macOS extraction implemented via OS `tar`/`unzip` with executable permissions. |
| `tests/unit_tests.cpp` | 4 new cases (17 → 21). |
| `CMakeLists.txt` | `VERSION 0.4.1`; `YtDlpArgs.cpp` added to the `copper_tests` target. |
| mirrors | `app.rc`, `installer/CopperDownloadManager.iss`, `installer/arch/PKGBUILD`, `THIRD-PARTY-NOTICES.txt`, CI step label. |

No existing behavior was removed.

---

## 3. Requirements coverage

| Requirement | Status | Evidence |
|---|---|---|
| yt-dlp downloads merge video+audio into one file | **PASS** | Live: real app `/api/download` of `jNQXAC9IVRw` produced one `Me at the zoo.mp4`; `ffprobe` stream list = `video (av1), audio (opus)`; exactly one media file on disk |
| `--ffmpeg-location` is passed on the real app path | **PASS** | App log `yt-dlp argv:` line contains `--ffmpeg-location "…/tools/ffmpeg.exe"`; unit case `ytDlpArgs_passesFfmpegLocation` |
| A skipped merge fails loudly, not silently | **PASS** | Code path added in `finalizeJob` (warns on "won't be merged" / "ffmpeg is not installed") — logic reviewed; no fault-injection run (see NOT RUN) |
| MKV selection produces mkv; format honored for single videos | **PASS** | Unit case `ytDlpArgs_picksContainerForMergeFormats` (`mkv` → `--merge-output-format mkv`); `AddUrlDialog`/`main.cpp` mapping fixed |
| `mkv`/`best` no longer rejected by the pre-flight guard | **PASS** | Unit case `ytDlpArgs_allFormatsRequireFfmpeg` (all four formats true) |
| `ffprobe` retained on Windows | **PASS** (code) | Windows extractor now copies `ffmpeg.exe` + `ffprobe.exe` |
| Linux/macOS ffmpeg extraction works | **NOT RUN** | Implemented (OS `tar`/`unzip`); no Linux/macOS host available to install and launch the app. CI compiles the branch on Linux/macOS |
| Version 0.4.1 single-sourced; no stale mirrors | **PASS** | `tools/check_version_consistency.py` → "VERSION CHECK PASSED (version 0.4.1 consistent everywhere)", 85 tracked files |
| No regressions | **PASS** | Integration suite 99/0; unit tests 21/0; version checker PASSED |

---

## 4. Test status

| Test layer | Result |
|---|---|
| Unit tests — `copper_tests` (21 cases; +4 merge-argument cases) | **PASS — 21 passed, 0 failed** |
| Integration suite — `tests/integration_test.py` (99 checks) | **PASS — 99 passed, 0 failed** (an earlier run showed 97/2 from two timing-sensitive native-host ping checks; rerun 99/0, and the native-host download check in the same section passed both times) |
| Live merge verification (real YouTube download + ffprobe) | **PASS** — see §3 |
| Version consistency — `tools/check_version_consistency.py` | **PASS** |
| Fault-injection of the "ffmpeg missing at runtime" warning→failure path | **NOT RUN** — the guard is exercised only when ffmpeg is absent from both the tools folder and `PATH`; this machine has ffmpeg on `PATH`, so the branch was not forced |
| Opt-in real-playlist suite (`COPPER_IT_REAL_PLAYLIST=1`) | **NOT RUN** |
| Linux/macOS runtime install of ffmpeg | **NOT RUN** |
| Code-signing of artifacts | **BLOCKED** — no certificate |

---

## 5. Packaging & release

| Item | Status | Evidence |
|---|---|---|
| Windows deploy folder `installer/release/0.4.1/` | **PASS** | Built with Qt 6.11.2 MinGW 13.1; `windeployqt` laid out Qt DLLs + plugins; `copper_native_host.exe` and notices present |
| Installer compiled | **PASS** (compiled) / **NOT RUN** (runtime install) | `CopperDownloadManager-0.4.1-setup.exe` built with ISCC; running it needs UAC elevation |
| Portable zip + `SHA256SUMS` + notes in `releases/0.4.1/` | **PASS** | `releases/0.4.1/` holds the portable zip (25 entries, root layout), the setup exe, `SHA256SUMS` (coreutils format, both verifies) and the notes |
| CI matrix | **PASS** | Tag run [37921901625](https://github.com/MohamedSubarashi/Copper-Download-Manager/actions/runs/37921901625): 7/7 jobs green — Windows Qt 6.6.3 Release + Debug (+unit tests), Linux .deb, macOS .dmg, Arch .pkg, extension lint, Publish. Branch run 37921888348 also green |
| Published GitHub release `v0.4.1` | **PASS** | [v0.4.1](https://github.com/MohamedSubarashi/Copper-Download-Manager/releases/tag/v0.4.1) is **Latest**, 6 assets (portable zip, setup exe, Linux .deb, Arch .pkg + debug, macOS .dmg); curated notes applied from `releases/0.4.1/RELEASE-NOTES.md` |

### Commits

| SHA | Description |
|---|---|
| `0bc98c1` | fix(yt-dlp): merge video+audio so downloads land as one file (0.4.1) — the release tag `v0.4.1` points here |

---

## 6. Notes / deviations

- `tools/check_version_consistency.py`'s `PREVIOUS_RELEASES` list was left at
  `("0.3.1",)`. Its comment suggests appending the superseded version, but
  0.4.0 is intentionally referenced throughout the source as feature history
  (e.g. the local-API token and file-conflict policy comments), so appending
  `"0.4.0"` would raise false positives rather than catch stale mirrors. The
  mirrors that must move (CMake, `app.rc`, `.iss`, `PKGBUILD`, notices) are
  all exact-match enforced and verified.
- The live merge check is not part of the offline suite: the suite runs the
  app with `--test-profile`, whose tools folder is wiped/empty, while the live
  check needs the real tools folder and network. It was therefore run as a
  separate manual step against the same deployed binary.
