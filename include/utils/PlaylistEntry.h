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
};

#endif
