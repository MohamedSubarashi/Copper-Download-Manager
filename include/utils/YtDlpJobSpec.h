#ifndef YTDLPJOBSPEC_H
#define YTDLPJOBSPEC_H

#include <QString>
#include <QVector>

// One yt-dlp invocation. A playlist is driven by a SINGLE process (one job) so
// the engine never has N extractors hitting the same site at once, and yt-dlp
// itself resumes .part files, retries and skips dead items.
//
// Kept in its own header (free of Q_OBJECT) so the command builder can be
// unit-tested without dragging in the manager's process/DB dependencies.
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

#endif  // YTDLPJOBSPEC_H
