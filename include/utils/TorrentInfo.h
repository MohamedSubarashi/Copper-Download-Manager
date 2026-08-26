#ifndef TORRENTINFO_H
#define TORRENTINFO_H

#include <QString>
#include <QStringList>
#include <QVector>

struct TorrentInfo {
    QString name;
    QString infoHash;
    QString magnetUri;
    QString totalSize;
    qint64 totalSizeBytes = 0;
    int pieceLength = 0;
    int numberOfPieces = 0;
    QStringList trackers;
    int fileCount = 0;

    int connectedPeers = 0;
    int seeds = 0;
};

#endif
