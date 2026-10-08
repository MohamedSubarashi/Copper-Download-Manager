#ifndef USERAGENT_H
#define USERAGENT_H

#include <QString>
#include <QCoreApplication>

// Centralized User-Agent for every outbound HTTP request the app makes
// (chunked downloads, update checks, GitHub API calls, dependency binary
// downloads) and for the default UA DatabaseManager uses when the user has not
// overridden the setting.
//
// The version component derives from QCoreApplication::applicationVersion(),
// which main.cpp sets from COPPER_VERSION_STRING (the single-source version).
// Nothing in the codebase should hardcode a "CopperDownloadManager/<version>"
// UA suffix anywhere else; this header is the one designated place for the
// prefix itself.
namespace Copper {

// Browser-like UA (servers often reject or mis-handle bare download-manager
// UAs). This is the DEFAULT for ordinary HTTP(S) download traffic.
inline QString copperUserAgent() {
    return QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                          "AppleWebKit/537.36 (KHTML, like Gecko) "
                          "Chrome/120.0.0.0 Safari/537.36 CopperDownloadManager/")
        + QCoreApplication::applicationVersion();
}

// Minimal UA for API/binary downloads (GitHub, dependency hosting).
inline QString bareUserAgent() {
    return QStringLiteral("CopperDownloadManager/")
        + QCoreApplication::applicationVersion();
}

} // namespace Copper

#endif // USERAGENT_H