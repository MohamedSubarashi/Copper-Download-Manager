#include "utils/TrackNumber.h"

QString TrackNumber::formatTrack(int number, int total) {
    Q_UNUSED(total);
    if (number < 1000) {
        return QString("%1").arg(number, 3, 10, QChar('0'));
    } else {
        return QString("%1").arg(number, 4, 10, QChar('0'));
    }
}

QString TrackNumber::prefixFileName(int number, int total, const QString& fileName) {
    QString track = formatTrack(number, total);
    return track + "." + fileName;
}
