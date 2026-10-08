#ifndef DEPENDENCYVERIFIER_H
#define DEPENDENCYVERIFIER_H

#include <QByteArray>
#include <QString>

// Verifies the third-party binaries Copper downloads at runtime before they
// are written to disk or executed (0.4.0 hardening). Everything fails closed:
// a missing manifest entry, a URL that no longer matches the pin, or a hash
// mismatch aborts the install with a logged reason instead of installing a
// corrupted or tampered binary.
class DependencyVerifier {
public:
    // Lowercase hex SHA-256 of data.
    static QString sha256Hex(const QByteArray& data);

    // True if the manifest pins this key for the current platform at all.
    static bool hasPin(const QString& key);

    // Verify data downloaded from url against the pinned hash for key
    // ("aria2" or "ffmpeg"). The URL is part of the check: bumping a download
    // URL without updating resources/dependencies.json fails rather than
    // silently skipping verification.
    static bool verifyPinned(const QString& key, const QString& url,
                             const QByteArray& data, QString* error);

    // Parse a published "<hex>  <filename>" checksum file (yt-dlp ships a
    // SHA2-256SUMS asset with every release) and return the hash recorded for
    // fileName, or an empty string if absent or malformed.
    static QString publishedHashFor(const QByteArray& sumsText, const QString& fileName);

    // Verify data against fileName's line in a published checksum file.
    static bool verifyAgainstSumsText(const QByteArray& sumsText, const QString& fileName,
                                      const QByteArray& data, QString* error);
};

#endif
