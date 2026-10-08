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
    qint64 endByte = -1;
    qint64 downloaded = 0;
    // Byte offset the in-flight request actually asked for (startByte +
    // downloaded on resume); the response's Content-Range must match it.
    qint64 requestStart = -1;
    // Whether the in-flight request carried a Range header; only then does a
    // 206 + exact Content-Range contract exist to validate.
    bool requestedRange = false;
    QNetworkReply* reply = nullptr;
    QFile* file = nullptr;
    bool error = false;
    // Set once the response headers of the in-flight request have been checked
    // (206 + exact Content-Range); no body byte is consumed before that.
    bool responseValidated = false;
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
    bool startChunkRequest(ChunkState& chunk);
    bool validateChunkResponse(ChunkState& chunk);
    bool mergeChunks();
    void cleanupChunks();
    void cleanupTempFiles();
    QString chunkFilePath(int index) const;
    QString resumeStatePath() const;
    QNetworkRequest buildChunkRequest(qint64 fromByte, qint64 toByte) const;
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
    // Validators from the HEAD/fallback response, sent as If-Range on resumed
    // requests so a remote file that changed between sessions cannot be
    // silently appended to stale partial chunks (the server answers 200 OK,
    // which the response validation treats as a range violation).
    QString respEtag;
    QString respLastModified;
    bool pendingRangeViolation = false;   // a 200-for-ranged response arrived
    bool rangeViolationRestarted = false; // the one clean restart was consumed
    bool forceNoRange = false;            // learned: server ignores Range headers
    // Attach If-Range validators to chunk requests (resume paths only, where
    // stale partial data exists); fresh downloads must not send it.
    bool attachingIfRange = false;
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
