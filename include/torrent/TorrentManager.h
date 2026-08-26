#ifndef TORRENTMANAGER_H
#define TORRENTMANAGER_H

#include <QObject>
#include <QString>
#include <QVector>
#include <functional>
#include "utils/PlaylistEntry.h"
#include "utils/TorrentInfo.h"

class TorrentManager : public QObject {
    Q_OBJECT
public:
    static TorrentManager& instance();
    int addTorrent(const QString& magnetOrFile, const QString& savePath);
    void fetchFileInfo(const QString& magnetOrFile, std::function<void(const QVector<PlaylistEntry>&, const TorrentInfo&)> callback);
    void pauseDownload(int id);
    void resumeDownload(int id);
    void removeDownload(int id);
    void addTrackers(int torrentId, const QStringList& trackers);
    QStringList getDefaultTrackers() const;

signals:
    void downloadProgress(int id, qint64 downloaded, qint64 total, qint64 speed);
    void downloadFinished(int id);
    void downloadFailed(int id, const QString& error);

private:
    TorrentManager();
};

#endif
