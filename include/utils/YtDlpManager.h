#ifndef YTDLPMANAGER_H
#define YTDLPMANAGER_H

#include <QObject>
#include <QString>
#include <QMap>
#include <QProcess>
#include <QTimer>
#include <QVector>
#include <functional>
#include "utils/PlaylistEntry.h"

class QNetworkAccessManager;
class QNetworkReply;

// One yt-dlp invocation. A playlist is driven by a SINGLE process (one job) so
// the engine never has N extractors hitting the same site at once, and yt-dlp
// itself resumes .part files, retries and skips dead items.
struct YtDlpJobSpec {
    QString url;              // video or playlist URL
    QString outputTemplate;   // value passed to -o
    QString format;           // mp3 / mp4 / mkv / best
    bool isPlaylist = false;
    int fragments = 1;        // -N concurrent fragments (playlist jobs)
    // 1-based playlist positions to download, e.g. {1,4,7}. Empty means the whole
    // playlist. This is what makes a partial selection in the item picker work:
    // yt-dlp is pointed at the playlist URL, so without --playlist-items it would
    // fetch every video and write files that have no table row.
    QVector<int> selectedItems;
    // Download archive for a playlist job: yt-dlp records every item that
    // finished here, so restarting the job skips them and only fetches the rest.
    QString archivePath;
};

struct YtDlpJob {
    QProcess* process = nullptr;
    QTimer* stallTimer = nullptr;
    YtDlpJobSpec spec;
    QString outBuf;           // partial stdout line
    QString errBuf;           // partial stderr line
    QString errorTail;        // recent stderr, used for failure messages
    bool sawOutput = false;   // has the child printed anything yet?
    bool paused = false;      // paused => keep the .part files, kill the process
    bool cancelled = false;   // ignore late signals from a torn-down child
    int stallTrips = 0;       // consecutive stall timeouts (first one only warns)
};

class YtDlpManager : public QObject {
    Q_OBJECT
public:
    static YtDlpManager& instance();
    bool isInstalled();
    QString getVersion();
    void installOrUpdate();
    QString getYtDlpPath();

    // Single-video download. outputPath may be a literal file path or a template.
    void startDownload(const QString& url, const QString& outputPath, int downloadId, const QString& format = "mp4");

    // Whole-playlist download driven by ONE yt-dlp process. Per-video progress is
    // reported through videoProgress()/videoPath() so the table can still show one
    // row per item. selectedItems holds the 1-based playlist positions the user
    // actually picked; pass an EMPTY list to download the whole playlist (callers
    // must never do that as a fallback for a selection they could not read).
    // trackNumbers selects the "<NNN>.<title>.<ext>" output template; without it
    // yt-dlp writes "<title>.<ext>". Note that yt-dlp can only number by playlist
    // position, so with trackNumbers on the app renames each file to its position
    // in the selection once it lands.
    void startPlaylistJob(int jobId, const QString& playlistUrl, const QString& outputDir,
                          int trackNumberWidth, const QString& format, int fragments = 4,
                          const QVector<int>& selectedItems = QVector<int>(),
                          bool trackNumbers = true);

    void pauseDownload(int id);
    void resumeDownload(int id);
    void cancelDownload(int id);
    bool isRunning(int id) const;
    bool isPaused(int id) const;
    QString getOutputTemplate(const QString& outputPath) const;

    void fetchVideoInfo(const QString& url, std::function<void(const QString&)> callback);
    void fetchPlaylistInfo(const QString& url, std::function<void(const QVector<PlaylistEntry>&)> callback);

signals:
    void installationProgress(const QString& status);
    void downloadProgress(int id, qint64 downloaded, qint64 total);
    void downloadSpeed(int id, qint64 bytesPerSecond);
    void downloadFinished(int id);
    void downloadFailed(int id, const QString& error);

    // Per-video progress inside a playlist job.
    // state: "downloading" | "finished" | "error"
    void videoProgress(int id, const QString& videoId, const QString& state,
                       qint64 downloaded, qint64 total, qint64 speed, qint64 eta,
                       int fragmentIndex, int fragmentCount);
    // Real on-disk path of a finished file (playlistIndex < 0 for single videos).
    void videoPath(int id, int playlistIndex, const QString& path);
    void playlistItemStarted(int id, int index, int total);
    void playlistFinished(int id, bool ok, const QString& error);

    void errorOccurred(const QString& error);

private:
    YtDlpManager();
    QString getToolsDir();
    void startBinaryDownload(const QString& url, const QString& fileName);

    void launchJob(int id, const YtDlpJobSpec& spec);
    void drainOutput(int id, bool isStdErr);
    void handleLine(int id, const QString& line, bool isStdErr);
    void handleProtocolLine(int id, const QString& line);
    void finalizeJob(int id, int exitCode);
    void destroyJob(int id);
    void armStallWatchdog(int id);
    void stopStallWatchdog(int id);
    bool hasFfmpegFor(const QString& format) const;

    QMap<int, YtDlpJob> jobs;
    bool isDownloading;
    QNetworkAccessManager* nam;
    QNetworkReply* activeReply;
};

#endif
