#ifndef DOWNLOADITEM_H
#define DOWNLOADITEM_H

#include <QString>
#include <QDateTime>
#include <QVector>
#include <QStringList>

struct DownloadItem {
    int id = 0;
    QString url;
    QString filePath;
    QString fileName;
    QString type;
    qint64 downloadedSize = 0;
    qint64 totalSize = 0;
    QString status;
    QDateTime addedAt;
    QDateTime completedAt;
    QString error;
    double progress = 0.0;
    bool isFolder = false;
    int parentId = -1;
    QVector<int> childIds;
    qint64 speed = 0;
    int chunks = 16;
    QString audioFormat;
    QString torrentSourceUrl;
    QVector<int> selectedIndices;
    int aria2cId = -1;
    int connectedPeers = 0;
    int leechers = 0;
    int seeds = 0;
    qint64 uploadSpeed = 0;
    qint64 uploadedSize = 0;
    QString infoHash;
    QStringList trackers;
    // Number of failed attempts so far. Used by the automatic retry policy:
    // a failed transfer is re-queued with a backoff while attempts < maxRetries.
    int attempts = 0;
    // Seconds remaining for this transfer (0 when unknown). yt-dlp reports it in
    // its progress template; the HTTP engine leaves it at 0 and it is derived
    // from speed/remaining bytes in the UI.
    qint64 eta = 0;
    // Playlist items are downloaded by ONE yt-dlp process, so a row is matched to
    // the process's output by the item's extractor id and by its position in the
    // playlist. Both are needed to attribute progress and final paths correctly.
    QString videoId;
    int trackIndex = 0;
};

#endif
