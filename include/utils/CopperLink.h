#ifndef COPPERLINK_H
#define COPPERLINK_H

#include <QString>
#include <QUrl>
#include <QUrlQuery>

struct CopperLink {
    QString url;
    QString filename;
    QString path;
    bool valid = false;
};

inline QString decodeOnce(const QString& value) {
    // The extension percent-encodes every query value with encodeURIComponent.
    // QUrlQuery may report raw (undecoded) values, so decode exactly once here.
    // A plain value like "https://..." decodes to itself; true %HH in a real URL
    // arrive as %25HH so a single decode yields the correct %HH.
    if (value.contains('%')) {
        return QUrl::fromPercentEncoding(value.toUtf8());
    }
    return value;
}

inline CopperLink parseCopperLink(const QString& raw) {
    CopperLink cl;
    if (!raw.startsWith("copper:", Qt::CaseInsensitive)) return cl;
    QUrl u(raw);
    if (u.host().compare("download", Qt::CaseInsensitive) != 0) return cl;
    QUrlQuery query(u);
    cl.url = decodeOnce(query.queryItemValue("url"));
    cl.filename = decodeOnce(query.queryItemValue("filename"));
    cl.path = decodeOnce(query.queryItemValue("path"));
    cl.valid = !cl.url.isEmpty();
    return cl;
}

#endif
