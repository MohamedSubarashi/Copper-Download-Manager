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

- **Qt 6.5+** — LGPLv3
- **CMake 3.16+**
- **C++17 compiler** — MSVC 2019+, GCC 9+, Clang 10+

## Building

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\Qt\6.x.x\msvc2022_64
cmake --build build --config Release
```

The release build is automatically deployed to `releases/<version>/`.

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
