#ifndef CHUNKEDDOWNLOADER_H
#define CHUNKEDDOWNLOADER_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>

struct ChunkState {
    int index = 0;
    qint64 startByte = 0;
    qint64 endByte = 0;
    qint64 downloaded = 0;
    QNetworkReply* reply = nullptr;
    QFile* file = nullptr;
};

class ChunkedDownloader : public QObject {
    Q_OBJECT
public:
    explicit ChunkedDownloader(QObject* parent = nullptr);
    ~ChunkedDownloader();

    void startDownload(const QString& url, const QString& filePath, int chunks = 16, int downloadId = 0);
    void pause();
    void resume();
    void cancel();
    bool isDownloading() const;
    bool isPaused() const;
    qint64 getDownloadedBytes() const;
    qint64 getTotalBytes() const;
    qint64 getSpeed() const;
    void setSpeedLimit(qint64 bytesPerSecond);

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

private:
    void setupChunks(qint64 totalSize);
    void mergeChunks();
    void cleanupChunks();
    void cleanupTempFiles();
    QString chunkFilePath(int index);
    void checkFallbackReply(QNetworkReply* reply);
    QString extractFilenameFromContentDisposition(const QByteArray& header);
    QString extractUrlFromHtml(const QByteArray& html);
    bool isHtmlResponse(QNetworkReply* reply);

    QString downloadUrl;
    QString saveFilePath;
    int totalChunks;
    int downloadId;
    bool downloading;
    bool paused;
    bool supportsRange;
    qint64 totalBytes;
    qint64 downloadedBytes;
    qint64 lastSpeedBytes;
    qint64 speed;

    QNetworkAccessManager* nam;
    QVector<ChunkState> chunks;
    QTimer* speedTimer;
    QNetworkReply* headReply;
};

#endif
