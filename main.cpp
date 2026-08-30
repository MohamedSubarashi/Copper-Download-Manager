#include "ui/MainWindow.h"
#include "ui/DownloadManagerDialog.h"
#include "utils/ThemeManager.h"
#include "utils/Logger.h"
#include "utils/DefaultHandler.h"
#include "utils/CopperLink.h"
#include "utils/Aria2cManager.h"
#include "db/DatabaseManager.h"
#include "core/LocalServer.h"
#include "core/DownloadManager.h"
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

static bool isRunningInstance() {
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", 24680);
    if (!socket.waitForConnected(2000)) return false;
    socket.write("GET /api/ping HTTP/1.1\r\nHost: 127.0.0.1:24680\r\nConnection: close\r\n\r\n");
    socket.waitForBytesWritten(2000);
    socket.waitForReadyRead(2000);
    QByteArray response = socket.readAll();
    socket.close();
    return response.contains("200 OK");
}

static bool sendToRunningInstance(const QString& arg) {
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", 24680);
    if (!socket.waitForConnected(2000)) return false;

    QJsonObject obj;
    obj["action"] = "open";
    obj["argument"] = arg;
    QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);

    QByteArray request = "POST /api/forward HTTP/1.1\r\n"
                         "Host: 127.0.0.1:24680\r\n"
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
    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", 24680);
    if (!socket.waitForConnected(2000)) return false;

    QJsonObject obj;
    obj["argument"] = "show";
    QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);

    QByteArray request = "POST /api/forward HTTP/1.1\r\n"
                         "Host: 127.0.0.1:24680\r\n"
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
    QApplication app(argc, argv);
    app.setApplicationName("Copper Download Manager");
    app.setApplicationVersion("0.3.5");
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

    for (int i = 1; i < argc; i++) {
        QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--minimized") {
            startMinimized = true;
            continue;
        }
        hasUrlArg = true;
        forwardArgs.append(arg);
    }

    if (isRunningInstance()) {
        if (hasUrlArg) {
            bool forwarded = false;
            for (const QString& arg : forwardArgs) {
                if (sendToRunningInstance(arg)) {
                    forwarded = true;
                    Logger::instance().info("Forwarded to running instance: " + arg.left(80));
                }
            }
            if (!forwarded) {
                Logger::instance().info("Forwarded arguments to existing instance, exiting");
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
                            if (!fullSavePath.endsWith('/') && !fullSavePath.endsWith('\\')) fullSavePath += "/";
                            fullSavePath += cl.filename;
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
        if (url.contains("youtube.com") || url.contains("youtu.be") ||
            url.contains("soundcloud.com") || url.contains("vimeo.com")) {
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
        if (url.contains("youtube.com") || url.contains("youtu.be") ||
            url.contains("soundcloud.com") || url.contains("vimeo.com")) {
            DownloadManager::instance().addDownload(url, "", "YtDlp");
        } else {
            DownloadManager::instance().addDownload(url, savePath, "HTTP");
        }
    }

    Logger::instance().info("Application started successfully");

    int result = app.exec();

    LocalServer::instance().stop();
    Aria2cManager::instance().shutdownDaemon();
    Logger::instance().info("Application shutting down");

    return result;
}
