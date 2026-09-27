#ifndef PLAYLISTENTRY_H
#define PLAYLISTENTRY_H

#include <QString>

struct PlaylistEntry {
    int index = 0;
    QString url;
    QString title;
    QString fileSize;
    qint64 fileSizeBytes = 0;
    bool selected = true;
    QString extension;
    // Extractor id of the individual item (e.g. the YouTube video id). Playlist
    // downloads run as ONE yt-dlp process, and its progress lines carry %(info.id)s,
    // so this is what lets a progress line be attributed to the right row.
    QString videoId;
    qint64 duration = 0;
};

#endif
