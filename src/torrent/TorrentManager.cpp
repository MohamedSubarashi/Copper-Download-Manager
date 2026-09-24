#include "torrent/TorrentManager.h"
#include "utils/Aria2cManager.h"
#include "utils/Logger.h"
#include "db/DatabaseManager.h"
#include <QStandardPaths>

TorrentManager::TorrentManager() {}

TorrentManager& TorrentManager::instance() {
    static TorrentManager instance;
    return instance;
}

int TorrentManager::addTorrent(const QString& magnetOrFile, const QString& savePath) {
    Logger::instance().info("TorrentManager: Adding torrent: " + magnetOrFile);

    int id = Aria2cManager::instance().addTorrent(magnetOrFile, savePath);

    if (id > 0) {
        connect(&Aria2cManager::instance(), &Aria2cManager::downloadProgress, this, [this, id](int aId, qint64 downloaded, qint64 total, qint64 speed) {
            if (aId == id) emit downloadProgress(id, downloaded, total, speed);
        });
        connect(&Aria2cManager::instance(), &Aria2cManager::downloadFinished, this, [this, id](int aId) {
            if (aId == id) emit downloadFinished(id);
        });
        connect(&Aria2cManager::instance(), &Aria2cManager::downloadFailed, this, [this, id](int aId, const QString& error) {
            if (aId == id) emit downloadFailed(id, error);
        });
    }

    return id;
}

void TorrentManager::fetchFileInfo(const QString& magnetOrFile, std::function<void(const QVector<PlaylistEntry>&, const TorrentInfo&)> callback) {
    Logger::instance().info("TorrentManager: Fetching file info: " + magnetOrFile);
    Aria2cManager::instance().fetchTorrentFiles(magnetOrFile, callback);
}

void TorrentManager::pauseDownload(int id) {
    Aria2cManager::instance().pauseDownload(id);
}

void TorrentManager::resumeDownload(int id) {
    Aria2cManager::instance().resumeDownload(id);
}

void TorrentManager::removeDownload(int id) {
    Aria2cManager::instance().removeDownload(id);
}

void TorrentManager::addTrackers(int torrentId, const QStringList& trackers) {
    Logger::instance().info("TorrentManager: Adding " + QString::number(trackers.size()) + " trackers to torrent " + QString::number(torrentId));
    Aria2cManager::instance().addTrackers(torrentId, trackers);
}
