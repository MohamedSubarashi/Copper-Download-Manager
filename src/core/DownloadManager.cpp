#include "core/DownloadManager.h"
#include "core/ChunkedDownloader.h"
#include "utils/Logger.h"
#include "utils/TrackNumber.h"
#include "utils/Aria2cManager.h"
#include "utils/YtDlpManager.h"
#include "utils/UrlDetector.h"
#include "utils/FileNameSanitizer.h"
#include "db/DatabaseManager.h"
#include <QFileInfo>
#include <QUrl>
#include <QStandardPaths>
#include <QDateTime>
#include <QDir>
#include <QProcess>
#include <QRegularExpression>


DownloadManager::DownloadManager() : nextId(1), maxConcurrent(5), speedLimit(0), speedLimitAccumulator(0) {
    speedLimitTimer = new QTimer(this);
    connect(speedLimitTimer, &QTimer::timeout, this, &DownloadManager::processSpeedLimit);
    speedLimitTimer->start(100);

    int maxId = DatabaseManager::instance().getMaxDownloadId();
    if (maxId >= nextId) {
        nextId = maxId + 1;
    }

    connect(&YtDlpManager::instance(), &YtDlpManager::downloadProgress, this, &DownloadManager::onYtDlpProgress);
    connect(&YtDlpManager::instance(), &YtDlpManager::downloadFinished, this, &DownloadManager::onYtDlpFinished);
    connect(&YtDlpManager::instance(), &YtDlpManager::downloadFailed, this, &DownloadManager::onYtDlpFailed);

    // Restore interrupted downloads once the event loop is running (after the
    // window and its signal connections exist) so resumed transfers show up in
    // the UI. Previously resetStaleDownloads() blindly marked every in-flight
    // item as Failed, which prevented any resume after a restart.
    QTimer::singleShot(0, this, &DownloadManager::restoreFromDatabase);
}

DownloadManager::~DownloadManager() {
    shutdown();
}

void DownloadManager::shutdown() {
    // Cancel every in-flight download and stop all spawned external processes
    // (yt-dlp, ffmpeg children, chunked transfers) so nothing is orphaned on exit.
    speedLimitTimer->stop();

    for (int id : activeChunkedDownloaders.keys()) {
        if (activeChunkedDownloaders.contains(id)) {
            activeChunkedDownloaders[id]->cancel();
            activeChunkedDownloaders[id]->deleteLater();
        }
    }
    activeChunkedDownloaders.clear();

    for (const DownloadItem& item : downloads) {
        if (item.type == "YtDlp") {
            YtDlpManager::instance().cancelDownload(item.id);
        } else if (item.type == "Torrent" && item.aria2cId > 0) {
            Aria2cManager::instance().removeDownload(item.aria2cId);
        }
    }
}

DownloadManager& DownloadManager::instance() {
    static DownloadManager instance;
    return instance;
}

