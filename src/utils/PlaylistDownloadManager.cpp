#include "utils/PlaylistDownloadManager.h"
#include "utils/YtDlpManager.h"
#include "utils/Logger.h"
#include "core/DownloadManager.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

PlaylistDownloadManager::PlaylistDownloadManager() {}

PlaylistDownloadManager& PlaylistDownloadManager::instance() {
    static PlaylistDownloadManager instance;
    return instance;
}

void PlaylistDownloadManager::fetchPlaylist(const QString& url, std::function<void(const QVector<PlaylistEntry>&)> callback) {
    Logger::instance().info("Fetching playlist: " + url);
    emit progress("Fetching playlist information...");

    YtDlpManager::instance().fetchPlaylistInfo(url, [this, callback](const QVector<PlaylistEntry>& entries) {
        if (entries.isEmpty()) {
            emit error("No entries found in playlist");
        } else {
            emit progress("Found " + QString::number(entries.size()) + " entries");
        }
        callback(entries);
    });
}

void PlaylistDownloadManager::downloadPlaylist(const QVector<PlaylistEntry>& entries, const QString& outputPath, const QString& type) {
    Logger::instance().info("Starting playlist download: " + QString::number(entries.size()) + " files");

    int selectedCount = 0;
    for (const PlaylistEntry& entry : entries) {
        if (entry.selected) selectedCount++;
    }

    emit progress("Starting download of " + QString::number(selectedCount) + " files...");
    DownloadManager::instance().addPlaylistDownload(entries, outputPath, type);
}
