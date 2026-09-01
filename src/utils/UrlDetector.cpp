#include "utils/UrlDetector.h"
#include <QUrl>
#include <QFileInfo>
#include <QRegularExpression>

UrlType UrlDetector::detect(const QString& url) {
    if (isTorrentUrl(url)) return UrlTorrent;
    if (isPlaylistUrl(url)) return UrlPlaylist;
    if (isYtDlpUrl(url)) return UrlYtDlp;
    if (isDirectUrl(url)) return UrlDirect;
    return UrlUnknown;
}

bool UrlDetector::isDirectUrl(const QString& url) {
    QUrl qurl(url);
    if (!qurl.isValid()) return false;

    QString scheme = qurl.scheme().toLower();
    if (scheme != "http" && scheme != "https" && scheme != "ftp" && scheme != "ftps") return false;

    QString path = qurl.path();
    if (path.isEmpty() || path == "/") return false;

    QStringList videoExts = {".mp4", ".mkv", ".avi", ".mov", ".wmv", ".flv", ".webm", ".m4v", ".mpg", ".mpeg"};
    QStringList audioExts = {".mp3", ".wav", ".flac", ".aac", ".ogg", ".wma", ".m4a", ".opus"};
    QStringList archiveExts = {".zip", ".rar", ".7z", ".tar", ".gz", ".bz2", ".xz"};
    QStringList docExts = {".pdf", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".txt"};

    QString ext = QFileInfo(path).suffix().toLower();
    for (const QString& e : videoExts + audioExts + archiveExts + docExts) {
        if (ext == e.mid(1)) return true;
    }

    return true;
}

bool UrlDetector::isPlaylistUrl(const QString& url) {
    QString lower = url.toLower();

    if (lower.contains("youtube.com/playlist") || lower.contains("youtube.com/watch") && lower.contains("list="))
        return true;
    if (lower.contains("youtu.be/") && lower.contains("list="))
        return true;
    if (lower.contains("soundcloud.com/") && lower.contains("/sets/"))
        return true;
    if (lower.contains("spotify.com/playlist") || lower.contains("open.spotify.com/playlist"))
        return true;
    if (lower.contains("music.apple.com/") && lower.contains("/playlist"))
        return true;

    return false;
}

bool UrlDetector::isTorrentUrl(const QString& url) {
    QString lower = url.toLower().trimmed();
    if (lower.startsWith("magnet:?")) return true;
    if (lower.endsWith(".torrent")) return true;

    QUrl qurl(url);
    if (qurl.isValid()) {
        QString path = qurl.path().toLower();
        if (path.endsWith(".torrent")) return true;
    }

    return false;
}

bool UrlDetector::isYtDlpUrl(const QString& url) {
    QString lower = url.toLower();

    QStringList ytDlpSites = {
        "youtube.com", "youtu.be", "youtube-nocookie.com",
        "vimeo.com", "dailymotion.com", "twitch.tv",
        "twitter.com", "x.com", "tiktok.com",
        "instagram.com", "facebook.com", "reddit.com",
        "soundcloud.com", "bandcamp.com",
        "nicovideo.jp", "bilibili.com", "bilibili.tv",
        "archive.org", "tumblr.com",
        "twitch.tv", "crunchyroll.com",
        "dailymotion.com", "veoh.com",
        "metacafe.com", "dailymotion.com",
        "rumble.com", "odysee.com"
    };

    for (const QString& site : ytDlpSites) {
        if (lower.contains(site)) return true;
    }

    return false;
}

QString UrlDetector::detectContentCategory(const QString& url) {
    QString lower = url.toLower().trimmed();

    if (isTorrentUrl(url)) return "torrent";
    if (lower.contains("youtube.com") || lower.contains("youtu.be") || lower.contains("vimeo.com") || lower.contains("soundcloud.com") || lower.contains("facebook.com") || lower.contains("twitter.com") || lower.contains("x.com") || lower.contains("tiktok.com") || lower.contains("instagram.com") || lower.contains("reddit.com") || lower.contains("rumble.com") || lower.contains("odysee.com") || lower.contains("bitchute.com")) {
        return "video";
    }

    QUrl qurl(url);
    QString path = qurl.isValid() ? qurl.path() : url;
    QString ext = QFileInfo(path).suffix().toLower();

    static const QStringList imageExts = {"jpg", "jpeg", "png", "gif", "webp", "bmp", "svg", "ico"};
    static const QStringList videoExts = {"mp4", "mkv", "avi", "mov", "wmv", "flv", "webm", "m4v", "mpeg", "mpg", "ts", "m2ts"};
    static const QStringList audioExts = {"mp3", "wav", "flac", "aac", "ogg", "m4a", "opus", "wma"};
    static const QStringList archiveExts = {"zip", "rar", "7z", "tar", "gz", "bz2", "xz", "tgz"};
    static const QStringList documentExts = {"pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "txt", "rtf", "csv"};
    static const QStringList executableExts = {"exe", "msi", "dmg", "apk", "deb", "rpm", "appimage"};

    if (imageExts.contains(ext)) return "image";
    if (videoExts.contains(ext)) return "video";
    if (audioExts.contains(ext)) return "audio";
    if (archiveExts.contains(ext)) return "archive";
    if (documentExts.contains(ext)) return "document";
    if (executableExts.contains(ext)) return "executable";

    if (lower.contains(".jpg") || lower.contains(".jpeg") || lower.contains(".png") || lower.contains(".gif") || lower.contains(".webp") || lower.contains(".avif")) return "image";
    if (lower.contains(".mp4") || lower.contains(".mkv") || lower.contains(".mp3") || lower.contains(".wav") || lower.contains(".flac")) return "video";

    return "other";
}

bool UrlDetector::isAllowedByDownloadType(const QString& url, const QHash<QString, QString>& modes) {
    if (modes.isEmpty()) {
        return true;
    }

    QString category = detectContentCategory(url);
    QString mode = modes.value(category, "disabled");
    if (mode == "allow") return true;
    if (mode == "block") return false;
    return true;
}

QString UrlDetector::typeToString(UrlType type) {
    switch (type) {
        case UrlTorrent: return "Torrent/Magnet";
        case UrlPlaylist: return "Playlist";
        case UrlYtDlp: return "Video (yt-dlp)";
        case UrlDirect: return "Direct Download";
        default: return "Unknown";
    }
}