int DownloadManager::addDownload(const QString& url, const QString& path, const QString& type, int chunks, const QString& audioFormat) {
    Logger::instance().info("Adding download: " + url + " Type: " + type + " Format: " + audioFormat);

    DownloadItem item;
    item.id = nextId++;
    item.url = url;
    item.type = type;
    item.audioFormat = audioFormat.isEmpty() ? "mp4" : audioFormat;
    item.totalSize = 0;
    item.downloadedSize = 0;
    item.status = "Queued";
    item.isFolder = false;
    item.progress = 0;
    item.chunks = chunks;
    item.addedAt = QDateTime::currentDateTime();
    item.speed = 0;

    QString fileName;
    if (type == "Torrent") {
        fileName = url.startsWith("magnet:") ? "Magnet Download" : QFileInfo(url).fileName();
    } else {
        QUrl qurl(url);
        fileName = qurl.fileName();
        if (fileName.isEmpty()) {
            QStringList pathParts = qurl.path().split('/', Qt::SkipEmptyParts);
            if (!pathParts.isEmpty()) fileName = pathParts.last();
        }
    }
    if (fileName.isEmpty()) fileName = "download_" + QString::number(item.id);
    // Audio-extraction requests (mp3) should target the final audio file name so
    // the on-disk result matches the download-table entry.
    if (type == "YtDlp" && item.audioFormat == "mp3") {
        QString base = QFileInfo(fileName).completeBaseName();
        base = sanitizeFileName(base.isEmpty() ? fileName : base, "download_" + QString::number(item.id));
        fileName = base + ".mp3";
    }
    item.fileName = sanitizeFileName(fileName, "download_" + QString::number(item.id));

    if (type == "Torrent") {
        item.filePath = path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) : path;
    } else {
        if (path.isEmpty()) {
            item.filePath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/" + fileName;
        } else if (QDir(path).exists() || path.endsWith('/') || path.endsWith('\\')) {
            item.filePath = path + "/" + fileName;
        } else {
            item.filePath = path;
        }
    }

    // A full-file path built for an mp3 extraction must also end in .mp3 so
    // yt-dlp's --extract-audio output matches the tracked path. item.fileName
    // is already sanitized and mp3-suffixed, so rejoin it with the directory.
    if (type == "YtDlp" && item.audioFormat == "mp3" && !QFileInfo(item.filePath).suffix().isEmpty()) {
        item.filePath = QFileInfo(item.filePath).path() + "/" + item.fileName;
    }

    QDir().mkpath(QFileInfo(item.filePath).absolutePath());

    int id = item.id;
    downloads[id] = item;

    DatabaseManager::instance().addDownload(item);
    emit downloadAdded(id, item.filePath, type, false);

    int activeCount = 0;
    for (const DownloadItem& d : downloads) {
        if (d.status == "Downloading") activeCount++;
    }

    if (activeCount < maxConcurrent) {
        if (type == "HTTP" || type == "HTTPS" || type == "FTP") {
            createChunkedDownloaderFor(id, false);
        } else if (type == "YtDlp") {
            downloads[id].status = "Downloading";
            YtDlpManager::instance().startDownload(url, item.filePath, id, item.audioFormat);
            emit statusChanged(id, "Downloading");
        } else if (type == "Torrent") {
            downloads[id].status = "Downloading";
            int ariaId = Aria2cManager::instance().addTorrent(url, item.filePath);

            if (ariaId > 0) {
                connect(&Aria2cManager::instance(), &Aria2cManager::downloadProgress, this, [this, id, ariaId](int aId, qint64 downloaded, qint64 total, qint64 spd) {
                    if (aId != ariaId || !downloads.contains(id)) return;
                    downloads[id].downloadedSize = downloaded;
                    downloads[id].totalSize = total;
                    downloads[id].progress = total > 0 ? (double)downloaded / total * 100.0 : 0;
                    downloads[id].speed = spd;
                    downloads[id].connectedPeers = Aria2cManager::instance().getConnectedPeers(ariaId);
                    downloads[id].leechers = Aria2cManager::instance().getLeechers(ariaId);
                    downloads[id].seeds = Aria2cManager::instance().getSeeds(ariaId);
                    downloads[id].uploadSpeed = Aria2cManager::instance().getUploadSpeed(ariaId);
                    downloads[id].uploadedSize = Aria2cManager::instance().getUploadedBytes(ariaId);
                    emit downloadProgress(id, downloaded, total);
                });

                connect(&Aria2cManager::instance(), &Aria2cManager::downloadFinished, this, [this, id, ariaId](int aId) {
                    if (aId != ariaId || !downloads.contains(id)) return;
                    downloads[id].status = "Completed";
                    downloads[id].progress = 100.0;
                    downloads[id].completedAt = QDateTime::currentDateTime();
                    DatabaseManager::instance().updateDownload(downloads[id]);
                    emit downloadFinished(id);
                    emit statusChanged(id, "Completed");
                    startNextQueued();
                });

                connect(&Aria2cManager::instance(), &Aria2cManager::downloadFailed, this, [this, id, ariaId](int aId, const QString& error) {
                    if (aId != ariaId || !downloads.contains(id)) return;
                    downloads[id].status = "Failed";
                    downloads[id].error = error;
                    DatabaseManager::instance().updateDownload(downloads[id]);
                    emit downloadFailed(id, error);
                    emit statusChanged(id, "Failed");
                    startNextQueued();
                });
            } else {
                downloads[id].status = "Failed";
                downloads[id].error = "aria2c not installed";
                emit downloadFailed(id, "aria2c not installed");
                emit statusChanged(id, "Failed");
            }
        }
    } else {
        Logger::instance().info("Download queued (max concurrent reached): " + QString::number(id));
    }

    return id;
}

void DownloadManager::createChunkedDownloaderFor(int id, bool resumeFromSaved) {
    if (!downloads.contains(id)) return;
    DownloadItem& item = downloads[id];

    ChunkedDownloader* downloader = new ChunkedDownloader(this);

    connect(downloader, &ChunkedDownloader::downloadProgress, this, &DownloadManager::onChunkProgress);
    connect(downloader, &ChunkedDownloader::downloadFinished, this, &DownloadManager::onChunkFinished);
    connect(downloader, &ChunkedDownloader::downloadFailed, this, &DownloadManager::onChunkFailed);
    connect(downloader, &ChunkedDownloader::speedUpdated, this, [this, id](qint64 spd) {
        onChunkSpeed(id, spd);
    });
    connect(downloader, &ChunkedDownloader::filePathChanged, this, [this](int id, const QString& newPath) {
        if (downloads.contains(id)) {
            downloads[id].filePath = newPath;
            downloads[id].fileName = QFileInfo(newPath).fileName();
            Logger::instance().info("Download path updated: id=" + QString::number(id) + " -> " + newPath);
        }
    });

    activeChunkedDownloaders[id] = downloader;
    downloads[id].status = "Downloading";

    bool resumed = false;
    if (resumeFromSaved) {
        QJsonObject state = ChunkedDownloader::readPersistedState(id);
        if (!state.isEmpty()) {
            int savedChunks = state.value("chunks").toInt(item.chunks);
            qint64 savedTotal = state.value("totalBytes").toString().toLongLong();
            bool savedRange = state.value("supportsRange").toBool();
            downloader->resumeFromState(item.url, item.filePath, savedChunks, savedTotal, savedRange, id);
            resumed = true;
        }
    }
    if (!resumed) {
        downloader->startDownload(item.url, item.filePath, item.chunks, id);
    }
    emit statusChanged(id, "Downloading");
}

