# Copper Download Manager

A high-performance, cross-platform download manager written in C++17 with Qt6.

## Features

- **Multi-chunk HTTP downloads** — Split files into multiple chunks for maximum speed
- **Torrent & Magnet support** — Via aria2c integration
- **YouTube/video downloads** — Via yt-dlp integration with format selection
- **Browser extension** — Chrome, Firefox & Edge extensions to intercept downloads automatically ([get the extension](#browser-extensions))
- **Playlist support** — Download entire playlists with track numbering
- **Speed limiter** — Set download/upload speed limits
- **Resume & pause** — Pause and resume downloads at any time
- **Single instance** — Multiple launches forward URLs to the running instance
- **System tray** — Minimize to tray with download progress

## Supported Platforms

| Platform | Status |
|----------|--------|
| Windows  | Supported |
| macOS    | Supported |
| Linux    | Supported |

## Requirements

- **Qt 6.5+** (validated on **Qt 6.11.2 MinGW 64-bit**) — LGPLv3
- **CMake 3.16+** (validated on **4.4.3**)
- **Ninja** build system
- **C++17 compiler** — MinGW/GCC (validated on **GCC 13.1.0**, `x86_64-posix-seh`); also MSVC 2019+ / Clang 10+
- **Optional:** `libtorrent-rasterbar` (torrent support; otherwise aria2c is used)

### Qt version matrix

| Qt version | Platform / toolchain | Validation |
|------------|----------------------|------------|
| 6.11.2 | Windows, MinGW 13.1 (local dev) | Full local validation: Release build, unit tests, 99-case integration suite |
| 6.6.3 | Windows MinGW 13.1, Ubuntu, macOS (CI) | CI: build (Debug + Release), unit tests, CRT/startup smoke checks, packaging |
| 6.5 – 6.10 | any supported toolchain | Expected to build (only Qt 6.5-era APIs are used); not part of the automated matrix |

Qt versions outside this table are untested; the code deliberately avoids APIs
newer than 6.5 so the matrix can widen without source changes.

> **Note:** Because the executable is linked as a GUI (Win32) application, it no longer opens a terminal/console log window on launch. Logs go only to `copper.log` in the app-data folder.

## Building (Windows, MinGW)

1. Install Qt + MinGW toolchain (e.g. `C:\Qt\6.11.2\mingw_64` with `C:\Qt\Tools\mingw1310_64`).
2. Ensure `gcc`, `ninja`, and `cmake` are on `PATH`.
3. Configure and build (use a **space-free** build directory):

```bash
cmake -S . -B C:\Qt\Builds\CopperDownloadManager\rel -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH=C:/Qt/6.11.2/mingw_64
cmake --build C:\Qt\Builds\CopperDownloadManager\rel
```

> **Windows version/icon resource:** `app.rc` is compiled from a copy inside the
> build tree by `windres` (invoked directly, with no include flags, so the
> source path may contain spaces). The resulting `.rsrc` object is then
> normalized by `tools/fix_version_resource.py` to fix a binutils 2.39 `windres`
> quirk that otherwise makes the version invisible to Windows; the exe reliably
> reports `FileVersion`/`ProductVersion` in Explorer and via
> `GetFileVersionInfo`.

The release build is automatically deployed to `installer/release/<version>/` (windeployqt) and the final package (Setup.exe + portable zip) is placed in `Release/`.

## CI

A GitHub Actions workflow (`.github/workflows/ci.yml`) builds the project in Debug and Release on Windows (Qt 6.6.3), smoke-checks that the produced executable is a GUI (non-console) application, and uploads a portable zip artifact. (CI uses the Qt version whose MinGW binaries link against the runner's preinstalled GCC; the local dev machine additionally uses Qt 6.11.2.)

## Testing

Three layers, all wired into CI where the runner supports them:

1. **Unit tests** (`copper_tests`, QtTest) — the dependency-light utilities:
   dependency-binary verification (fail-closed hash/URL contract), file-name
   sanitization, URL intake classification, version/User-Agent single-source,
   and the local API token. Build with the project (`COPPER_BUILD_TESTS=ON`,
   default) and run `ctest --test-dir build` or `build/copper_tests` directly.
   The binary never touches the real profile (QStandardPaths test mode +
   `APPDATA`/`LOCALAPPDATA` overrides).

2. **Integration suite** (`tests/integration_test.py`) — launches the deployed
   exe on an isolated profile against local HTTP servers and asserts real
   behavior end to end: add/pause/resume/cancel/retry/queue, byte-exact
   resume, range validation, the token-gated local API and origin allowlist,
   native-messaging pipe intake, torrent/aria2 flows, `/api/diagnostics`, and
   crash-free relaunch. Run:

   ```bash
   $env:COPPER_EXE = (Resolve-Path "installer/release/<version>/CopperDownloadManager.exe").Path
   python tests/integration_test.py
   ```

3. **Extension lint** (`tests/validate_extensions.py`) — Chrome/Firefox
   manifest and background-script invariants.

## External Tools (Downloaded on Demand)

Copper downloads these tools when the user requests them from Settings → Tools:

| Tool | Version | License | Purpose |
|------|---------|---------|---------|
| aria2c | 1.37.0 | GPLv2 | Torrent/magnet downloads |
| FFmpeg | Latest | GPL v2+ | Audio/video processing |
| yt-dlp | 2026.07.01 | Unlicense | Video/audio downloading |

## Project Structure

```
Copper Download Manager/
├── CMakeLists.txt          # Build configuration
├── main.cpp                # Entry point, single-instance logic
├── app.rc                  # Windows resource file (icon, version)
├── THIRD-PARTY-NOTICES.txt # License notices for all dependencies
├── Assets/                 # Application icons
├── extensions/             # Browser extensions (Chrome & Firefox)
│   ├── Copper Download Manager Chrome/
│   └── Copper Download Manager Firefox/
├── include/
│   ├── core/               # Core download engine headers
│   ├── db/                 # Database manager header
│   ├── ui/                 # UI headers
│   └── utils/              # Utility headers (aria2c, ffmpeg, yt-dlp)
└── src/
    ├── core/               # Download engine implementation
    ├── db/                 # SQLite database
    ├── ui/                 # Qt UI (MainWindow, Settings, dialogs)
    └── utils/              # External tool managers
```

## License

Copper Download Manager is freeware. See [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt) for third-party license details.

## Browser Extensions

The extension intercepts downloads in the browser and hands them to the running
Copper Download Manager app. It is free and open source.

| Browser | Store | Status |
|----------|-------|--------|
| Firefox | [addons.mozilla.org](https://addons.mozilla.org/en-US/firefox/addon/copper-download-manager) | **Available** |
| Chrome | [chromewebstore.google.com](https://chromewebstore.google.com/detail/copper-download-manager/cmokehdedogdpbfhlamopeifgogaihni) | **Available** |
| Edge | [microsoftedge.microsoft.com](https://microsoftedge.microsoft.com/addons/detail/copper-download-manager/fnganciafgifhelimkoeiiapdcdmnnlb) | **Available** |
| Opera | — | Coming soon |

> Opera is Chromium-based, so the Chrome build can be sideloaded in developer
> mode until its store listing goes live.

## Author

**Mohamed Subarashi** — [GitHub](https://github.com/MohamedSubarashi) · [Ko-fi](https://ko-fi.com/mohamedsubarashi)
