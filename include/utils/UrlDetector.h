#ifndef URLDETECTOR_H
#define URLDETECTOR_H

#include <QString>
#include <QHash>

enum UrlType {
    UrlUnknown,
    UrlDirect,
    UrlPlaylist,
    UrlTorrent,
    UrlYtDlp
};

class UrlDetector {
public:
    static UrlType detect(const QString& url);
    static bool isDirectUrl(const QString& url);
    static bool isPlaylistUrl(const QString& url);
    static bool isTorrentUrl(const QString& url);
    static bool isYtDlpUrl(const QString& url);
    static QString detectContentCategory(const QString& url);
    static bool isAllowedByDownloadType(const QString& url, const QHash<QString, QString>& modes);
    static QString typeToString(UrlType type);
};

#endif