void DownloadManager::restoreFromDatabase() {
    QVector<DownloadItem> all = DatabaseManager::instance().getAllDownloads();
    if (all.isEmpty()) return;

    Logger::instance().info("Restoring " + QString::number(all.size()) + " download(s) from database");

    QHash<int, QVector<int>> children;
    for (const DownloadItem& item : all) {
        if (item.parentId >= 0) children[item.parentId].append(item.id);
    }
    downloads.clear();
    for (const DownloadItem& item : all) {
        DownloadItem copy = item;
        copy.childIds = children.value(item.id);
        downloads[item.id] = copy;
    }

    const QStringList interrupted = {"Downloading", "Queued", "Paused", "Resuming"};
    QVector<int> startOrder;

    for (DownloadItem& item : downloads) {
        if (item.parentId != -1) continue;  // children mirror their folder parent
        if (!interrupted.contains(item.status)) continue;

        bool isHttp = item.type == "HTTP" || item.type == "HTTPS" || item.type == "FTP";
        if (isHttp) {
            if (item.totalSize > 0 && QFile::exists(item.filePath) && QFileInfo(item.filePath).size() >= item.totalSize) {
                item.status = "Completed";
                for (int cid : item.childIds) {
                    if (downloads.contains(cid)) downloads[cid].status = "Completed";
                }
                DatabaseManager::instance().updateDownload(item);
                continue;
            }
            if (!ChunkedDownloader::hasPersistedData(item.id)) {
                item.status = "Failed";
                item.error = "Download interrupted and cannot be resumed (no partial data available).";
                DatabaseManager::instance().updateDownload(item);
                emit statusChanged(item.id, "Failed");
                continue;
            }
        }

        // Interrupted and resumable: queue it; start below up to maxConcurrent.
        item.status = "Queued";
        DatabaseManager::instance().updateDownload(item);
        startOrder.append(item.id);
    }

    int started = 0;
    for (int id : startOrder) {
        if (started >= maxConcurrent) break;
        if (!downloads.contains(id)) continue;
        DownloadItem& item = downloads[id];
        bool isHttp = item.type == "HTTP" || item.type == "HTTPS" || item.type == "FTP";
        if (isHttp) {
            createChunkedDownloaderFor(id, true);
        } else {
            // Torrent / YtDlp: go through resumeDownload, which re-adds the
            // torrent to aria2 (resuming via its .aria2 control file) or
            // restarts yt-dlp. resumeDownload accepts Queued status.
            resumeDownload(id);
        }
        started++;
    }

    // Force one table refresh so persisted history + resumed items are shown.
    if (!all.isEmpty()) {
        emit downloadAdded(all.first().id, all.first().filePath, all.first().type, all.first().isFolder);
    }

    Logger::instance().info("Download restore finished, started " + QString::number(started) + " transfer(s)");
}

