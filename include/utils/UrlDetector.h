#ifndef URLDETECTOR_H
#define URLDETECTOR_H

#include <QString>

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
    static bool isAllowedByDownloadType(const QString& url, const QString& mode, const QStringList& selectedFilters);
    static QString typeToString(UrlType type);
};

#endif
