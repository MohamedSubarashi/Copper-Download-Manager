#ifndef TRACKNUMBER_H
#define TRACKNUMBER_H

#include <QString>

class TrackNumber {
public:
    static QString formatTrack(int number, int total);
    static QString prefixFileName(int number, int total, const QString& fileName);
};

#endif
