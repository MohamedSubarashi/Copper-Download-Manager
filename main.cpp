#include "ui/MainWindow.h"
#include "ui/DownloadManagerDialog.h"
#include "utils/ThemeManager.h"
#include "utils/Logger.h"
#include "utils/DefaultHandler.h"
#include "utils/CopperLink.h"
#include "utils/FileNameSanitizer.h"
#include "utils/UrlDetector.h"
#include "utils/Aria2cManager.h"
#include "db/DatabaseManager.h"
#include "core/LocalServer.h"
#include "core/PipeServer.h"
#include "core/DownloadManager.h"
#include "utils/NativeMessaging.h"
#include <QApplication>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// --- Crash backtrace logging ----------------------------------------------
// Hooks the top-level exception filter so any unhandled access violation logs a
// module+offset backtrace to copper.log before Windows terminates the process.
// This lets us capture the exact (module, RVA) of the crashing call chain for
// hard-to-reproduce faults (e.g. the torrent-add access violation) without a
// debugger. Uses only kernel32/ntdll so no extra link libraries are needed.
#ifdef _WIN32
#include <windows.h>
extern "C" typedef USHORT (WINAPI *RtlCaptureStackBackTraceFn)(ULONG, ULONG, PVOID*, PULONG);

namespace {

void crashLog(const char* msg) {
    const char* path = getenv("APPDATA");
    if (!path) return;
    char full[1024];
    std::snprintf(full, sizeof(full), "%s\\Copper\\Copper Download Manager\\copper.log", path);
    FILE* f = fopen(full, "a");
    if (!f) return;
    fprintf(f, "[CRASH] %s\n", msg);
    fclose(f);
}

void crashResolveFrame(void* addr) {
    // Resolve module base + RVA using only kernel32 APIs (no dbghelp link lib).
    char baseName[128] = "<unknown>";
    void* base = addr;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.AllocationBase) {
        base = mbi.AllocationBase;
        char fullPath[512];
        DWORD got = GetModuleFileNameA((HMODULE)mbi.AllocationBase, fullPath, sizeof(fullPath));
        if (got > 0 && got < sizeof(fullPath)) {
            const char* slash = strrchr(fullPath, '\\');
            const char* nm = slash ? slash + 1 : fullPath;
            std::snprintf(baseName, sizeof(baseName), "%s", nm);
        }
    }
    char out[512];
    std::snprintf(out, sizeof(out), "    %s+0x%llx", baseName,
                  (unsigned long long)((unsigned char*)addr - (unsigned char*)base));
    crashLog(out);
}