void DownloadManager::addPlaylistDownload(const QVector<PlaylistEntry>& entries, const QString& path, const QString& type, bool useTrackNumbers, const QString& audioFormat, const QString& torrentSourceUrl, const QString& folderName) {
    Logger::instance().info("Adding playlist download: " + QString::number(entries.size()) + " files, type: " + type + ", tracks: " + (useTrackNumbers ? "yes" : "no") + ", format: " + audioFormat);

    QString outputBase = path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) : path;
    if (outputBase.isEmpty()) {
        outputBase = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    }
    QDir().mkpath(outputBase);

    if (type == "Torrent" && !torrentSourceUrl.isEmpty()) {
        QVector<int> selectedIndices;
        for (const PlaylistEntry& entry : entries) {
            if (entry.selected) {
                selectedIndices.append(entry.index);
            }
        }

        QString parentName = folderName.isEmpty() ? QFileInfo(outputBase).fileName() : folderName;
        QString torrentFolder = parentName;
        if (torrentFolder.isEmpty()) torrentFolder = QFileInfo(outputBase).fileName();

        DownloadItem parentItem;
        parentItem.id = nextId++;
        parentItem.url = torrentSourceUrl;
        parentItem.filePath = outputBase;
        parentItem.fileName = parentName;
        parentItem.type = "Torrent";
        parentItem.status = "Downloading";
        parentItem.isFolder = true;
        parentItem.progress = 0;
        parentItem.addedAt = QDateTime::currentDateTime();
        parentItem.torrentSourceUrl = torrentSourceUrl;
        parentItem.selectedIndices = selectedIndices;
        downloads[parentItem.id] = parentItem;

        DatabaseManager::instance().addDownload(parentItem);
        emit downloadAdded(parentItem.id, outputBase, "Torrent", true);

        QString torrentFolderPath = QDir(outputBase).filePath(torrentFolder);
        QDir().mkpath(torrentFolderPath);

        for (const PlaylistEntry& entry : entries) {
            if (!entry.selected) continue;
            QString childPath = QDir(torrentFolderPath).filePath(entry.title);
            addChildDownload(parentItem.id, torrentSourceUrl, childPath, "Torrent", audioFormat);
            if (downloads.contains(parentItem.id) && !downloads[parentItem.id].childIds.isEmpty()) {
                int lastChildId = downloads[parentItem.id].childIds.last();
                if (downloads.contains(lastChildId)) {
                    downloads[lastChildId].status = "Downloading";
                    downloads[lastChildId].totalSize = entry.fileSizeBytes;
                    emit statusChanged(lastChildId, "Downloading");
                }
            }
        }

        int ariaId = Aria2cManager::instance().addTorrentWithSelection(torrentSourceUrl, outputBase, selectedIndices);
        if (ariaId > 0) {
            downloads[parentItem.id].aria2cId = ariaId;
            connect(&Aria2cManager::instance(), &Aria2cManager::downloadProgress, this, [this, id = parentItem.id, ariaId](int aId, qint64 downloaded, qint64 total, qint64 spd) {
                if (aId != ariaId || !downloads.contains(id)) return;
                downloads[id].downloadedSize = downloaded;
                downloads[id].totalSize = total;
                downloads[id].progress = total > 0 ? (double)downloaded / total * 100.0 : 0;
                downloads[id].speed = spd;
                downloads[id].connectedPeers = Aria2cManager::instance().getConnectedPeers(ariaId);
                downloads[id].leechers = Aria2cManager::instance().getLeechers(ariaId);
                downloads[id].seeds = Aria2cManager::instance().getSeeds(ariaId);
                downloads[id].uploadSpeed = Aria2cManager::instance().getUploadSpeed(ariaId);
                downloads[id].uploadedSize = Aria2cManager::instance().getUploadedBytes(ariaId);
                downloads[id].infoHash = Aria2cManager::instance().getInfoHash(ariaId);
                downloads[id].trackers = Aria2cManager::instance().getTrackerList(ariaId);
                {
                    QVector<Aria2FileSize> fileSizes = Aria2cManager::instance().getFileSizes(ariaId);
                    QMap<QString, Aria2FileSize> fileMap;
                    for (const Aria2FileSize& fs : fileSizes) {
                        fileMap.insert(QFileInfo(fs.path).fileName(), fs);
                    }
                    for (int cid : downloads[id].childIds) {
                        if (!downloads.contains(cid)) continue;
                        downloads[cid].speed = spd;
                        downloads[cid].connectedPeers = downloads[id].connectedPeers;
                        downloads[cid].leechers = downloads[id].leechers;
                        downloads[cid].seeds = downloads[id].seeds;
                        downloads[cid].uploadSpeed = downloads[id].uploadSpeed;
                        downloads[cid].uploadedSize = downloads[id].uploadedSize;
                        auto it = fileMap.constFind(downloads[cid].fileName);
                        if (it != fileMap.constEnd()) {
                            downloads[cid].totalSize = it->total;
                            downloads[cid].downloadedSize = it->completed;
                            downloads[cid].progress = it->total > 0 ? (double)it->completed / it->total * 100.0 : 0;
                        } else {
                            double childProg = qBound(0.0, downloads[id].progress, 100.0);
                            if (downloads[cid].totalSize <= 0) downloads[cid].totalSize = total;
                            downloads[cid].downloadedSize = childProg <= 0 ? 0 : (qint64)(downloads[cid].totalSize * childProg / 100.0);
                            downloads[cid].progress = childProg;
                        }
                    }
                }
                emit downloadProgress(id, downloaded, total);
                emit downloadSpeed(id, spd);
            });
            connect(&Aria2cManager::instance(), &Aria2cManager::downloadFinished, this, [this, id = parentItem.id, ariaId](int aId) {
                if (aId != ariaId || !downloads.contains(id)) return;
                downloads[id].status = "Completed";
                downloads[id].progress = 100.0;
                downloads[id].completedAt = QDateTime::currentDateTime();
                for (int cid : downloads[id].childIds) {
                    if (downloads.contains(cid)) {
                        downloads[cid].status = "Completed";
                        downloads[cid].progress = 100.0;
                        downloads[cid].completedAt = QDateTime::currentDateTime();
                    }
                }
                DatabaseManager::instance().updateDownload(downloads[id]);
                emit downloadFinished(id);
                emit statusChanged(id, "Completed");
                startNextQueued();
            });
            connect(&Aria2cManager::instance(), &Aria2cManager::downloadFailed, this, [this, id = parentItem.id, ariaId](int aId, const QString& error) {
                if (aId != ariaId || !downloads.contains(id)) return;
                downloads[id].status = "Failed";
                downloads[id].error = error;
                for (int cid : downloads[id].childIds) {
                    if (downloads.contains(cid)) {
                        downloads[cid].status = "Failed";
                        downloads[cid].error = error;
                    }
                }
                DatabaseManager::instance().updateDownload(downloads[id]);
                emit downloadFailed(id, error);
                emit statusChanged(id, "Failed");
                startNextQueued();
            });
            Logger::instance().info("Torrent download started, aria2c id=" + QString::number(ariaId) + ", parent=" + parentName + ", files=" + QString::number(selectedIndices.size()));
        } else {
            downloads[parentItem.id].status = "Failed";
            downloads[parentItem.id].error = "Failed to start aria2c";
            DatabaseManager::instance().updateDownload(downloads[parentItem.id]);
            emit downloadFailed(parentItem.id, "Failed to start aria2c");
            emit statusChanged(parentItem.id, "Failed");
        }
        return;
    }

    DownloadItem playlistItem;
    playlistItem.id = nextId++;
    playlistItem.url = entries.isEmpty() ? "" : entries[0].url;

    QString playlistFolderPath;
    if (type == "YtDlp" && !folderName.isEmpty()) {
        playlistFolderPath = path + "/" + folderName;
    } else if (type == "YtDlp" && entries.size() > 1) {
        QString cleanName = entries[0].title;
        QRegularExpression re("[\\\\/:*?\"<>|]");
        cleanName.replace(re, "_");
        if (cleanName.length() > 60) cleanName = cleanName.left(57) + "...";
        playlistFolderPath = path + "/" + cleanName;
    } else {
        playlistFolderPath = path;
    }
    QDir().mkpath(playlistFolderPath);

    playlistItem.filePath = playlistFolderPath;
    playlistItem.fileName = folderName.isEmpty() ? QFileInfo(playlistFolderPath).fileName() : folderName;
    playlistItem.type = type;
    playlistItem.status = "Downloading";
    playlistItem.isFolder = true;
    playlistItem.progress = 0;
    playlistItem.addedAt = QDateTime::currentDateTime();
    downloads[playlistItem.id] = playlistItem;

    DatabaseManager::instance().addDownload(playlistItem);
    emit downloadAdded(playlistItem.id, playlistFolderPath, type, true);

    int total = entries.size();
    for (int i = 0; i < entries.size(); i++) {
        const PlaylistEntry& entry = entries[i];
        if (!entry.selected) continue;

        QString childFileName;
        if (useTrackNumbers) {
            QString trackPrefix = TrackNumber::formatTrack(i + 1, total);
            QString ext = entry.extension.isEmpty() ? "mp4" : entry.extension;
            childFileName = trackPrefix + "." + entry.title + "." + ext;
        } else {
            QString ext = entry.extension.isEmpty() ? "mp4" : entry.extension;
            childFileName = entry.title + "." + ext;
        }
        QString childPath = playlistFolderPath + "/" + childFileName;

        addChildDownload(playlistItem.id, entry.url, childPath, type, audioFormat);
    }
}

