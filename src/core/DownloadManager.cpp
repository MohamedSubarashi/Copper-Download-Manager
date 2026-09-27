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
#include <QFile>
#include <QSet>
#include <QUrl>
#include <QStandardPaths>
#include <QDateTime>
#include <QDir>
#include <QProcess>
#include <QRegularExpression>

// yt-dlp's download archive for a playlist job. It records every item that
// finished successfully, so restarting a job continues with the remaining items
// instead of downloading the whole playlist again.
static QString jobArchivePath(const QString& folder) {
    return folder + "/.copper-archive.txt";
}


DownloadManager::DownloadManager() : nextId(1), maxConcurrent(5), speedLimit(0), speedLimitAccumulator(0) {
    speedLimitTimer = new QTimer(this);
    connect(speedLimitTimer, &QTimer::timeout, this, &DownloadManager::processSpeedLimit);
    speedLimitTimer->start(100);

    // Honor the configured concurrency instead of the old hard-coded 5. The value
    // used to be ignored entirely, which is part of why playlists over-subscribed
    // the extractor and every queued video was fed into a throttled process.
    QString savedMax = DatabaseManager::instance().getSetting("maxConcurrentDownloads", "");
    bool ok = false;
    int configured = savedMax.toInt(&ok);
    if (ok && configured > 0) maxConcurrent = configured;

    int maxId = DatabaseManager::instance().getMaxDownloadId();
    if (maxId >= nextId) {
        nextId = maxId + 1;
    }

    connect(&YtDlpManager::instance(), &YtDlpManager::downloadProgress, this, &DownloadManager::onYtDlpProgress);
    connect(&YtDlpManager::instance(), &YtDlpManager::downloadSpeed, this, &DownloadManager::onYtDlpSpeed);
    connect(&YtDlpManager::instance(), &YtDlpManager::downloadFinished, this, &DownloadManager::onYtDlpFinished);
    connect(&YtDlpManager::instance(), &YtDlpManager::downloadFailed, this, &DownloadManager::onYtDlpFailed);
    connect(&YtDlpManager::instance(), &YtDlpManager::videoProgress, this, &DownloadManager::onYtDlpVideoProgress);
    connect(&YtDlpManager::instance(), &YtDlpManager::videoPath, this, &DownloadManager::onYtDlpVideoPath);
    connect(&YtDlpManager::instance(), &YtDlpManager::playlistFinished, this, &DownloadManager::onYtDlpPlaylistFinished);

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

    for (QTimer* timer : retryTimers) {
        timer->stop();
    }
    retryTimers.clear();

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

static QStringList splitFormatList(const QString& raw) {
    QStringList result;
    for (const QString& token : raw.split(QRegularExpression("[,;\\s]+"), Qt::SkipEmptyParts)) {
        QString clean = token.trimmed().toLower();
        while (clean.startsWith('.')) clean = clean.mid(1);
        if (!clean.isEmpty()) result.append(clean);
    }
    return result;
}

QStringList DownloadManager::includeFileFormats() {
    static const QStringList defaultInclude = {};
    QString saved = DatabaseManager::instance().getSetting("formatIncludeExtensions", "");
    if (saved.isEmpty()) return defaultInclude;
    return splitFormatList(saved);
}

QStringList DownloadManager::excludeFileFormats() {
    static const QStringList defaultExclude = {
        "png","jpg","jpeg","gif","webp","bmp","svg","ico","avif","jfif",
        "heic","heif","tif","tiff","raw","psd","eps","ai","dng","cr2","nef","arw","exr"
    };
    QString saved = DatabaseManager::instance().getSetting("formatExcludeExtensions", "");
    if (saved.isEmpty()) return defaultExclude;
    return splitFormatList(saved);
}

bool DownloadManager::fileFormatFilterEnabled() {
    return DatabaseManager::instance().getSetting("formatFilterEnabled", "true") == "true";
}

bool DownloadManager::isFileFormatAllowed(const QString& url) {
    if (!fileFormatFilterEnabled()) return true;
    QString ext = QFileInfo(QUrl(url).path()).suffix().toLower();
    if (ext.isEmpty()) return true;
    QStringList exclude = excludeFileFormats();
    if (exclude.contains(ext)) return false;
    QStringList include = includeFileFormats();
    if (!include.isEmpty() && !include.contains(ext)) return false;
    return true;
}

int DownloadManager::addDownload(const QString& url, const QString& path, const QString& type, int chunks, const QString& audioFormat) {
    if (!isFileFormatAllowed(url)) {
        Logger::instance().info("Download blocked by file-format filter: " + url);
        return -1;
    }

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
        // Folder parents (playlist/torrent containers) are not real transfers and
        // must not consume a concurrent slot, otherwise injecting several playlists
        // deadlocks their children in "Queued".
        if (d.status == "Downloading" && !d.isFolder) activeCount++;
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

    // Children of a yt-dlp playlist job are mirrors of the single process that
    // drives the whole playlist - they are never transfers of their own. Their
    // persisted "Downloading" status would otherwise count as one active transfer
    // per item and block every other download from ever starting after a restart.
    for (int id : downloads.keys()) {
        if (!downloads.contains(id)) continue;
        DownloadItem& child = downloads[id];
        if (child.parentId == -1) continue;
        if (!downloads.contains(child.parentId)) continue;
        if (downloads[child.parentId].type != "YtDlp") continue;
        if (child.status != "Completed") {
            child.status = "Queued";
            child.speed = 0;
        }
    }

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

        if (item.type == "YtDlp" && item.isFolder) {
            // Items yt-dlp already finished are recorded in its download archive;
            // re-running the job skips them, so mirror that here instead of
            // showing every track as pending again.
            markJobChildrenFromDisk(item.id);
        }

        // Interrupted and resumable: queue it; start below up to maxConcurrent.
        item.status = "Queued";
        item.speed = 0;
        DatabaseManager::instance().updateDownload(item);
        startOrder.append(item.id);
    }

    int activeCount = 0;
    for (const DownloadItem& d : downloads) {
        if (d.status == "Downloading" && !d.isFolder) activeCount++;
    }
    int started = 0;
    int startedJobs = 0;
    for (int id : startOrder) {
        if (!downloads.contains(id)) continue;
        DownloadItem& item = downloads[id];
        // Folder jobs (a playlist is one yt-dlp process) are governed by the yt-dlp
        // job limit, not by maxConcurrent, so they must not consume a transfer slot.
        if (!item.isFolder && activeCount + started >= maxConcurrent) break;
        bool isHttp = item.type == "HTTP" || item.type == "HTTPS" || item.type == "FTP";
        if (isHttp) {
            createChunkedDownloaderFor(id, true);
            activeCount++;
        } else {
            // Torrent / YtDlp: go through resumeDownload, which re-adds the
            // torrent to aria2 (resuming via its .aria2 control file) or
            // restarts yt-dlp. resumeDownload accepts Queued status.
            resumeDownload(id);
        }
        if (item.isFolder) startedJobs++;
        else started++;
    }

    // Force one table refresh so persisted history + resumed items are shown.
    if (!all.isEmpty()) {
        emit downloadAdded(all.first().id, all.first().filePath, all.first().type, all.first().isFolder);
    }

    Logger::instance().info("Download restore finished, started " + QString::number(started) +
                            " transfer(s) and " + QString::number(startedJobs) + " job(s)");
}

