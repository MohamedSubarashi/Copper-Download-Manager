#include "utils/ApiToken.h"

#include "utils/Logger.h"
#include "utils/NativeMessaging.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>

namespace {

// In-process cache. Every caller runs on the main thread (LocalServer, the
// second-instance probes in main(), startup config writes), so no locking is
// needed.
QString& cachedToken() {
    static QString token;
    return token;
}

bool isWellFormed(const QString& token) {
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
    return re.match(token).hasMatch();
}

// Compare without early exit so the time taken does not reveal how many
// leading characters of the presented token matched.
bool constantTimeEqual(const QString& a, const QString& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a.at(i).unicode() ^ b.at(i).unicode());
    return diff == 0;
}

}  // namespace

bool ApiToken::loadFromDisk() {
    QFile f(NativeMessaging::hostConfigPath());
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonObject cfg = QJsonDocument::fromJson(f.readAll()).object();
    const QString value = cfg.value(QStringLiteral("apiToken")).toString();
    if (!isWellFormed(value)) return false;
    cachedToken() = value;
    return true;
}

bool ApiToken::persist(const QString& value) {
    // Merge instead of truncate: copper_host_config.json also carries
    // copperExecutable (and the reverse write must not drop the token), so
    // each writer preserves the other's keys. QSaveFile replaces the file
    // atomically - the native host may be reading it concurrently and must
    // never observe a half-written document.
    const QString path = NativeMessaging::hostConfigPath();

    QJsonObject cfg;
    {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject existing = QJsonDocument::fromJson(f.readAll()).object();
            if (!existing.isEmpty()) cfg = existing;
        }
    }
    cfg[QStringLiteral("apiToken")] = value;

    QDir().mkpath(QFileInfo(path).absolutePath());

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        Logger::instance().error("ApiToken: cannot open " + path + ": " + out.errorString());
        return false;
    }
    if (out.write(QJsonDocument(cfg).toJson(QJsonDocument::Indented)) < 0) {
        Logger::instance().error("ApiToken: write failed for " + path + ": " + out.errorString());
        out.cancelWriting();
        return false;
    }
    if (!out.commit()) {
        Logger::instance().error("ApiToken: cannot commit " + path + ": " + out.errorString());
        return false;
    }
    return true;
}

QString ApiToken::token() {
    QString& cached = cachedToken();
    if (!cached.isEmpty()) return cached;
    if (loadFromDisk()) return cached;

    // First run (or unreadable config): mint a 64-hex token and persist it so
    // every client - second app instance, native host, browser extension -
    // can read it from the one shared file.
    quint32 words[8];
    for (quint32& w : words) w = QRandomGenerator::system()->generate();
    cached = QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(words), sizeof(words)).toHex());

    if (!persist(cached)) {
        // Serve it from memory anyway: the server and this process's own
        // clients stay in sync, while other clients fail closed (401) - the
        // safe direction for an unwritable profile.
        Logger::instance().warning(
            "ApiToken: token not persisted; other clients will be rejected until the config dir is writable");
    }
    return cached;
}

bool ApiToken::matches(const QString& provided) {
    if (provided.isEmpty()) return false;
    if (constantTimeEqual(provided, token())) return true;
    // The cache can go stale if another process rewrote the config; re-read
    // once before rejecting so a valid token on disk is never ignored.
    if (loadFromDisk() && constantTimeEqual(provided, cachedToken())) return true;
    return false;
}
