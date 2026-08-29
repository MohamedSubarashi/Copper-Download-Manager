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
};

#endif
