#ifndef DOWNLOADMANAGER_H
#define DOWNLOADMANAGER_H

#include <QObject>
#include <QVector>
#include <QMap>
#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QTimer>
#include <QProcess>
#include "core/DownloadItem.h"
#include "utils/PlaylistEntry.h"

class ChunkedDownloader;

class DownloadManager : public QObject {
    Q_OBJECT
public:
    static DownloadManager& instance();

    static bool isFileFormatAllowed(const QString& url);
    static bool fileFormatFilterEnabled();
    static QStringList includeFileFormats();
    static QStringList excludeFileFormats();

    int addDownload(const QString& url, const QString& path, const QString& type, int chunks = 16, const QString& audioFormat = "mp4");
    // Creates the folder row plus one row per selected item and returns the folder
    // (job) id. playlistUrl is the real playlist URL: a yt-dlp playlist is driven
    // by a single process pointed at it, not at the per-item URLs.
    int addPlaylistDownload(const QVector<PlaylistEntry>& entries, const QString& path, const QString& type, bool useTrackNumbers = true, const QString& audioFormat = "mp4", const QString& torrentSourceUrl = "", const QString& folderName = "", const QString& playlistUrl = "");
    void pauseDownload(int id);
    void resumeDownload(int id);
    void cancelDownload(int id);
    void removeDownload(int id);
    void pauseAll();
    void resumeAll();
    void clearCompleted();
    // resetAttempts=false is used by the automatic retry, which must keep counting
    // towards the limit; a user-requested retry starts a fresh attempt budget.
    void retryDownload(int id, bool resetAttempts = true);
    void retryAllFailed();
    QVector<DownloadItem> getDownloads() const;
    QVector<DownloadItem> getDownloadsByStatus(const QString& status);
    DownloadItem getDownload(int id) const;
    void updateMaxConcurrent(int max);
    void setSpeedLimit(qint64 bytesPerSecond);
    qint64 getSpeedLimit() const;
    int maxRetries() const;
    void addChildDownload(int parentId, const QString& url, const QString& path, const QString& type, const QString& audioFormat = "mp4");
    void restoreFromDatabase();
    void shutdown();

    // Download-item introspection for the per-download info window.
    int childCount(int id) const;

signals:
    void downloadAdded(int id, const QString& path, const QString& type, bool isFolder);
    void downloadProgress(int id, qint64 downloaded, qint64 total);
    void downloadFinished(int id);
    void downloadFailed(int id, const QString& error);
    void downloadSpeed(int id, qint64 speed);
    void downloadRemoved(int id);
    void downloadPaused(int id);
    void downloadResumed(int id);
    void totalSpeedUpdated(qint64 speed);
    void statusChanged(int id, const QString& status);

private:
    DownloadManager();
    ~DownloadManager();

    void startNextQueued();
    void updateAggregateProgress(int parentId);
    void createChunkedDownloaderFor(int id, bool resumeFromSaved);
    void onChunkProgress(int id, qint64 downloaded, qint64 total);
    void onChunkFinished(int id);
    void onChunkFailed(int id, const QString& error);
    void onChunkSpeed(int id, qint64 speed);
    void onYtDlpProgress(int id, qint64 downloaded, qint64 total);
    void onYtDlpSpeed(int id, qint64 speed);
    void onYtDlpFinished(int id);
    void onYtDlpFailed(int id, const QString& error);
    void onYtDlpVideoProgress(int id, const QString& videoId, const QString& state,
                              qint64 downloaded, qint64 total, qint64 speed, qint64 eta,
                              int fragmentIndex, int fragmentCount);
    void onYtDlpVideoPath(int id, int playlistIndex, const QString& path);
    void onYtDlpPlaylistFinished(int id, bool ok, const QString& error);

    // Starts (or restarts) the single yt-dlp process that drives a playlist job.
    void startYtDlpPlaylistJob(int jobId);
    int ytDlpPlaylistJobWidth(int jobId) const;
    int findChildByVideoId(int parentId, const QString& videoId) const;
    int findChildByTrackIndex(int parentId, int trackIndex) const;
    int findChildForPath(int parentId, int playlistIndex, const QString& path) const;
    // The name the app guessed for an item can differ from what yt-dlp actually
    // wrote (its own filename sanitizing is the authority), so a missing item is
    // looked up in the job folder by its "<track number>." prefix.
    QString findJobFileForTrack(const QString& folder, int trackIndex) const;
    // Renames a completed file that still carries a yt-dlp fragment suffix
    // ("title.f616.mp4") to its final name, and returns the resulting path.
    QString tidyJobFilePath(const QString& path);
    // True for a row that only mirrors a yt-dlp playlist job. Such a row must never
    // start a transfer of its own - the job's single process is the transfer.
    bool isPlaylistJobItem(int id) const;
    int activeYtDlpJobCount() const;
    bool ytDlpSlotAvailable() const;
    void markJobChildrenFromDisk(int jobId, bool includePaused = false);
    void scheduleRetry(int id, const QString& error);
    void noteYtDlpChildFinished(int id, int childId, const QString& path);

    void processSpeedLimit();
    void applySpeedLimitToDownloaders();

    QMap<int, DownloadItem> downloads;
    QMap<int, ChunkedDownloader*> activeChunkedDownloaders;
    // Live aria2 signal connections per torrent download (progress/finished/
    // failed). Cleared on re-resume so connections never accumulate across the
    // many times a paused/restarted torrent gets resumed.
    QHash<int, QList<QMetaObject::Connection>> ariaConnectionHandles;
    QHash<int, qint64> lastProgressToDbMs;
    // Automatic retry: a failed transfer is re-queued after a backoff instead of
    // staying red forever. Ids live here while their backoff timer runs.
    QHash<int, QTimer*> retryTimers;
    int nextId;
    int maxConcurrent;
    qint64 speedLimit;
    QTimer* speedLimitTimer;
    qint64 speedLimitAccumulator;
};

#endif
