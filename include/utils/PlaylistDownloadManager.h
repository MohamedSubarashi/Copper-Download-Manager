#ifndef PLAYLISTDOWNLOADMANAGER_H
#define PLAYLISTDOWNLOADMANAGER_H

#include <QObject>
#include <QString>
#include <QVector>
#include "utils/PlaylistEntry.h"

class PlaylistDownloadManager : public QObject {
    Q_OBJECT
public:
    static PlaylistDownloadManager& instance();
    void fetchPlaylist(const QString& url, std::function<void(const QVector<PlaylistEntry>&)> callback);
    void downloadPlaylist(const QVector<PlaylistEntry>& entries, const QString& outputPath, const QString& type);

signals:
    void progress(const QString& status);
    void finished();
    void error(const QString& error);

private:
    PlaylistDownloadManager();
};

#endif
