#ifndef CHUNKEDDOWNLOADER_H
#define CHUNKEDDOWNLOADER_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QTimer>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QJsonObject>

struct ChunkState {
    int index = 0;
    qint64 startByte = 0;
    qint64 endByte = 0;
    qint64 downloaded = 0;
    QNetworkReply* reply = nullptr;
    QFile* file = nullptr;
    bool error = false;
    QString errorMessage;
};

class ChunkedDownloader : public QObject {
    Q_OBJECT
public:
    explicit ChunkedDownloader(QObject* parent = nullptr);
    ~ChunkedDownloader();

    void startDownload(const QString& url, const QString& filePath, int chunks = 16, int downloadId = 0);
    void resumeFromState(const QString& url, const QString& filePath, int chunks, qint64 totalSize, bool range, int downloadId);
    void pause();
    void resume();
    void cancel();
    void discardPartialData();
    bool isDownloading() const;
    bool isPaused() const;
    qint64 getDownloadedBytes() const;
    qint64 getTotalBytes() const;
    qint64 getSpeed() const;
    void setSpeedLimit(qint64 bytesPerSecond);

    // Persists the metadata needed to resume this download after an app
    // restart (url, target path, chunk count, total size, range support).
    void persistResumeState() const;
    // Returns true when a previous session left resumable .chunk data on disk.
    static bool hasPersistedData(int downloadId);
    // Reads the saved resume metadata for a download id (empty object if absent).
    static QJsonObject readPersistedState(int downloadId);

signals:
    void downloadProgress(int id, qint64 downloaded, qint64 total);
    void downloadFinished(int id);
    void downloadFailed(int id, const QString& error);
    void speedUpdated(qint64 speed);
    void filePathChanged(int id, const QString& newPath);

private slots:
    void onChunkReadyRead();
    void onChunkFinished();
    void onChunkError(QNetworkReply::NetworkError error);
    void onSpeedTimer();
    void onHeadFinished();
    void onDrainTimer();
    void onHangTimer();

private:
    void setupChunks(qint64 totalSize);
    void startChunkRequests(const QString& url, const QString& filePath, int id);
    void mergeChunks();
    void cleanupChunks();
    void cleanupTempFiles();
    QString chunkFilePath(int index);
    QString resumeStatePath() const;
    void checkFallbackReply(QNetworkReply* reply);
    QString extractFilenameFromContentDisposition(const QByteArray& header);
    QString extractUrlFromHtml(const QByteArray& html);
    bool isHtmlResponse(QNetworkReply* reply);
    void refreshThrottleBudget();
    void drainAvailableData(qint64 maxBytes);
    void resetThrottleState();

    QString downloadUrl;
    QString saveFilePath;
    int totalChunks;
    int downloadId;
    bool downloading;
    bool paused;
    bool cancelled;
    bool supportsRange;
    qint64 totalBytes;
    qint64 downloadedBytes;
    qint64 lastSpeedBytes;
    qint64 speed;
    qint64 lastActivityMs;

    QNetworkAccessManager* nam;
    QVector<ChunkState> chunks;
    QTimer* speedTimer;
    QTimer* hangTimer;
    QNetworkReply* headReply;

    qint64 limitBytesPerSec;
    QElapsedTimer throttleTimer;
    qint64 throttleBudget;
    qint64 throttleRemaining;
    QTimer* drainTimer;
    bool throttleActive;
};

#endif