int DownloadManager::addPlaylistDownload(const QVector<PlaylistEntry>& entries, const QString& path, const QString& type, bool useTrackNumbers, const QString& audioFormat, const QString& torrentSourceUrl, const QString& folderName, const QString& playlistUrl) {
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
            // The parent never got an aria2 id: mark any children that were left
            // "Queued" as failed too so they don't linger forever behind the
            // failed parent.
            for (int cid : downloads[parentItem.id].childIds) {
                if (downloads.contains(cid)) {
                    downloads[cid].status = "Failed";
                    downloads[cid].error = "Failed to start aria2c";
                }
            }
            DatabaseManager::instance().updateDownload(downloads[parentItem.id]);
            emit downloadFailed(parentItem.id, "Failed to start aria2c");
            emit statusChanged(parentItem.id, "Failed");
        }
        return parentItem.id;
    }

    // A yt-dlp playlist is downloaded by ONE yt-dlp process, and its per-video
    // progress lines are mirrored onto one table row per item. Spawning one
    // process per video is what made playlists fail: several extractors hitting
    // the same site at once get throttled, the app saw the quiet children as
    // stalled and killed them, and the queue then fed the next videos into the
    // same trap. yt-dlp itself already handles retries, resumes .part files and
    // skips dead items, so one process is both simpler and far more reliable.
    QString jobUrl = playlistUrl;
    if (jobUrl.isEmpty()) jobUrl = entries.isEmpty() ? QString() : entries[0].url;

    DownloadItem playlistItem;
    playlistItem.id = nextId++;
    playlistItem.url = jobUrl;
    playlistItem.audioFormat = audioFormat.isEmpty() ? "mp4" : audioFormat;

    QString playlistFolderPath;
    if (!folderName.isEmpty()) {
        playlistFolderPath = path + "/" + folderName;
    } else if (entries.size() > 1) {
        QString cleanName = entries[0].title;
        QRegularExpression re("[\\\\/:*?\"<>|]");
        cleanName.replace(re, "_");
        if (cleanName.length() > 60) cleanName = cleanName.left(57) + "...";
        cleanName = sanitizeFileName(cleanName, "playlist");
        playlistFolderPath = path + "/" + cleanName;
    } else {
        playlistFolderPath = path;
    }
    QDir().mkpath(playlistFolderPath);

    playlistItem.filePath = playlistFolderPath;
    playlistItem.fileName = folderName.isEmpty() ? QFileInfo(playlistFolderPath).fileName() : folderName;
    playlistItem.type = type;
    playlistItem.status = "Queued";
    playlistItem.isFolder = true;
    playlistItem.progress = 0;
    playlistItem.addedAt = QDateTime::currentDateTime();
    // Register the parent before the children: addChildDownload() appends to the
    // parent's childIds, so inserting it afterwards would drop every child.
    downloads[playlistItem.id] = playlistItem;

    // One row per selected item. The names only need to be close to what yt-dlp
    // will write: every finished file reports its real path back (after_move)
    // and the row adopts it.
    int total = entries.size();
    for (int i = 0; i < entries.size(); i++) {
        const PlaylistEntry& entry = entries[i];
        if (!entry.selected) continue;

        QString ext = entry.extension.isEmpty() ? "mp4" : entry.extension;
        if (playlistItem.audioFormat == "mp3") ext = "mp3";
        // Number files by their real playlist position so the name matches the
        // row the table shows and stays stable no matter which items are selected.
        QString childFileName;
        if (useTrackNumbers) {
            childFileName = TrackNumber::formatTrack(entry.index, total) + "." +
                            sanitizeFileName(entry.title, "track_" + QString::number(entry.index)) +
                            "." + ext;
        } else {
            childFileName = sanitizeFileName(entry.title, "track_" + QString::number(entry.index)) + "." + ext;
        }
        QString childPath = playlistFolderPath + "/" + childFileName;

        addChildDownload(playlistItem.id, entry.url, childPath, type, playlistItem.audioFormat);
        if (!downloads.contains(playlistItem.id)) continue;
        const QList<int> kids = downloads[playlistItem.id].childIds;
        if (kids.isEmpty()) continue;
        DownloadItem& child = downloads[kids.last()];
        child.videoId = entry.videoId;
        child.trackIndex = entry.index;
        DatabaseManager::instance().updateDownload(child);
    }

    DatabaseManager::instance().addDownload(playlistItem);
    emit downloadAdded(playlistItem.id, playlistFolderPath, type, true);

    // The job itself is a single transfer: it takes one concurrency slot and one
    // yt-dlp slot, no matter how many videos the playlist holds.
    if (ytDlpSlotAvailable()) {
        startYtDlpPlaylistJob(playlistItem.id);
    } else {
        Logger::instance().info("Playlist job queued: all yt-dlp job slots are busy");
    }
    return playlistItem.id;
}

