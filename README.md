# Copper Download Manager

A high-performance, cross-platform download manager written in C++17 with Qt6.

## Features

- **Multi-chunk HTTP downloads** — Split files into multiple chunks for maximum speed
- **Torrent & Magnet support** — Via aria2c integration
- **YouTube/video downloads** — Via yt-dlp integration with format selection
- **Browser extension** — Chrome & Firefox extensions to intercept downloads automatically
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
- **CMake 3.16+** (validated on **3.30.5**)
- **Ninja** build system
- **C++17 compiler** — MinGW/GCC (validated on **GCC 13.1.0**, `x86_64-posix-seh`); also MSVC 2019+ / Clang 10+
- **Optional:** `libtorrent-rasterbar` (torrent support; otherwise aria2c is used)

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

> **Space-in-path caveat:** `app.rc` (Windows version/icon resource) is skipped when the source path contains spaces (e.g. `Copper Download Manager`) to avoid a MinGW `windres` preprocessing failure. The app still reports its version at runtime via `main.cpp`.

The release build is automatically deployed to `installer/release/<version>/` (windeployqt) and the final package (Setup.exe + portable zip) is placed in `Release/`.

## CI

A GitHub Actions workflow (`.github/workflows/ci.yml`) builds the project in Debug and Release across Qt 6.6.3 and 6.11.2 on Windows, and smoke-checks that the produced executable is a GUI (non-console) application.

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

## Author

**Mohamed Subarashi** — [GitHub](https://github.com/MohamedSubarashi) · [Ko-fi](https://ko-fi.com/mohamedsubarashi)
