#ifndef ARIA2CMANAGER_H
#define ARIA2CMANAGER_H

#include <QObject>
#include <QString>
#include <QProcess>
#include <QVector>
#include <QMap>
#include <functional>
#include "utils/PlaylistEntry.h"
#include "utils/TorrentInfo.h"

class QNetworkAccessManager;
class QNetworkReply;

struct Aria2cDownloadTask {
    int id = 0;
    QString url;
    QString outputPath;
    QProcess* process = nullptr;
    bool isRunning = false;
    qint64 downloadedBytes = 0;
    qint64 totalBytes = 0;
    qint64 speed = 0;
    int connectedPeers = 0;
    int seeds = 0;
};

class Aria2cManager : public QObject {
    Q_OBJECT
public:
    static Aria2cManager& instance();
    bool isInstalled();
    QString getVersion();
    void installOrUpdate();
    QString getAria2cPath();
    bool ensureInstalled();
    int addTorrent(const QString& magnetOrFile, const QString& savePath);
    int addTorrentWithSelection(const QString& magnetOrFile, const QString& savePath, const QVector<int>& selectedIndices);
    void fetchTorrentFiles(const QString& magnetOrFile, std::function<void(const QVector<PlaylistEntry>&, const TorrentInfo&)> callback);
    void pauseDownload(int id);
    void resumeDownload(int id);
    void removeDownload(int id);
    void addTrackers(int torrentId, const QStringList& trackers);
    bool isRunning(int id) const;
    int getConnectedPeers(int id) const;
    int getSeeds(int id) const;

signals:
    void downloadProgress(int id, qint64 downloaded, qint64 total, qint64 speed);
    void downloadFinished(int id);
    void downloadFailed(int id, const QString& error);
    void installationProgress(const QString& status);
    void errorOccurred(const QString& error);

private:
    Aria2cManager();
    int getNextId();
    void parseProgress(const QString& line, int id);
    void parseFileList(const QByteArray& output, QVector<PlaylistEntry>& entries);
    void fetchMagnetMetadata(const QString& magnet, std::function<void(const QVector<PlaylistEntry>&, const TorrentInfo&)> callback);
    void fetchTorrentFileList(const QString& torrentPath, std::function<void(const QVector<PlaylistEntry>&, const TorrentInfo&)> callback);
    QString getToolsDir();
    void startDownload(const QString& url, const QString& fileName);
    bool extractAria2c(const QString& zipPath);

    QMap<int, Aria2cDownloadTask> tasks;
    int nextId;
    int maxConcurrent;
    bool isDownloading;
    QNetworkAccessManager* nam;
    QNetworkReply* activeReply;
};

#endif