void DownloadManager::addChildDownload(int parentId, const QString& url, const QString& path, const QString& type, const QString& audioFormat) {
    DownloadItem child;
    child.id = nextId++;
    child.url = url;
    child.filePath = path;
    child.fileName = QFileInfo(path).fileName();
    child.type = type;
    child.status = "Queued";
    child.isFolder = false;
    child.parentId = parentId;
    child.progress = 0;
    child.addedAt = QDateTime::currentDateTime();
    child.chunks = 16;
    child.audioFormat = audioFormat;
    downloads[child.id] = child;

    if (downloads.contains(parentId)) {
        downloads[parentId].childIds.append(child.id);
    }

    DatabaseManager::instance().addDownload(child);
    emit downloadAdded(child.id, path, type, false);

    int activeCount = 0;
    for (const DownloadItem& d : downloads) {
        if (d.status == "Downloading") activeCount++;
    }

    if (activeCount < maxConcurrent) {
        if (type == "HTTP" || type == "HTTPS" || type == "FTP") {
            createChunkedDownloaderFor(child.id, false);
        } else if (type == "YtDlp") {
            downloads[child.id].status = "Downloading";
            YtDlpManager::instance().startDownload(url, path, child.id, audioFormat);
            emit statusChanged(child.id, "Downloading");
        }
    }
}

void DownloadManager::pauseDownload(int id) {
    if (!downloads.contains(id)) return;
    DownloadItem& item = downloads[id];

    if (item.status != "Downloading" && item.status != "Queued") return;

    item.status = "Paused";
    Logger::instance().info("Download paused: " + QString::number(id));

    if (activeChunkedDownloaders.contains(id)) {
        activeChunkedDownloaders[id]->pause();
    }

    if (item.type == "Torrent" && item.aria2cId > 0) {
        Aria2cManager::instance().pauseDownload(item.aria2cId);
    }

    if (item.type == "YtDlp") {
        YtDlpManager::instance().pauseDownload(id);
    }

    if (item.isFolder && item.type == "Torrent") {
        for (int cid : item.childIds) {
            if (downloads.contains(cid)) downloads[cid].status = "Paused";
        }
    }

    DatabaseManager::instance().updateDownload(item);
    emit downloadPaused(id);
    emit statusChanged(id, "Paused");
}

