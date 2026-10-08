#include "utils/DependencyVerifier.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {

// The pinned manifest is compiled into the binary (resources.qrc) so a
// tampered install directory cannot swap the pins along with the binaries.
QJsonObject manifestObject() {
    static QJsonObject manifest = [] {
        QFile f(QStringLiteral(":/dependencies.json"));
        if (!f.open(QIODevice::ReadOnly)) return QJsonObject();
        return QJsonDocument::fromJson(f.readAll()).object();
    }();
    return manifest;
}

QString platformKey() {
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

// Entry for key on this platform: either a flat entry (single-platform tools)
// or a platforms{...} map keyed by platform. Empty object when absent.
QJsonObject entryFor(const QString& key) {
    const QJsonObject top = manifestObject();
    if (!top.contains(key)) return QJsonObject();
    const QJsonObject entry = top.value(key).toObject();
    if (entry.contains("platforms"))
        return entry.value("platforms").toObject().value(platformKey()).toObject();
    return entry;
}

} // namespace

QString DependencyVerifier::sha256Hex(const QByteArray& data) {
    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool DependencyVerifier::hasPin(const QString& key) {
    return !entryFor(key).value("sha256").toString().isEmpty();
}

bool DependencyVerifier::verifyPinned(const QString& key, const QString& url,
                                      const QByteArray& data, QString* error) {
    const QJsonObject entry = entryFor(key);
    const QString pin = entry.value("sha256").toString().toLower();
    const QString pinnedUrl = entry.value("url").toString();
    if (pin.isEmpty()) {
        if (error) {
            *error = QStringLiteral("no %1 pin for %2 in the manifest; refusing to install an unverified binary")
                         .arg(key, platformKey());
        }
        return false;
    }
    if (url.compare(pinnedUrl, Qt::CaseInsensitive) != 0) {
        if (error) {
            *error = QStringLiteral("%1 download URL does not match the one pinned in the manifest "
                                    "(code: %2, manifest: %3); update resources/dependencies.json "
                                    "together with the URL")
                         .arg(key, url, pinnedUrl);
        }
        return false;
    }
    const QString actual = sha256Hex(data);
    if (actual != pin) {
        if (error) {
            *error = QStringLiteral("%1 download failed verification: expected sha256 %2, got %3")
                         .arg(key, pin, actual);
        }
        return false;
    }
    return true;
}

QString DependencyVerifier::publishedHashFor(const QByteArray& sumsText, const QString& fileName) {
    const QList<QByteArray> lines = sumsText.split('\n');
    for (const QByteArray& raw : lines) {
        const QString line = QString::fromUtf8(raw).trimmed();
        // "<64 hex>  <filename>" per the coreutils convention yt-dlp follows;
        // be lenient about the exact separator.
        const int sep = line.indexOf(QLatin1Char(' '));
        if (sep <= 0) continue;
        const QString hash = line.left(sep).toLower();
        const QString name = line.mid(sep).trimmed();
        if (!QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(hash).hasMatch()) continue;
        if (name == fileName) return hash;
    }
    return QString();
}

bool DependencyVerifier::verifyAgainstSumsText(const QByteArray& sumsText, const QString& fileName,
                                               const QByteArray& data, QString* error) {
    const QString expected = publishedHashFor(sumsText, fileName);
    if (expected.isEmpty()) {
        if (error) {
            *error = QStringLiteral("published checksums do not list %1; refusing to install an unverified binary")
                         .arg(fileName);
        }
        return false;
    }
    const QString actual = sha256Hex(data);
    if (actual != expected) {
        if (error) {
            *error = QStringLiteral("%1 failed verification against the published checksums: "
                                    "expected %2, got %3")
                         .arg(fileName, expected, actual);
        }
        return false;
    }
    return true;
}
