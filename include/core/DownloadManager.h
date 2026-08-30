#ifndef DOWNLOADMANAGER_H
#define DOWNLOADMANAGER_H

#include <QObject>
#include <QVector>
#include <QMap>
#include <QHash>
#include <QTimer>
#include <QProcess>
#include "core/DownloadItem.h"
#include "utils/PlaylistEntry.h"

class ChunkedDownloader;

class DownloadManager : public QObject {
    Q_OBJECT
public:
    static DownloadManager& instance();

    int addDownload(const QString& url, const QString& path, const QString& type, int chunks = 16);
    void addPlaylistDownload(const QVector<PlaylistEntry>& entries, const QString& path, const QString& type, bool useTrackNumbers = true, const QString& audioFormat = "mp4", const QString& torrentSourceUrl = "", const QString& folderName = "");
    void pauseDownload(int id);
    void resumeDownload(int id);
    void cancelDownload(int id);
    void removeDownload(int id);
    void pauseAll();
    void resumeAll();
    void clearCompleted();
    QVector<DownloadItem> getDownloads() const;
    QVector<DownloadItem> getDownloadsByStatus(const QString& status);
    DownloadItem getDownload(int id) const;
    void updateMaxConcurrent(int max);
    void setSpeedLimit(qint64 bytesPerSecond);
    qint64 getSpeedLimit() const;
    void addChildDownload(int parentId, const QString& url, const QString& path, const QString& type, const QString& audioFormat = "mp4");

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
    void onChunkProgress(int id, qint64 downloaded, qint64 total);
    void onChunkFinished(int id);
    void onChunkFailed(int id, const QString& error);
    void onChunkSpeed(int id, qint64 speed);
    void onYtDlpProgress(int id, qint64 downloaded, qint64 total);
    void onYtDlpFinished(int id);
    void onYtDlpFailed(int id, const QString& error);
    void processSpeedLimit();
    void applySpeedLimitToDownloaders();

    QMap<int, DownloadItem> downloads;
    QMap<int, ChunkedDownloader*> activeChunkedDownloaders;
    QHash<int, qint64> lastProgressToDbMs;
    int nextId;
    int maxConcurrent;
    qint64 speedLimit;
    QTimer* speedLimitTimer;
    qint64 speedLimitAccumulator;
};

#endif