void DownloadManager::resumeDownload(int id) {
    if (!downloads.contains(id)) return;
    DownloadItem& item = downloads[id];

    if (item.status != "Paused" && item.status != "Failed" && item.status != "Queued") return;

    item.status = "Downloading";
    Logger::instance().info("Download resumed: " + QString::number(id));

    if (activeChunkedDownloaders.contains(id)) {
        activeChunkedDownloaders[id]->resume();
    } else if (item.type == "HTTP" || item.type == "HTTPS" || item.type == "FTP") {
        // Prefer continuing from the partial .chunk data of a crashed/previous
        // session; otherwise start from scratch.
        createChunkedDownloaderFor(id, ChunkedDownloader::hasPersistedData(id));
    } else if (item.type == "YtDlp") {
        if (YtDlpManager::instance().isRunning(id)) {
            YtDlpManager::instance().resumeDownload(id);
        } else {
            YtDlpManager::instance().startDownload(item.url, item.filePath, id, item.audioFormat);
        }
    } else if (item.type == "Torrent") {
        QString sourceUrl = item.torrentSourceUrl.isEmpty() ? item.url : item.torrentSourceUrl;
        int ariaId = -1;
        if (item.aria2cId > 0 && Aria2cManager::instance().isRunning(item.aria2cId)) {
            // Torrent still live in the daemon (paused) -> just unpause it.
            ariaId = item.aria2cId;
            Aria2cManager::instance().resumeDownload(ariaId);
            for (int cid : downloads[id].childIds) {
                if (downloads.contains(cid)) downloads[cid].status = "Downloading";
            }
        } else {
            if (!item.selectedIndices.isEmpty()) {
                ariaId = Aria2cManager::instance().addTorrentWithSelection(sourceUrl, QFileInfo(item.filePath).absolutePath(), item.selectedIndices);
            } else {
                ariaId = Aria2cManager::instance().addTorrent(sourceUrl, QFileInfo(item.filePath).absolutePath());
            }
            if (ariaId > 0) {
                downloads[id].aria2cId = ariaId;
                for (int cid : downloads[id].childIds) {
                    if (downloads.contains(cid)) downloads[cid].status = "Downloading";
                }
            }
        }
        if (ariaId > 0) {
            downloads[id].aria2cId = ariaId;
            connect(&Aria2cManager::instance(), &Aria2cManager::downloadProgress, this, [this, id, ariaId](int aId, qint64 downloaded, qint64 total, qint64 spd) {
                if (aId != ariaId || !downloads.contains(id)) return;
                downloads[id].downloadedSize = downloaded;
                downloads[id].totalSize = total;
                downloads[id].progress = total > 0 ? (double)downloaded / total * 100.0 : 0;
                downloads[id].speed = spd;
                downloads[id].connectedPeers = Aria2cManager::instance().getConnectedPeers(ariaId);
                downloads[id].leechers = Aria2cManager::instance().getLeechers(ariaId);
                downloads[id].seeds = Aria2cManager::instance().getSeeds(ariaId);
                downloads[id].uploadSpeed = Aria2cManager::instance().getUploadSpeed(ariaId);
                downloads[id].uploadedSize = Aria2cManager::instance().getUploadedBytes(ariaId);
                downloads[id].infoHash = Aria2cManager::instance().getInfoHash(ariaId);
                downloads[id].trackers = Aria2cManager::instance().getTrackerList(ariaId);
                {
                    QVector<Aria2FileSize> fileSizes = Aria2cManager::instance().getFileSizes(ariaId);
                    QMap<QString, Aria2FileSize> fileMap;
                    for (const Aria2FileSize& fs : fileSizes) {
                        fileMap.insert(QFileInfo(fs.path).fileName(), fs);
                    }
                    for (int cid : downloads[id].childIds) {
                        if (!downloads.contains(cid)) continue;
                        downloads[cid].speed = spd;
                        downloads[cid].connectedPeers = downloads[id].connectedPeers;
                        downloads[cid].leechers = downloads[id].leechers;
                        downloads[cid].seeds = downloads[id].seeds;
                        downloads[cid].uploadSpeed = downloads[id].uploadSpeed;
                        downloads[cid].uploadedSize = downloads[id].uploadedSize;
                        auto it = fileMap.constFind(downloads[cid].fileName);
                        if (it != fileMap.constEnd()) {
                            downloads[cid].totalSize = it->total;
                            downloads[cid].downloadedSize = it->completed;
                            downloads[cid].progress = it->total > 0 ? (double)it->completed / it->total * 100.0 : 0;
                        } else {
                            double childProg = qBound(0.0, downloads[id].progress, 100.0);
                            if (downloads[cid].totalSize <= 0) downloads[cid].totalSize = total;
                            downloads[cid].downloadedSize = childProg <= 0 ? 0 : (qint64)(downloads[cid].totalSize * childProg / 100.0);
                            downloads[cid].progress = childProg;
                        }
                    }
                }
                emit downloadProgress(id, downloaded, total);
                emit downloadSpeed(id, spd);
            });
            connect(&Aria2cManager::instance(), &Aria2cManager::downloadFinished, this, [this, id, ariaId](int aId) {
                if (aId != ariaId || !downloads.contains(id)) return;
                downloads[id].status = "Completed";
                downloads[id].progress = 100.0;
                downloads[id].completedAt = QDateTime::currentDateTime();
                for (int cid : downloads[id].childIds) {
                    if (downloads.contains(cid)) {
                        downloads[cid].status = "Completed";
                        downloads[cid].progress = 100.0;
                        downloads[cid].completedAt = QDateTime::currentDateTime();
                    }
                }
                DatabaseManager::instance().updateDownload(downloads[id]);
                emit downloadFinished(id);
                emit statusChanged(id, "Completed");
                startNextQueued();
            });
            connect(&Aria2cManager::instance(), &Aria2cManager::downloadFailed, this, [this, id, ariaId](int aId, const QString& error) {
                if (aId != ariaId || !downloads.contains(id)) return;
                downloads[id].status = "Failed";
                downloads[id].error = error;
                for (int cid : downloads[id].childIds) {
                    if (downloads.contains(cid)) {
                        downloads[cid].status = "Failed";
                        downloads[cid].error = error;
                    }
                }
                DatabaseManager::instance().updateDownload(downloads[id]);
                emit downloadFailed(id, error);
                emit statusChanged(id, "Failed");
                startNextQueued();
            });
        }
    }

    DatabaseManager::instance().updateDownload(item);
    emit downloadResumed(id);
    emit statusChanged(id, "Downloading");
}

void DownloadManager::cancelDownload(int id) {
    if (!downloads.contains(id)) return;
    DownloadItem& item = downloads[id];

    item.status = "Cancelled";
    Logger::instance().info("Download cancelled: " + QString::number(id));

    if (activeChunkedDownloaders.contains(id)) {
        activeChunkedDownloaders[id]->cancel();
        activeChunkedDownloaders[id]->discardPartialData();
        activeChunkedDownloaders[id]->deleteLater();
        activeChunkedDownloaders.remove(id);
    }

    if (item.type == "Torrent" && item.aria2cId > 0) {
        Aria2cManager::instance().removeDownload(item.aria2cId);
        item.aria2cId = -1;
        item.speed = 0;
        item.uploadSpeed = 0;
    }

    if (item.type == "YtDlp") {
        YtDlpManager::instance().cancelDownload(id);
    }

    if (item.isFolder) {
        for (int cid : item.childIds) {
            if (downloads.contains(cid)) {
                if (downloads[cid].type == "YtDlp") {
                    YtDlpManager::instance().cancelDownload(cid);
                }
                downloads[cid].status = "Cancelled";
                DatabaseManager::instance().updateDownload(downloads[cid]);
            }
        }
    }

    DatabaseManager::instance().updateDownload(item);
    emit statusChanged(id, "Cancelled");
}

