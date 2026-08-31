#ifndef ARIA2CMANAGER_H
#define ARIA2CMANAGER_H

#include <QObject>
#include <QString>
#include <QProcess>
#include <QVector>
#include <QMap>
#include <QTimer>
#include <QSet>
#include <QJsonObject>
#include <functional>
#include "utils/PlaylistEntry.h"
#include "utils/TorrentInfo.h"
#include "utils/PeerInfo.h"

class QNetworkAccessManager;
class QNetworkReply;

struct Aria2cTracker {
    QString url;
    QString status;      // used/error/not-pending
    int index = -1;
};

struct Aria2cDownloadTask {
    int id = 0;
    QString url;             // magnet / torrent path
    QString outputPath;      // save directory
    QString gid;             // aria2 GID (hex string)
    QString infoHash;
    QString torrentName;
    QProcess* process = nullptr;   // null for RPC-managed torrents (daemon runs separately)
    bool isRunning = false;
    bool isTorrent = true;
    bool finishedEmitted = false;
    bool failedEmitted = false;
    bool terminal = false;         // complete/error/removed reached; stop polling

    qint64 downloadedBytes = 0;
    qint64 totalBytes = 0;
    qint64 speed = 0;         // download speed
    qint64 uploadSpeed = 0;
    qint64 uploadedBytes = 0;
    int connectedPeers = 0;
    int leechers = 0;
    int seeds = 0;
    QStringList trackers;
    QVector<PeerInfo> peers;
};

class Aria2cManager : public QObject {
    Q_OBJECT
public:
    static Aria2cManager& instance();

    // Installation / version
    bool isInstalled();
    QString getVersion();
    void installOrUpdate();
    QString getAria2cPath();
    bool ensureInstalled();

    // RPC daemon lifetime
    bool ensureDaemon();
    bool daemonRunning() const;
    void shutdownDaemon();
    QString daemonToken() const { return m_token; }
    bool rpcAvailable() const { return m_daemonRunning; }

    // Downloads (RPC backed)
    int addTorrent(const QString& magnetOrFile, const QString& savePath);
    int addTorrentWithSelection(const QString& magnetOrFile, const QString& savePath, const QVector<int>& selectedIndices);
    void fetchTorrentFiles(const QString& magnetOrFile, std::function<void(const QVector<PlaylistEntry>&, const TorrentInfo&)> callback);

    void pauseDownload(int id);
    void resumeDownload(int id);
    void removeDownload(int id);
    bool isRunning(int id) const;

    // Torrent runtime state
    int getConnectedPeers(int id) const;
    int getLeechers(int id) const;
    int getSeeds(int id) const;
    qint64 getUploadSpeed(int id) const;
    qint64 getUploadedBytes(int id) const;
    qint64 getTotalBytes(int id) const;
    qint64 getDownloadedBytes(int id) const;
    qint64 getSpeed(int id) const;
    QString getInfoHash(int id) const;
    QVector<PeerInfo> getPeers(int id) const;
    QStringList getTrackers(int id) const;
    QStringList getTrackerList(int id) const;
    QString getGid(int id) const;

    // Tracker / seed / control
    void addTrackers(int torrentId, const QStringList& trackers);
    void addTrackerToTorrent(int torrentId, const QString& tracker);
    void removeTrackerFromTorrent(int torrentId, const QString& tracker);
    void seedTorrent(int torrentId, int seedTimeMinutes);   // -1 = forever, 0 = no seed, >0 minutes
    void cancelSeeding(int torrentId);

signals:
    void downloadProgress(int id, qint64 downloaded, qint64 total, qint64 speed);
    void downloadFinished(int id);
    void downloadFailed(int id, const QString& error);
    void installationProgress(const QString& status);
    void errorOccurred(const QString& error);
    void torrentStateUpdated(int id);          // peers/seeds/upload/trackers changed
    void daemonStateChanged(bool running);

private:
    Aria2cManager();
    ~Aria2cManager();

    QJsonObject rpcCall(const QString& method, const QJsonArray& params, int timeoutMs = 8000);
    QJsonValue rpcResult(const QString& method, const QJsonArray& params, int timeoutMs = 8000);
    bool startDaemonProcess();
    bool killProcessOnTcpPort(int port);
    void poll();
    int getNextId();
    QString seedTimeArg() const;

    void parseTorrentStatus(int id, const QJsonObject& status);
    void parsePeers(int id);

    void fetchMagnetMetadata(const QString& magnet, std::function<void(const QVector<PlaylistEntry>&, const TorrentInfo&)> callback, int attempt = 0);
    void fetchTorrentFileList(const QString& torrentPath, std::function<void(const QVector<PlaylistEntry>&, const TorrentInfo&)> callback);
    void fetchRemoteTorrentFile(const QString& url, std::function<void(const QVector<PlaylistEntry>&, const TorrentInfo&)> callback);

    // Old one-shot helper (kept only for install path)
    QString getToolsDir();
    void startDownload(const QString& url, const QString& fileName);
    bool extractAria2c(const QString& zipPath);

public:
    // Resolve/download a torrent source (local path, magnet, or remote .torrent URL)
    // to the local path that aria2 should seed from. For magnets, localPath is empty
    // because aria2 handles magnet links natively.
    void resolveTorrentSource(const QString& url, std::function<void(const QString& localPath, const QString& error)> callback);
    void downloadTorrentFile(const QString& url, std::function<void(const QString& localPath, const QString& error)> callback);

    QMap<int, Aria2cDownloadTask> tasks;
    QMap<QString, int> taskByGid;
    int nextId;
    int maxConcurrent;
    bool isDownloading;
    bool m_daemonRunning = false;
    bool m_daemonStarting = false;
    bool m_rpcUnauthorized = false;
    QString m_token;
    QProcess* m_daemonProcess = nullptr;
    QNetworkAccessManager* nam;
    QNetworkReply* activeReply;
    QTimer* pollTimer;
    int pollTick = 0;
};

#endif