void DownloadManager::addChildDownload(int parentId, const QString& url, const QString& path, const QString& type, const QString& audioFormat) {
    if (!isFileFormatAllowed(url)) {
        Logger::instance().info("Child download blocked by file-format filter: " + url);
        return;
    }
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

    // A yt-dlp child is driven by its folder parent's single process, so it must
    // never start a process of its own. Only independently-queued HTTP children
    // (torrent file lists included) start here.
    if (type == "YtDlp") return;

    int activeCount = 0;
    for (const DownloadItem& d : downloads) {
        if (d.status == "Downloading" && !d.isFolder) activeCount++;
    }

    if (activeCount < maxConcurrent) {
        if (type == "HTTP" || type == "HTTPS" || type == "FTP") {
            createChunkedDownloaderFor(child.id, false);
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
        // Terminates the process and keeps the .part files; resume relaunches it
        // with --continue.
        YtDlpManager::instance().pauseDownload(id);
    }

    if (item.isFolder) {
        for (int cid : item.childIds) {
            if (!downloads.contains(cid)) continue;
            // Items that already reached a terminal state stay that way: a pause
            // must not turn a finished file back into a pending one, or the job
            // would look like it still has work to do.
            const QString childStatus = downloads[cid].status;
            if (childStatus == "Completed" || childStatus == "Cancelled") continue;
            downloads[cid].status = "Paused";
            downloads[cid].speed = 0;
            // Persist each item too, so a restart does not resurrect items that the
            // user had already paused.
            DatabaseManager::instance().updateDownload(downloads[cid]);
            emit statusChanged(cid, "Paused");
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

    // A playlist item is driven by its folder's single yt-dlp process. Starting it
    // on its own would spawn a second process per video - the exact behavior that
    // got the extractor throttled and broke playlists.
    if (isPlaylistJobItem(id)) {
        Logger::instance().info("Playlist item " + QString::number(id) +
                                " is driven by its job, not started on its own");
        return;
    }

    // A playlist job is a single yt-dlp process, so resuming it is just a matter
    // of relaunching that process with --continue. Handled first so that "no free
    // job slot" leaves the row queued instead of claiming to be downloading.
    if (item.type == "YtDlp" && item.isFolder) {
        if (!ytDlpSlotAvailable()) {
            Logger::instance().info("Playlist job " + QString::number(id) + " stays queued: all yt-dlp job slots busy");
            return;
        }
        if (item.status == "Failed") {
            retryDownload(id);
            return;
        }
        // Whatever the previous run finished is adopted from the archive first, so
        // the rows show the truth and yt-dlp only fetches what is left.
        markJobChildrenFromDisk(id);
        for (int cid : item.childIds) {
            if (!downloads.contains(cid)) continue;
            if (downloads[cid].status == "Completed" || downloads[cid].status == "Cancelled") continue;
            // Items left Paused/Queued by the pause must become active again,
            // otherwise their progress lines are ignored and they never finish.
            downloads[cid].status = "Queued";
            downloads[cid].speed = 0;
            DatabaseManager::instance().updateDownload(downloads[cid]);
            emit statusChanged(cid, "Queued");
        }
        item.status = "Queued";
        Logger::instance().info("Playlist job resumed: " + QString::number(id));
        startYtDlpPlaylistJob(id);
        emit downloadResumed(id);
        return;
    }

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
            // Disconnect any prior live aria handlers for this download so the
            // three connections below do not accumulate across repeated resumes.
            for (const QMetaObject::Connection& c : ariaConnectionHandles.take(id))
                disconnect(c);
            QList<QMetaObject::Connection>& handles = ariaConnectionHandles[id];
            handles << connect(&Aria2cManager::instance(), &Aria2cManager::downloadProgress, this, [this, id, ariaId](int aId, qint64 downloaded, qint64 total, qint64 spd) {
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
            handles << connect(&Aria2cManager::instance(), &Aria2cManager::downloadFinished, this, [this, id, ariaId](int aId) {
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
            handles << connect(&Aria2cManager::instance(), &Aria2cManager::downloadFailed, this, [this, id, ariaId](int aId, const QString& error) {
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

    // A queued automatic retry must not fire after the user cancelled the item.
    if (QTimer* pending = retryTimers.take(id)) {
        pending->stop();
        pending->deleteLater();
    }

    item.status = "Cancelled";
    item.speed = 0;
    item.eta = 0;
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
            if (!downloads.contains(cid)) continue;
            if (QTimer* pendingChild = retryTimers.take(cid)) {
                pendingChild->stop();
                pendingChild->deleteLater();
            }
            // Files that already finished stay Completed: cancelling the job stops
            // the remaining items, it does not undo the ones on disk.
            if (downloads[cid].status == "Completed") continue;
            downloads[cid].status = "Cancelled";
            downloads[cid].speed = 0;
            DatabaseManager::instance().updateDownload(downloads[cid]);
            emit statusChanged(cid, "Cancelled");
        }
    }

    DatabaseManager::instance().updateDownload(item);
    emit statusChanged(id, "Cancelled");
    startNextQueued();
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
        // Folder parents are containers, not real transfers; never count them
        // against maxConcurrent (prevents multi-playlist deadlock).
        if (d.status == "Downloading" && !d.isFolder) activeCount++;
    }

    for (int id : downloads.keys()) {
        if (!downloads.contains(id)) continue;
        if (downloads[id].status != "Queued" || isPlaylistJobItem(id)) continue;
        // An item waiting out its automatic-retry backoff belongs to its retry
        // timer: starting it here would defeat the backoff and hammer a source
        // that has only just failed.
        if (retryTimers.contains(id)) continue;
        // Folder jobs are governed by the yt-dlp job limit rather than by
        // maxConcurrent, so they must not consume a transfer slot - and they must
        // still be considered once the transfer slots are full.
        const bool isFolder = downloads[id].isFolder;
        if (!isFolder && activeCount >= maxConcurrent) continue;
        resumeDownload(id);
        if (!isFolder) activeCount++;
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
    int counted = 0;

    for (int childId : parent.childIds) {
        if (!downloads.contains(childId)) continue;
        const DownloadItem& child = downloads[childId];
        // Items the user cancelled are not part of what this folder was asked to
        // deliver, so they must not drag the folder's progress down forever.
        if (child.status == "Cancelled") continue;
        totalProgress += child.progress;
        totalDownloaded += child.downloadedSize;
        totalSize += child.totalSize;
        counted++;
        if (child.status == "Completed") completedCount++;
    }

    if (counted == 0) return;

    parent.progress = totalProgress / counted;
    parent.downloadedSize = totalDownloaded;
    parent.totalSize = totalSize;

    if (completedCount == counted && parent.type != "YtDlp") {
        // A yt-dlp playlist job is only complete when its process says so: an item
        // reports "finished" per format, so every child can read as completed
        // while ffmpeg is still merging the last one.
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

    if (downloads[id].parentId != -1) {
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

    if (item.parentId != -1) {
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

    if (item.parentId != -1) {
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

    if (downloads[id].parentId != -1) {
        updateAggregateProgress(downloads[id].parentId);
    }
}

void DownloadManager::onYtDlpSpeed(int id, qint64 spd) {
    if (!downloads.contains(id)) return;
    downloads[id].speed = spd;
    emit downloadSpeed(id, spd);
}

int DownloadManager::childCount(int id) const {
    return downloads.value(id).childIds.size();
}

int DownloadManager::maxRetries() const {
    return qBound(0, DatabaseManager::instance().getSetting("maxRetries", "3").toInt(), 10);
}

int DownloadManager::ytDlpPlaylistJobWidth(int jobId) const {
    auto job = downloads.constFind(jobId);
    if (job == downloads.constEnd()) return 3;
    int maxIndex = 0;
    for (int cid : job.value().childIds) {
        if (downloads.contains(cid)) maxIndex = qMax(maxIndex, downloads[cid].trackIndex);
    }
    // Must match TrackNumber::formatTrack() so the row's placeholder name lines
    // up with the file yt-dlp actually writes.
    return maxIndex >= 1000 ? 4 : 3;
}

int DownloadManager::activeYtDlpJobCount() const {
    int count = 0;
    for (const DownloadItem& item : downloads) {
        if (item.type != "YtDlp") continue;
        if (item.status != "Downloading") continue;
        if (YtDlpManager::instance().isRunning(item.id)) count++;
    }
    return count;
}

bool DownloadManager::ytDlpSlotAvailable() const {
    // Playlist downloads are a single process, so this caps how many extractors
    // the app runs at once. A handful of parallel processes is what triggers the
    // site-side throttling that used to break playlists.
    int maxJobs = qBound(1, DatabaseManager::instance().getSetting("maxYtDlpJobs", "2").toInt(), 8);
    return activeYtDlpJobCount() < maxJobs;
}

bool DownloadManager::isPlaylistJobItem(int id) const {
    auto item = downloads.constFind(id);
    if (item == downloads.constEnd() || item.value().parentId == -1) return false;
    auto parent = downloads.constFind(item.value().parentId);
    if (parent == downloads.constEnd()) return false;
    return parent.value().isFolder && parent.value().type == "YtDlp";
}

QString DownloadManager::findJobFileForTrack(const QString& folder, int trackIndex) const {
    if (trackIndex <= 0 || folder.isEmpty()) return QString();
    QDir dir(folder);
    if (!dir.exists()) return QString();
    // Files are written as "<NNN>.<title>.<ext>", so the leading number identifies
    // the playlist position no matter what yt-dlp did to the rest of the name.
    const QStringList names = dir.entryList(QDir::Files, QDir::Name);
    for (const QString& name : names) {
        if (name.startsWith('.')) continue;
        const int dot = name.indexOf('.');
        if (dot <= 0) continue;
        bool ok = false;
        const int index = name.left(dot).toInt(&ok);
        if (!ok || index != trackIndex) continue;
        const QFileInfo info(dir.filePath(name));
        if (info.size() <= 0) continue;
        return info.absoluteFilePath();
    }
    return QString();
}

QString DownloadManager::tidyJobFilePath(const QString& path) {
    if (path.isEmpty()) return path;

    // A download that is interrupted and then continued can end up as the fragment
    // yt-dlp was writing ("<title>.f616.mp4") instead of the merged "<title>.mp4".
    // That is a valid, complete file, just badly named, so give it the name the
    // playlist row advertises.
    static const QRegularExpression fragmentRe("\\.f\\d+(\\.[A-Za-z0-9]{1,5})$");
    const QRegularExpressionMatch m = fragmentRe.match(path);
    if (!m.hasMatch()) return path;

    QString clean = path;
    clean.remove(m.capturedStart(0), m.capturedLength(0));
    const QFileInfo cleanInfo(clean);
    if (cleanInfo.exists() || clean.isEmpty()) return path;   // never clobber a real file

    const QFileInfo source(path);
    if (!source.exists() || source.size() <= 0) return path;
    if (!QFile::rename(path, clean)) return path;

    Logger::instance().info("Renamed interrupted fragment to the final name: " +
                            source.fileName() + " -> " + cleanInfo.fileName());
    return clean;
}

int DownloadManager::findChildByVideoId(int parentId, const QString& videoId) const {
    if (videoId.isEmpty()) return -1;
    auto parent = downloads.constFind(parentId);
    if (parent == downloads.constEnd()) return -1;
    for (int cid : parent.value().childIds) {
        if (downloads.contains(cid) && downloads[cid].videoId == videoId) return cid;
    }
    return -1;
}

int DownloadManager::findChildByTrackIndex(int parentId, int trackIndex) const {
    if (trackIndex <= 0) return -1;
    auto parent = downloads.constFind(parentId);
    if (parent == downloads.constEnd()) return -1;
    for (int cid : parent.value().childIds) {
        if (downloads.contains(cid) && downloads[cid].trackIndex == trackIndex) return cid;
    }
    return -1;
}

int DownloadManager::findChildForPath(int parentId, int playlistIndex, const QString& path) const {
    auto parent = downloads.constFind(parentId);
    if (parent == downloads.constEnd()) return -1;

    int cid = playlistIndex > 0 ? findChildByTrackIndex(parentId, playlistIndex) : -1;
    if (cid != -1) return cid;

    // A path without a usable playlist index (an unnumbered playlist, or rows
    // saved before the position was recorded) is matched by the file name the row
    // already shows, then by "the one item still to do".
    const QString baseName = QFileInfo(path).completeBaseName();
    for (int kid : parent.value().childIds) {
        if (!downloads.contains(kid)) continue;
        if (downloads[kid].status == "Completed" || downloads[kid].status == "Cancelled") continue;
        if (downloads[kid].filePath == path) return kid;
        if (!baseName.isEmpty() && QFileInfo(downloads[kid].filePath).completeBaseName() == baseName) return kid;
    }
    for (int kid : parent.value().childIds) {
        if (!downloads.contains(kid)) continue;
        if (downloads[kid].status == "Queued" || downloads[kid].status == "Downloading") return kid;
    }
    return -1;
}

void DownloadManager::startYtDlpPlaylistJob(int jobId) {
    if (!downloads.contains(jobId)) return;
    DownloadItem& job = downloads[jobId];
    if (!job.isFolder || job.type != "YtDlp") return;
    if (YtDlpManager::instance().isRunning(jobId)) return;

    if (!ytDlpSlotAvailable()) {
        Logger::instance().info("Playlist job " + QString::number(jobId) + " stays queued: all yt-dlp job slots busy");
        return;
    }

    // Items yt-dlp already finished are skipped through its download archive.
    markJobChildrenFromDisk(jobId);

    for (int cid : job.childIds) {
        if (!downloads.contains(cid)) continue;
        DownloadItem& child = downloads[cid];
        if (child.status == "Completed" || child.status == "Cancelled") continue;
        if (child.status == "Queued") emit statusChanged(cid, "Queued");
    }

    QString format = job.audioFormat.isEmpty() ? "mp4" : job.audioFormat;
    int fragments = qBound(1, DatabaseManager::instance().getSetting("ytDlpFragments", "4").toInt(), 16);

    // The rows are the user's selection, so yt-dlp must be told to fetch exactly
    // those playlist positions. A row without a known position cannot be turned
    // into --playlist-items, so a job that has any such row falls back to the whole
    // playlist (a missing video is better than a silently missing download).
    QVector<int> selected;
    bool hasUnknownPosition = false;
    for (int cid : job.childIds) {
        if (!downloads.contains(cid)) continue;
        const DownloadItem& child = downloads.value(cid);
        if (child.status == "Cancelled") continue;
        if (child.trackIndex > 0) {
            selected.append(child.trackIndex);
        } else {
            hasUnknownPosition = true;
        }
    }
    if (hasUnknownPosition) {
        if (!selected.isEmpty()) {
            Logger::instance().info("Playlist job " + QString::number(jobId) +
                                    " has rows without a playlist position: downloading the whole playlist");
        }
        selected.clear();
    }

    job.status = "Downloading";
    job.speed = 0;
    job.error.clear();
    // The attempt counter is deliberately NOT reset here: it is owned by the retry
    // policy, and clearing it on every (re)start would make a permanently failing
    // job retry forever.
    DatabaseManager::instance().updateDownload(job);
    emit statusChanged(jobId, "Downloading");

    YtDlpManager::instance().startPlaylistJob(jobId, job.url, job.filePath,
                                              ytDlpPlaylistJobWidth(jobId), format, fragments,
                                              selected);
    Logger::instance().info("Started yt-dlp playlist job " + QString::number(jobId) +
                            " (" + QString::number(job.childIds.size()) + " item(s)" +
                            (selected.isEmpty() ? ", whole playlist"
                                                : ", positions " + QString::number(selected.size())) +
                            ") -> " + job.filePath);
}

void DownloadManager::markJobChildrenFromDisk(int jobId, bool includePaused) {
    if (!downloads.contains(jobId)) return;
    const QString folder = downloads[jobId].filePath;
    const QString archive = jobArchivePath(folder);

    // yt-dlp appends "<extractor> <video id>" per finished item to the download
    // archive and skips those on the next run, so it is the authoritative record
    // of what a restarted job will not download again.
    QSet<QString> archived;
    QFile file(archive);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!file.atEnd()) {
            QString line = QString::fromUtf8(file.readLine()).trimmed();
            int sp = line.indexOf(' ');
            if (sp > 0) archived.insert(line.mid(sp + 1));
        }
        file.close();
    }

    for (int cid : downloads[jobId].childIds) {
        if (!downloads.contains(cid)) continue;
        DownloadItem& child = downloads[cid];
        if (child.status == "Completed" || child.status == "Cancelled") continue;
        if (child.status == "Paused" && !includePaused) continue;

        const QFileInfo info(child.filePath);
        bool onDisk = !child.filePath.isEmpty() && info.exists() && info.size() > 0;
        bool inArchive = !child.videoId.isEmpty() && archived.contains(child.videoId);

        if (onDisk || inArchive) {
            if (!onDisk) {
                // Finished, but under a name we did not predict: adopt the real file
                // so the row's path (and "Open file") points at the right thing.
                QString actual = findJobFileForTrack(folder, child.trackIndex);
                if (!actual.isEmpty()) {
                    child.filePath = tidyJobFilePath(actual);
                    child.fileName = QFileInfo(child.filePath).fileName();
                }
            } else {
                child.filePath = tidyJobFilePath(child.filePath);
                child.fileName = QFileInfo(child.filePath).fileName();
            }
            const QFileInfo real(child.filePath);
            child.status = "Completed";
            child.progress = 100.0;
            child.speed = 0;
            child.error.clear();
            child.completedAt = QDateTime::currentDateTime();
            if (real.exists() && real.size() > 0) {
                child.downloadedSize = real.size();
                child.totalSize = real.size();
            }
            DatabaseManager::instance().updateDownload(child);
            Logger::instance().info("Playlist item already on disk: " + QString::number(cid) +
                                    " -> " + child.fileName);
            emit statusChanged(cid, "Completed");
        } else if (child.status != "Queued") {
            child.status = "Queued";
            emit statusChanged(cid, "Queued");
        }
    }
}

void DownloadManager::onYtDlpVideoProgress(int id, const QString& videoId, const QString& state,
                                           qint64 downloaded, qint64 total, qint64 speed, qint64 eta,
                                           int fragmentIndex, int fragmentCount) {
    Q_UNUSED(fragmentIndex);
    Q_UNUSED(fragmentCount);
    if (!downloads.contains(id)) return;

    if (!downloads[id].isFolder) {
        // Single video: the job and the row are the same thing.
        DownloadItem& item = downloads[id];
        if (state == "finished") { onYtDlpFinished(id); return; }
        if (item.status == "Completed" || item.status == "Cancelled") return;
        if (downloaded >= 0) item.downloadedSize = downloaded;
        if (total > 0) item.totalSize = total;
        if (item.totalSize > 0) {
            item.progress = qBound(0.0, (double)item.downloadedSize / item.totalSize * 100.0, 100.0);
        }
        item.speed = qMax<qint64>(0, speed);
        item.eta = qMax<qint64>(0, eta);
        emit downloadProgress(id, item.downloadedSize, item.totalSize);
        emit downloadSpeed(id, item.speed);
        return;
    }

    // Playlist job: attribute the line to the row of the item it belongs to.
    int cid = findChildByVideoId(id, videoId);
    if (cid == -1) return;
    DownloadItem& child = downloads[cid];
    if (child.status == "Completed" || child.status == "Cancelled" || child.status == "Paused") return;

    if (state == "finished") {
        noteYtDlpChildFinished(id, cid, QString());
        return;
    }

    if (child.status != "Downloading") {
        child.status = "Downloading";
        emit statusChanged(cid, "Downloading");
    }
    if (downloaded >= 0) child.downloadedSize = downloaded;
    if (total > 0) child.totalSize = total;
    if (child.totalSize > 0) {
        child.progress = qBound(0.0, (double)child.downloadedSize / child.totalSize * 100.0, 100.0);
    }
    child.speed = qMax<qint64>(0, speed);
    child.eta = qMax<qint64>(0, eta);
    emit downloadProgress(cid, child.downloadedSize, child.totalSize);
    emit downloadSpeed(cid, child.speed);

    // The folder row shows the speed of whatever the single process is fetching.
    downloads[id].speed = child.speed;
    emit downloadSpeed(id, child.speed);

    // Throttle SQLite writes per row: a fragment-parallel job can report several
    // progress lines per second and must not turn that into a disk write storm.
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto it = lastProgressToDbMs.constFind(cid);
    if (it == lastProgressToDbMs.constEnd() || now - it.value() >= 500) {
        DatabaseManager::instance().updateDownloadProgress(cid, child.downloadedSize, child.totalSize, child.progress);
        lastProgressToDbMs[cid] = now;
    }

    updateAggregateProgress(id);
}

void DownloadManager::onYtDlpVideoPath(int id, int playlistIndex, const QString& path) {
    if (!downloads.contains(id) || path.isEmpty()) return;

    if (!downloads[id].isFolder) {
        DownloadItem& item = downloads[id];
        const QString real = tidyJobFilePath(path);
        if (item.filePath == real) return;
        // Adopt the name yt-dlp actually used so "open folder" and the completed
        // file always agree with the row.
        item.filePath = real;
        item.fileName = QFileInfo(real).fileName();
        DatabaseManager::instance().updateDownload(item);
        emit statusChanged(id, item.status);
        return;
    }

    int cid = findChildForPath(id, playlistIndex, path);
    if (cid == -1) return;
    DownloadItem& child = downloads[cid];
    if (child.status == "Cancelled") return;

    const QString real = tidyJobFilePath(path);
    if (child.filePath != real) {
        child.filePath = real;
        child.fileName = QFileInfo(real).fileName();
    }
    // The file only exists once the merge finished, so this is the point where the
    // real size is known.
    const QFileInfo info(child.filePath);
    if (info.exists() && info.size() > 0) {
        child.downloadedSize = info.size();
        child.totalSize = info.size();
        if (child.status == "Completed") child.progress = 100.0;
    }
    DatabaseManager::instance().updateDownload(child);
    emit statusChanged(cid, child.status);
    updateAggregateProgress(id);
}

void DownloadManager::noteYtDlpChildFinished(int id, int childId, const QString& path) {
    if (!downloads.contains(childId)) return;
    DownloadItem& child = downloads[childId];
    if (child.status == "Cancelled") return;

    if (!path.isEmpty()) {
        child.filePath = tidyJobFilePath(path);
        child.fileName = QFileInfo(child.filePath).fileName();
    }
    if (child.status == "Completed") {
        DatabaseManager::instance().updateDownload(child);
        return;   // a merged video reports "finished" once per format
    }

    child.status = "Completed";
    child.progress = 100.0;
    child.speed = 0;
    child.eta = 0;
    child.error.clear();
    child.completedAt = QDateTime::currentDateTime();
    const QFileInfo info(child.filePath);
    if (info.exists() && info.size() > 0) {
        child.downloadedSize = info.size();
        child.totalSize = info.size();
    }
    DatabaseManager::instance().updateDownload(child);
    Logger::instance().info("Playlist item completed: " + QString::number(childId) + " -> " + child.fileName);
    emit downloadFinished(childId);
    emit statusChanged(childId, "Completed");
    updateAggregateProgress(id);
}

void DownloadManager::onYtDlpPlaylistFinished(int id, bool ok, const QString& error) {
    if (!downloads.contains(id) || !downloads[id].isFolder) return;
    DownloadItem& job = downloads[id];
    if (job.status == "Cancelled") return;

    // Terminal sweep: settle every item the process never reported on. yt-dlp
    // skips archived items and silently drops unavailable ones, so whatever is
    // still pending here is a genuine failure. Items are also reconciled against
    // the download archive, which is what a skipped item leaves behind.
    markJobChildrenFromDisk(id, /*includePaused=*/true);

    int completed = 0, failed = 0, cancelled = 0;
    for (int cid : job.childIds) {
        if (!downloads.contains(cid)) continue;
        DownloadItem& child = downloads[cid];
        if (child.status == "Cancelled") { cancelled++; continue; }

        const QFileInfo info(child.filePath);
        if (info.exists() && info.size() > 0) {
            if (child.status != "Completed") {
                child.status = "Completed";
                child.progress = 100.0;
                child.completedAt = QDateTime::currentDateTime();
                child.speed = 0;
                child.error.clear();
                DatabaseManager::instance().updateDownload(child);
                emit statusChanged(cid, "Completed");
            }
            // The file on disk is the truth for both numbers, so the folder's
            // "downloaded / total" always adds up and never reads as 209 MB / 99 MB.
            child.downloadedSize = info.size();
            child.totalSize = info.size();
            completed++;
        } else if (child.status == "Completed") {
            // Marked complete from the archive but the file is gone.
            child.status = "Failed";
            child.error = "The downloaded file is missing.";
            failed++;
            DatabaseManager::instance().updateDownload(child);
            emit statusChanged(cid, "Failed");
        } else {
            child.status = "Failed";
            child.speed = 0;
            child.error = error.isEmpty() ? "Item could not be downloaded." : error;
            failed++;
            DatabaseManager::instance().updateDownload(child);
            emit statusChanged(cid, "Failed");
        }
    }

    if (failed > 0 && completed == 0) {
        // Nothing at all got downloaded - report the job as failed so the
        // automatic retry policy can take a second run at it.
        job.status = "Failed";
        job.error = error;
        job.speed = 0;
        DatabaseManager::instance().updateDownload(job);
        Logger::instance().error("Playlist job failed (" + QString::number(failed) + " item(s)): " + error);
        emit downloadFailed(id, error);
        emit statusChanged(id, "Failed");
        scheduleRetry(id, error);
    } else {
        // A partial success is still a success: the failed items are individually
        // retryable and the folder must not be shown as a failed transfer.
        job.status = "Completed";
        // Cancelled items are not part of what the job was asked to deliver, so
        // they stay out of the denominator - otherwise a job the user stopped
        // early could never read as 100%.
        const int settled = completed + failed;
        job.progress = settled > 0 ? (double)completed / settled * 100.0 : 100.0;
        job.downloadedSize = 0;
        job.totalSize = 0;
        for (int cid : job.childIds) {
            if (!downloads.contains(cid)) continue;
            job.downloadedSize += downloads[cid].downloadedSize;
            job.totalSize += downloads[cid].totalSize;
        }
        job.completedAt = QDateTime::currentDateTime();
        job.speed = 0;
        job.error = failed > 0 ? QString("%1 of %2 item(s) could not be downloaded. Right-click the item and choose Retry.")
                                     .arg(failed).arg(settled)
                               : QString();
        DatabaseManager::instance().updateDownload(job);
        Logger::instance().info("Playlist job finished: " + QString::number(completed) + " completed, " +
                                QString::number(failed) + " failed, " + QString::number(cancelled) + " cancelled");
        emit downloadFinished(id);
        emit statusChanged(id, "Completed");
    }

    lastProgressToDbMs.remove(id);
    startNextQueued();
}

void DownloadManager::onYtDlpFinished(int id) {
    if (!downloads.contains(id)) return;
    // A playlist job settles itself in onYtDlpPlaylistFinished(), which also has
    // to resolve the individual items.
    if (downloads[id].isFolder) return;
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

    if (item.parentId != -1) {
        updateAggregateProgress(item.parentId);
    }

    startNextQueued();
}

void DownloadManager::onYtDlpFailed(int id, const QString& error) {
    if (!downloads.contains(id)) return;
    // Playlist jobs report their outcome through playlistFinished().
    if (downloads[id].isFolder) return;
    DownloadItem& item = downloads[id];

    if (item.status == "Cancelled") return;

    item.status = "Failed";
    item.error = error;
    item.speed = 0;
    item.eta = 0;
    // Keep the partial file: a retry continues it instead of starting over.
    DatabaseManager::instance().updateDownload(item);
    Logger::instance().error("yt-dlp download failed: " + QString::number(id) + " - " + error);
    emit downloadFailed(id, error);
    emit statusChanged(id, "Failed");

    if (item.parentId != -1) {
        updateAggregateProgress(item.parentId);
    }

    startNextQueued();

    // Automatic retry with backoff. Playlist items are not re-queued on their own:
    // they are re-fetched by re-running their job, which skips the items its
    // download archive already recorded.
    if (item.parentId == -1) scheduleRetry(id, error);
}

void DownloadManager::scheduleRetry(int id, const QString& error) {
    if (!downloads.contains(id)) return;
    DownloadItem& item = downloads[id];
    if (item.status == "Cancelled" || item.status == "Completed") return;

    int limit = maxRetries();
    if (limit <= 0 || item.attempts >= limit) {
        Logger::instance().info("Not retrying download " + QString::number(id) + ": attempt limit reached");
        return;
    }

    item.attempts++;
    item.status = "Queued";
    item.error = error;
    DatabaseManager::instance().updateDownload(item);
    emit statusChanged(id, "Queued");

    // Back off between attempts so a throttled or briefly unreachable source is
    // not hammered immediately again.
    int delaySec = qMin(5 * (1 << (item.attempts - 1)), 120);
    if (QTimer* existing = retryTimers.take(id)) {
        existing->stop();
        existing->deleteLater();
    }
    QTimer* timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, id, timer]() {
        timer->deleteLater();
        retryTimers.remove(id);
        if (!downloads.contains(id)) return;
        if (downloads[id].status != "Queued") return;   // paused/cancelled meanwhile
        retryDownload(id, /*resetAttempts=*/false);
    });
    retryTimers.insert(id, timer);
    timer->start(delaySec * 1000);

    Logger::instance().info("Scheduled automatic retry " + QString::number(item.attempts) + "/" +
                            QString::number(limit) + " for download " + QString::number(id) +
                            " in " + QString::number(delaySec) + "s");
}

void DownloadManager::retryDownload(int id, bool resetAttempts) {
    if (!downloads.contains(id)) return;

    // Retrying an item inside a folder re-queues the folder instead: the folder
    // is the real transfer, and starting a second process for one of its items
    // would fight the job that owns it.
    if (downloads[id].parentId != -1) {
        int parentId = downloads[id].parentId;
        if (downloads.contains(parentId) &&
            (downloads[parentId].type == "Torrent" || downloads[parentId].type == "YtDlp")) {
            retryDownload(parentId);
            return;
        }
    }

    DownloadItem& item = downloads[id];
    if (item.status == "Downloading") return;
    if (item.status == "Completed" && !item.isFolder) return;

    if (QTimer* pending = retryTimers.take(id)) {
        pending->stop();
        pending->deleteLater();
    }

    // A retry the user asked for starts a fresh attempt budget; an automatic retry
    // keeps counting, so the retry limit is actually reached.
    if (resetAttempts) {
        item.attempts = 0;
    }
    item.error.clear();
    item.completedAt = QDateTime();

    if (item.isFolder) {
        bool anyFailedChild = false;
        for (int cid : item.childIds) {
            if (downloads.contains(cid) && downloads[cid].status == "Failed") anyFailedChild = true;
        }
        // Re-running a fully finished job is a deliberate re-download, so the
        // archive that makes yt-dlp skip finished items is dropped. When only some
        // items failed it is kept, and the retry re-fetches just those.
        if (item.status == "Completed" && !anyFailedChild) {
            QFile::remove(jobArchivePath(item.filePath));
        }
        for (int cid : item.childIds) {
            if (!downloads.contains(cid)) continue;
            DownloadItem& child = downloads[cid];
            child.status = "Queued";
            child.progress = 0;
            child.downloadedSize = 0;
            child.totalSize = 0;
            child.speed = 0;
            child.eta = 0;
            if (resetAttempts) {
                child.attempts = 0;
            }
            child.error.clear();
            emit statusChanged(cid, "Queued");
        }
    }

    item.status = "Queued";
    DatabaseManager::instance().updateDownload(item);
    emit statusChanged(id, "Queued");

    // A retry is an explicit user action, so it starts now rather than waiting for
    // the automatic backoff - unless every yt-dlp job slot is busy.
    if (item.type == "YtDlp" && !ytDlpSlotAvailable()) {
        Logger::instance().info("Retry of " + QString::number(id) + " stays queued: all yt-dlp job slots busy");
        startNextQueued();
        return;
    }
    resumeDownload(id);
}

void DownloadManager::retryAllFailed() {
    QVector<DownloadItem> failed = getDownloadsByStatus("Failed");
    QSet<int> retried;
    int count = 0;
    for (const DownloadItem& item : failed) {
        // A folder child is retried through its folder, so several failed items of
        // the same job must not start the job several times.
        int target = item.id;
        if (item.parentId != -1 && downloads.contains(item.parentId) &&
            (downloads[item.parentId].type == "Torrent" || downloads[item.parentId].type == "YtDlp")) {
            target = item.parentId;
        }
        if (retried.contains(target)) continue;
        retried.insert(target);
        retryDownload(target);
        count++;
    }
    Logger::instance().info("Retried " + QString::number(count) + " failed download(s)");
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