void DownloadManager::removeDownload(int id) {
    if (!downloads.contains(id)) return;

    cancelDownload(id);

    DatabaseManager::instance().removeDownload(id);
    downloads.remove(id);

    emit downloadRemoved(id);
}

void DownloadManager::pauseAll() {
    for (int id : downloads.keys()) {
        if (downloads[id].status == "Downloading") {
            pauseDownload(id);
        }
    }
}

void DownloadManager::resumeAll() {
    for (int id : downloads.keys()) {
        if (downloads[id].status == "Paused") {
            resumeDownload(id);
        }
    }
}

void DownloadManager::clearCompleted() {
    QVector<int> toRemove;
    for (int id : downloads.keys()) {
        if (downloads[id].status == "Completed" || downloads[id].status == "Cancelled") {
            toRemove.append(id);
        }
    }
    for (int id : toRemove) {
        removeDownload(id);
    }
    DatabaseManager::instance().clearCompleted();
}

void DownloadManager::updateMaxConcurrent(int max) {
    maxConcurrent = max;
    startNextQueued();
}

void DownloadManager::setSpeedLimit(qint64 bytesPerSecond) {
    speedLimit = qMax<qint64>(0, bytesPerSecond);
    Logger::instance().info("Speed limit set to " + QString::number(speedLimit) + " B/s");
    applySpeedLimitToDownloaders();
}

qint64 DownloadManager::getSpeedLimit() const {
    return speedLimit;
}

void DownloadManager::startNextQueued() {
    int activeCount = 0;
    for (const DownloadItem& d : downloads) {
        if (d.status == "Downloading") activeCount++;
    }

    for (int id : downloads.keys()) {
        if (activeCount >= maxConcurrent) break;
        if (downloads[id].status == "Queued") {
            resumeDownload(id);
            activeCount++;
        }
    }
}

void DownloadManager::updateAggregateProgress(int parentId) {
    if (!downloads.contains(parentId)) return;
    DownloadItem& parent = downloads[parentId];
    if (!parent.isFolder) return;

    if (parent.childIds.isEmpty()) return;

    double totalProgress = 0;
    qint64 totalDownloaded = 0;
    qint64 totalSize = 0;
    int completedCount = 0;

    for (int childId : parent.childIds) {
        if (downloads.contains(childId)) {
            const DownloadItem& child = downloads[childId];
            totalProgress += child.progress;
            totalDownloaded += child.downloadedSize;
            totalSize += child.totalSize;
            if (child.status == "Completed") completedCount++;
        }
    }

    parent.progress = totalProgress / parent.childIds.size();
    parent.downloadedSize = totalDownloaded;
    parent.totalSize = totalSize;

    if (completedCount == parent.childIds.size()) {
        parent.status = "Completed";
        parent.completedAt = QDateTime::currentDateTime();
        DatabaseManager::instance().updateDownload(parent);
        emit downloadFinished(parentId);
        emit statusChanged(parentId, "Completed");
    }
}

void DownloadManager::onChunkProgress(int id, qint64 downloaded, qint64 total) {
    if (!downloads.contains(id)) return;
    downloads[id].downloadedSize = downloaded;
    downloads[id].totalSize = total;
    downloads[id].progress = total > 0 ? (double)downloaded / total * 100.0 : 0;

    // Throttle SQLite writes to at most one per 500ms per download so high
    // chunk progress frequency cannot lag the UI thread.
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto it = lastProgressToDbMs.constFind(id);
    if (it == lastProgressToDbMs.constEnd() || now - it.value() >= 500) {
        DatabaseManager::instance().updateDownloadProgress(id, downloaded, total, downloads[id].progress);
        lastProgressToDbMs[id] = now;
        // Keep the resume sidecar fresh so a hard crash still leaves enough
        // metadata (url/chunk layout) to continue this download after restart.
        if (activeChunkedDownloaders.contains(id) && total > 0) {
            activeChunkedDownloaders[id]->persistResumeState();
        }
    }

    emit downloadProgress(id, downloaded, total);

    if (downloads[id].parentId > 0) {
        updateAggregateProgress(downloads[id].parentId);
    }
}

void DownloadManager::onChunkFinished(int id) {
    if (!downloads.contains(id)) return;
    DownloadItem& item = downloads[id];

    item.status = "Completed";
    item.progress = 100.0;
    item.completedAt = QDateTime::currentDateTime();
    item.speed = 0;

    // Detect the actual file size even when the server never supplied a
    // Content-Length (chunked/streaming responses). The merged file on disk is the
    // source of truth for the real size.
    if (!item.filePath.isEmpty()) {
        qint64 onDisk = QFileInfo(item.filePath).size();
        if (onDisk > 0) {
            item.totalSize = onDisk;
            item.downloadedSize = onDisk;
        }
    }

    if (activeChunkedDownloaders.contains(id)) {
        activeChunkedDownloaders[id]->deleteLater();
        activeChunkedDownloaders.remove(id);
    }

    DatabaseManager::instance().updateDownload(item);
    Logger::instance().info("Download completed: " + QString::number(id));
    emit downloadFinished(id);
    emit statusChanged(id, "Completed");

    if (item.parentId > 0) {
        updateAggregateProgress(item.parentId);
    }

    startNextQueued();
}