LONG WINAPI TopLevelExceptionFilter(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    char hdr[256];
    std::snprintf(hdr, sizeof(hdr), "Unhandled exception: code=0x%08lx at 0x%p",
                  (unsigned long)er->ExceptionCode, (void*)er->ExceptionAddress);
    crashLog(hdr);

    if (er->ExceptionCode == 0xC0000005 && er->NumberParameters >= 2) {
        char av[256];
        std::snprintf(av, sizeof(av),
                      "Access violation: type=0x%llx address=0x%llx (call target 0x%llx)",
                      (unsigned long long)er->ExceptionInformation[0],
                      (unsigned long long)er->ExceptionInformation[1],
                      (unsigned long long)er->ExceptionAddress);
        crashLog(av);
    }

    // Capture the backtrace of the faulting thread from within the filter.
    static RtlCaptureStackBackTraceFn RtlCaptureStackBackTrace =
        (RtlCaptureStackBackTraceFn)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                                   "RtlCaptureStackBackTrace");
    crashLog("Backtrace (module+offset):");
    if (RtlCaptureStackBackTrace) {
        void* frames[48] = {};
        USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        for (USHORT i = 0; i < n; i++) crashResolveFrame(frames[i]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace
#endif // _WIN32

static int localApiPort() {
    return DatabaseManager::instance().getSetting("localServerPort", "24680").toInt();
}

static bool isRunningInstance() {
    int port = localApiPort();
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", port);
    if (!socket.waitForConnected(2000)) return false;
    QString host = "127.0.0.1:" + QString::number(port);
    socket.write("GET /api/ping HTTP/1.1\r\nHost: " + host.toUtf8() + "\r\nConnection: close\r\n\r\n");
    socket.waitForBytesWritten(2000);
    socket.waitForReadyRead(2000);
    QByteArray response = socket.readAll();
    socket.close();
    return response.contains("200 OK");
}

static bool sendToRunningInstance(const QString& arg) {
    int port = localApiPort();
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", port);
    if (!socket.waitForConnected(2000)) return false;

    QJsonObject obj;
    obj["action"] = "open";
    obj["argument"] = arg;
    QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QString host = "127.0.0.1:" + QString::number(port);

    QByteArray request = "POST /api/forward HTTP/1.1\r\n"
                         "Host: " + host.toUtf8() + "\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                         "Connection: close\r\n"
                         "\r\n" + body;

    socket.write(request);
    socket.waitForBytesWritten(3000);
    socket.waitForReadyRead(3000);
    socket.close();
    return true;
}

static bool sendShowToRunningInstance() {
    int port = localApiPort();
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", port);
    if (!socket.waitForConnected(2000)) return false;

    QJsonObject obj;
    obj["argument"] = "show";
    QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QString host = "127.0.0.1:" + QString::number(port);

    QByteArray request = "POST /api/forward HTTP/1.1\r\n"
                         "Host: " + host.toUtf8() + "\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                         "Connection: close\r\n"
                         "\r\n" + body;

    socket.write(request);
    socket.waitForBytesWritten(3000);
    socket.waitForReadyRead(3000);
    socket.close();
    return true;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(TopLevelExceptionFilter);
#endif

    QApplication app(argc, argv);
    app.setApplicationName("Copper Download Manager");
    app.setApplicationVersion("0.7.0");
    app.setOrganizationName("Copper");

    Logger::instance().info("========================================");
    Logger::instance().info("Copper Download Manager v" + app.applicationVersion());
    Logger::instance().info("========================================");

    DatabaseManager::instance().init();

    DefaultHandler::instance().autoUpdateRegistryPath();

    QString theme = DatabaseManager::instance().getSetting("theme", "System");
    ThemeManager::instance().applyTheme(ThemeManager::instance().stringToTheme(theme));
    Logger::instance().info("Theme applied: " + theme);

    int speedLimit = DatabaseManager::instance().getSetting("speedLimit", "0").toInt();
    if (speedLimit > 0) {
        DownloadManager::instance().setSpeedLimit((qint64)speedLimit * 1024);
        Logger::instance().info("Speed limit set: " + QString::number(speedLimit) + " KB/s");
    }

    bool startMinimized = false;
    bool hasUrlArg = false;
    QStringList forwardArgs;

    QString regBrowser;
    QString regExtensionId;

    for (int i = 1; i < argc; i++) {
        QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--minimized") {
            startMinimized = true;
            continue;
        }
        if (arg == "--register-native-extension" && (i + 2) < argc) {
            regBrowser = QString::fromLocal8Bit(argv[i + 1]);
            regExtensionId = QString::fromLocal8Bit(argv[i + 2]);
            i += 2;
            hasUrlArg = true;
            continue;
        }
        hasUrlArg = true;
        forwardArgs.append(arg);
    }

    // Handle --register-native-extension <browser> <id> even if an instance is
    // already running (write the manifest and exit without opening the GUI).
    if (!regBrowser.isEmpty() && !regExtensionId.isEmpty()) {
        NativeMessaging::writeHostConfig();
        QString b = regBrowser.toLower();
        if (b == "firefox") {
            NativeMessaging::installManifest("firefox", {regExtensionId});
        } else {
            NativeMessaging::installManifest("chrome", {regExtensionId});
            if (b == "edge") NativeMessaging::installManifest("edge", {regExtensionId});
        }
        Logger::instance().info("Registered native-messaging extension '" + regExtensionId +
                                "' for browser '" + regBrowser + "'");
        return 0;
    }

    if (isRunningInstance()) {
        if (hasUrlArg) {
            int forwarded = 0;
            for (const QString& arg : forwardArgs) {
                if (sendToRunningInstance(arg)) {
                    forwarded++;
                    Logger::instance().info("Forwarded to running instance: " + arg.left(80));
                }
            }
            if (forwarded > 0) {
                Logger::instance().info("Forwarded " + QString::number(forwarded) + " argument(s) to existing instance, exiting");
            } else {
                Logger::instance().warning("Instance already running but argument forwarding failed, exiting");
            }
        } else {
            sendShowToRunningInstance();
            Logger::instance().info("Instance already running, activating existing window, exiting");
        }
        return 0;
    }

    int port = DatabaseManager::instance().getSetting("localServerPort", "24680").toInt();
    if (LocalServer::instance().start(port)) {
        Logger::instance().info("Local API server started on port " + QString::number(port));
    } else {
        Logger::instance().warning("Failed to start local API server on port " + QString::number(port));
    }

    // Named-pipe intake for the browser native-messaging host (IDM model).
    if (PipeServer::instance().start()) {
        Logger::instance().info("Native messaging intake (named pipe) started");
    } else {
        Logger::instance().warning("Failed to start named pipe intake");
    }

    // Register the Firefox host manifest on startup (its gecko.id is stable and
    // known ahead of time). Chrome/Edge is registered when a specific extension
    // id reports in via the "register" handshake, since its id is load-dependent.
    NativeMessaging::installManifest("firefox", {NativeMessaging::firefoxExtensionId()});
    NativeMessaging::writeHostConfig();

    QStringList pendingTorrentFiles;
    QStringList pendingMagnetLinks;
    QStringList pendingHttpUrls;
    QList<QPair<QString, QString>> pendingCopperDownloads;

    for (const QString& arg : forwardArgs) {
        if (arg.startsWith("copper://")) {
            Logger::instance().info("Protocol URL received: " + arg);
            QString path = arg.mid(QString("copper://").size());
            if (path == "open") {
                Logger::instance().info("copper://open received, showing main window");
            } else {
                CopperLink cl = parseCopperLink(arg);
                if (cl.valid) {
                    Logger::instance().info("Copper download injection: " + cl.url.left(80));
                    if (cl.url.startsWith("magnet:?")) {
                        pendingMagnetLinks.append(cl.url);
                    } else {
                        QString fullSavePath = cl.path;
                        if (!fullSavePath.isEmpty() && !cl.filename.isEmpty()) {
                            QString safeName = sanitizeFileName(cl.filename);
                            if (!fullSavePath.endsWith('/') && !fullSavePath.endsWith('\\')) fullSavePath += "/";
                            fullSavePath += safeName;
                        } else if (cl.path.isEmpty()) {
                            fullSavePath.clear(); // use default download dir
                        }
                        pendingCopperDownloads.append({cl.url, fullSavePath});
                    }
                } else {
                    Logger::instance().warning("Unhandled copper:// URL: " + arg);
                }
            }
            continue;
        }
        if (arg.startsWith("magnet:")) {
            Logger::instance().info("Magnet link received: " + arg.left(80) + "...");
            pendingMagnetLinks.append(arg);
            continue;
        }
        if (arg.startsWith("http://") || arg.startsWith("https://") || arg.startsWith("ftp://")) {
            Logger::instance().info("URL argument received: " + arg);
            pendingHttpUrls.append(arg);
            continue;
        }

        QString cleaned = arg;
        if (cleaned.startsWith("\"") && cleaned.endsWith("\"")) cleaned = cleaned.mid(1, cleaned.length() - 2);
        cleaned = QDir::fromNativeSeparators(cleaned);
        QFileInfo fi(cleaned);
        if (fi.exists() && fi.suffix().toLower() == "torrent") {
            Logger::instance().info("Torrent file argument received: " + fi.absoluteFilePath());
            pendingTorrentFiles.append(fi.absoluteFilePath());
        } else {
            Logger::instance().warning("Unknown argument: " + arg);
        }
    }

    MainWindow w;
    if (startMinimized && forwardArgs.isEmpty()) {
        w.showMinimized();
    } else {
        w.show();
    }

    if (!forwardArgs.isEmpty()) {
        w.raise();
        w.activateWindow();
    }

    for (const QString& torrentFile : pendingTorrentFiles) {
        QString savePath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        DownloadManagerDialog dialog(SourceTorrent, torrentFile, savePath, &w);
        if (dialog.exec() == QDialog::Accepted) {
            QVector<PlaylistEntry> selected = dialog.getSelectedEntries();
            QString outputPath = dialog.getOutputPath();
            bool useTracks = dialog.getUseTrackNumbers();
            QString fmt = dialog.getAudioFormat();
            QString torrentName = dialog.getTorrentName();
            if (!selected.isEmpty()) {
                DownloadManager::instance().addPlaylistDownload(selected, outputPath, "Torrent", useTracks, fmt, torrentFile, torrentName);
            } else {
                DownloadManager::instance().addDownload(torrentFile, outputPath, "Torrent");
            }
        }
    }

    for (const QString& magnetLink : pendingMagnetLinks) {
        QString savePath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        DownloadManagerDialog dialog(SourceTorrent, magnetLink, savePath, &w);
        if (dialog.exec() == QDialog::Accepted) {
            QVector<PlaylistEntry> selected = dialog.getSelectedEntries();
            QString outputPath = dialog.getOutputPath();
            bool useTracks = dialog.getUseTrackNumbers();
            QString fmt = dialog.getAudioFormat();
            QString torrentName = dialog.getTorrentName();
            if (!selected.isEmpty()) {
                DownloadManager::instance().addPlaylistDownload(selected, outputPath, "Torrent", useTracks, fmt, magnetLink, torrentName);
            } else {
                DownloadManager::instance().addDownload(magnetLink, outputPath, "Torrent");
            }
        }
    }

    for (const QString& url : pendingHttpUrls) {
        UrlType detected = UrlDetector::detect(url);
        if (detected == UrlPlaylist || detected == UrlYtDlp) {
            QString savePath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
            DownloadManagerDialog dialog(SourceVideo, url, savePath, &w);
            if (dialog.exec() == QDialog::Accepted) {
                QVector<PlaylistEntry> selected = dialog.getSelectedEntries();
                QString outputPath = dialog.getOutputPath();
                bool useTracks = dialog.getUseTrackNumbers();
                QString fmt = dialog.getAudioFormat();
                if (!selected.isEmpty()) {
                    DownloadManager::instance().addPlaylistDownload(selected, outputPath, "YtDlp", useTracks, fmt);
                } else {
                    DownloadManager::instance().addDownload(url, outputPath, "YtDlp");
                }
            }
        } else {
            DownloadManager::instance().addDownload(url, "", "HTTP");
        }
    }

    for (const QPair<QString, QString>& pair : pendingCopperDownloads) {
        const QString& url = pair.first;
        const QString& savePath = pair.second;
        if (UrlDetector::isYtDlpUrl(url) || UrlDetector::isPlaylistUrl(url)) {
            DownloadManager::instance().addDownload(url, "", "YtDlp");
        } else {
            DownloadManager::instance().addDownload(url, savePath, "HTTP");
        }
    }

    Logger::instance().info("Application started successfully");

    int result = app.exec();

    Logger::instance().info("Application shutting down");

    // Stop all in-flight downloads and spawned processes (yt-dlp/ffmpeg/aria2c)
    // before the local server and daemon teardown so nothing is left orphaned.
    DownloadManager::instance().shutdown();
    LocalServer::instance().stop();
    Aria2cManager::instance().shutdownDaemon();

    return result;
}
