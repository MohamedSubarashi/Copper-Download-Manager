#ifndef FILENAMESANITIZER_H
#define FILENAMESANITIZER_H

#include <QString>
#include <QChar>

// Removes path separators, forbidden Windows characters, control characters,
// path-traversal segments (..), and trailing dots/spaces from a user- or
// server-supplied file name. A single slash/backslash-safe name is returned.
inline QString sanitizeFileName(const QString& raw, const QString& fallback = QStringLiteral("download")) {
    QString name = raw;
    if (name.isEmpty()) return fallback;

    // Reject path traversal outright: split on separators and keep only the
    // last component, then drop any remaining ".." segment names.
    name = name.section('/', -1).section('\\', -1);
    if (name == ".." || name == ".") name.clear();

    // Strip Windows-forbidden characters and C0 control characters.
    QString cleaned;
    cleaned.reserve(name.size());
    for (const QChar& ch : name) {
        ushort u = ch.unicode();
        if (u < 0x20) continue;                           // control chars
        if (u == '<' || u == '>' || u == ':' || u == '"' ||
            u == '|' || u == '?' || u == '*' ||
            u == '/' || u == '\\') continue;              // forbidden on Windows
        if (u == 0x7F) continue;                          // DEL
        cleaned.append(ch);
    }

    // Remove trailing periods and spaces (Windows trims them at the shell level).
    int end = cleaned.size();
    while (end > 0 && (cleaned[end - 1] == '.' || cleaned[end - 1].isSpace())) end--;
    cleaned.truncate(end);

    // Guard against empty and over-long results.
    if (cleaned.isEmpty() || cleaned == "." || cleaned == "..") return fallback;
    if (cleaned.size() > 200) cleaned.truncate(200);

    return cleaned;
}

#endif // FILENAMESANITIZER_H