void DownloadManager::onChunkFailed(int id, const QString& error) {
    if (!downloads.contains(id)) return;
    DownloadItem& item = downloads[id];

    item.status = "Failed";
    item.error = error;
    item.speed = 0;

    if (activeChunkedDownloaders.contains(id)) {
        activeChunkedDownloaders[id]->deleteLater();
        activeChunkedDownloaders.remove(id);
    }

    DatabaseManager::instance().updateDownload(item);
    Logger::instance().error("Download failed: " + QString::number(id) + " - " + error);
    emit downloadFailed(id, error);
    emit statusChanged(id, "Failed");

    if (item.parentId > 0) {
        updateAggregateProgress(item.parentId);
    }

    startNextQueued();
}

void DownloadManager::onChunkSpeed(int id, qint64 spd) {
    if (!downloads.contains(id)) return;
    downloads[id].speed = spd;
    emit downloadSpeed(id, spd);
}

void DownloadManager::onYtDlpProgress(int id, qint64 downloaded, qint64 total) {
    if (!downloads.contains(id)) return;
    downloads[id].downloadedSize = downloaded;
    downloads[id].totalSize = total;
    downloads[id].progress = total > 0 ? (double)downloaded / total * 100.0 : 0;

    emit downloadProgress(id, downloaded, total);

    if (downloads[id].parentId > 0) {
        updateAggregateProgress(downloads[id].parentId);
    }
}

void DownloadManager::onYtDlpFinished(int id) {
    if (!downloads.contains(id)) return;
    DownloadItem& item = downloads[id];

    item.status = "Completed";
    item.progress = 100.0;
    item.completedAt = QDateTime::currentDateTime();
    item.speed = 0;

    // Completions that lack a parsed total size (e.g. yt-dlp never emitted a
    // "NN% of XXMiB" line) still know the truth: the size of the file written to
    // disk. Reflect that as the actual file size.
    if (!item.filePath.isEmpty()) {
        qint64 onDisk = QFileInfo(item.filePath).size();
        if (onDisk > 0) {
            item.downloadedSize = onDisk;
            item.totalSize = onDisk;
        }
    }

    DatabaseManager::instance().updateDownload(item);
    Logger::instance().info("yt-dlp download completed: " + QString::number(id));
    emit downloadFinished(id);
    emit statusChanged(id, "Completed");

    if (item.parentId > 0) {
        updateAggregateProgress(item.parentId);
    }

    startNextQueued();
}

void DownloadManager::onYtDlpFailed(int id, const QString& error) {
    if (!downloads.contains(id)) return;
    DownloadItem& item = downloads[id];

    item.status = "Failed";
    item.error = error;
    item.speed = 0;

    DatabaseManager::instance().updateDownload(item);
    Logger::instance().error("yt-dlp download failed: " + QString::number(id) + " - " + error);
    emit downloadFailed(id, error);
    emit statusChanged(id, "Failed");

    if (item.parentId > 0) {
        updateAggregateProgress(item.parentId);
    }

    startNextQueued();
}

void DownloadManager::processSpeedLimit() {
    if (speedLimit <= 0) return;

    qint64 totalCurrentSpeed = 0;
    qint64 activeChunkedCount = 0;
    for (const DownloadItem& item : downloads) {
        if (item.status == "Downloading") {
            totalCurrentSpeed += item.speed;
        }
    }
    for (const ChunkedDownloader* dl : activeChunkedDownloaders) {
        if (dl->isDownloading()) activeChunkedCount++;
    }

    // Lower hysteresis band: once aggregate speed is comfortably under the limit,
    // cancel any residual throttling so downloads can run at full speed again.
    if (totalCurrentSpeed <= (qint64)(speedLimit * 0.8)) {
        for (ChunkedDownloader* dl : activeChunkedDownloaders) {
            dl->setSpeedLimit(0);
        }
        return;
    }

    if (totalCurrentSpeed > speedLimit) {
        double ratio = (double)speedLimit / totalCurrentSpeed;
        for (int id : downloads.keys()) {
            if (downloads[id].status == "Downloading" && activeChunkedDownloaders.contains(id)) {
                activeChunkedDownloaders[id]->setSpeedLimit(qMax<qint64>(1024, (qint64)(downloads[id].speed * ratio)));
            }
        }
    } else if (activeChunkedCount > 0) {
        // In the hysteresis zone, hold the current caps (do not loosen or tighten).
    }
}

void DownloadManager::applySpeedLimitToDownloaders() {
    for (int id : activeChunkedDownloaders.keys()) {
        ChunkedDownloader* dl = activeChunkedDownloaders[id];
        if (speedLimit <= 0) {
            dl->setSpeedLimit(0);
        } else if (dl->isDownloading()) {
            // Reset the downloader to unlimited; the polling processSpeedLimit()
            // will apply a proportional cap if the aggregate exceeds the limit.
            dl->setSpeedLimit(0);
        }
    }
}

QVector<DownloadItem> DownloadManager::getDownloads() const {
    return downloads.values();
}

QVector<DownloadItem> DownloadManager::getDownloadsByStatus(const QString& status) {
    QVector<DownloadItem> result;
    for (const DownloadItem& item : downloads) {
        if (item.status == status) {
            result.append(item);
        }
    }
    return result;
}

DownloadItem DownloadManager::getDownload(int id) const {
    if (downloads.contains(id)) {
        return downloads[id];
    }
    return DownloadItem();
}
