#ifndef YTDLPMANAGER_H
#define YTDLPMANAGER_H

#include <QObject>
#include <QString>
#include <QMap>
#include <QProcess>
#include <QTimer>
#include <functional>
#include <QVector>
#include "utils/PlaylistEntry.h"

class QNetworkAccessManager;
class QNetworkReply;

class YtDlpManager : public QObject {
    Q_OBJECT
public:
    static YtDlpManager& instance();
    bool isInstalled();
    QString getVersion();
    void installOrUpdate();
    QString getYtDlpPath();
    void startDownload(const QString& url, const QString& outputPath, int downloadId, const QString& format = "mp4");
    void pauseDownload(int id);
    void resumeDownload(int id);
    void cancelDownload(int id);
    void fetchVideoInfo(const QString& url, std::function<void(const QString&)> callback);
    void fetchPlaylistInfo(const QString& url, std::function<void(const QVector<PlaylistEntry>&)> callback);
    bool isRunning(int id) const;

signals:
    void installationProgress(const QString& status);
    void downloadProgress(int id, qint64 downloaded, qint64 total);
    void downloadFinished(int id);
    void downloadFailed(int id, const QString& error);
    void errorOccurred(const QString& error);

private:
    YtDlpManager();
    QString getToolsDir();
    void startBinaryDownload(const QString& url, const QString& fileName);
    void processYtDlpOutput(int id);
    void armStallWatchdog(int id);
    void stopStallWatchdog(int id);

    QMap<int, QProcess*> activeProcesses;
    QMap<int, QTimer*> stallTimers;
    bool isDownloading;
    QNetworkAccessManager* nam;
    QNetworkReply* activeReply;
};

#endif
