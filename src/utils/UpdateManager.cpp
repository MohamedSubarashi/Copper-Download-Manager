#include "utils/UpdateManager.h"
#include "utils/Logger.h"
#include "utils/UserAgent.h"
#include "db/DatabaseManager.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QMessageBox>
#include <QTimer>
#include <QCryptographicHash>
#include <QSaveFile>

UpdateManager::UpdateManager()
    : nam(new QNetworkAccessManager(this)),
      updateAvailableFlag(false),
      silentCheck(false),
      downloading(false) {}

UpdateManager& UpdateManager::instance() {
    static UpdateManager instance;
    return instance;
}

int UpdateManager::compareVersions(const QString& a, const QString& b) const {
    QStringList pa = a.split('.');
    QStringList pb = b.split('.');

    int n = qMax(pa.size(), pb.size());
    for (int i = 0; i < n; i++) {
        int x = i < pa.size() ? pa[i].toInt() : 0;
        int y = i < pb.size() ? pb[i].toInt() : 0;
        if (x < y) return -1;
        if (x > y) return 1;
    }
    return 0;
}

void UpdateManager::checkForUpdates(bool silent) {
    silentCheck = silent;
    Logger::instance().info("Checking for updates (silent=" + QString::number(silent) + ")...");

    QNetworkRequest request(QUrl("https://api.github.com/repos/MohamedSubarashi/Copper-Download-Manager/releases/latest"));
    request.setRawHeader("Accept", "application/vnd.github.v3+json");
    request.setRawHeader("User-Agent", Copper::bareUserAgent().toUtf8());

    QNetworkReply* reply = nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onVersionCheckFinished(reply);
    });
}

void UpdateManager::onVersionCheckFinished(QNetworkReply* reply) {
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        Logger::instance().error("Update check failed: " + reply->errorString());
        if (!silentCheck) {
            emit errorOccurred("Failed to check for updates: " + reply->errorString());
        }
        emit checkFinished();
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonObject release = doc.object();

    latestVersion = release["tag_name"].toString().trimmed();
    changelog = release["body"].toString().trimmed();
    if (latestVersion.startsWith('v')) {
        latestVersion = latestVersion.mid(1);
    }

    downloadUrl.clear();
    downloadDigest.clear();
    QJsonArray assets = release["assets"].toArray();
    for (const QJsonValue& asset : assets) {
        QJsonObject a = asset.toObject();
        QString name = a["name"].toString();
#ifdef PLATFORM_WINDOWS
        if (name.endsWith(".exe", Qt::CaseInsensitive)) {
            downloadUrl = a["browser_download_url"].toString();
            downloadDigest = a["digest"].toString();   // "sha256:<hex>" or empty
            break;
        }
#else
        if (name.endsWith(".AppImage", Qt::CaseInsensitive) ||
            name.endsWith(".dmg", Qt::CaseInsensitive) ||
            name.endsWith(".tar.gz", Qt::CaseInsensitive)) {
            downloadUrl = a["browser_download_url"].toString();
            downloadDigest = a["digest"].toString();
            break;
        }
#endif
    }

    if (downloadUrl.isEmpty()) {
        Logger::instance().error("No downloadable asset found in latest release");
        if (!silentCheck) {
            emit errorOccurred("No downloadable file found in the latest release.");
        }
        emit checkFinished();
        return;
    }

    QString currentVersion = QCoreApplication::applicationVersion();
    int cmp = compareVersions(currentVersion, latestVersion);

    Logger::instance().info("Current version: " + currentVersion + ", Latest: " + latestVersion + ", cmp=" + QString::number(cmp));

    if (cmp < 0) {
        updateAvailableFlag = true;
        emit updateAvailable(latestVersion);
    } else {
        updateAvailableFlag = false;
        if (!silentCheck) {
            emit noUpdateAvailable();
        }
    }

    emit checkFinished();
}

void UpdateManager::downloadAndInstall() {
    if (downloading) return;
    if (!updateAvailableFlag || downloadUrl.isEmpty()) return;

    downloading = true;
    Logger::instance().info("Downloading update from: " + downloadUrl);

    QNetworkRequest request(downloadUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", Copper::bareUserAgent().toUtf8());

    QNetworkReply* reply = nam->get(request);
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (total > 0) {
            int pct = (int)((received * 100) / total);
            emit downloadProgress(pct);
        }
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onDownloadFinished(reply);
    });
}

void UpdateManager::onDownloadFinished(QNetworkReply* reply) {
    reply->deleteLater();
    downloading = false;

    if (reply->error() != QNetworkReply::NoError) {
        Logger::instance().error("Update download failed: " + reply->errorString());
        emit downloadFailed("Failed to download update: " + reply->errorString());
        return;
    }

    QByteArray data = reply->readAll();

    // An update installer is executed; refuse to stage anything that cannot be
    // verified against the digest GitHub recorded for this exact asset.
    if (!downloadDigest.startsWith("sha256:", Qt::CaseInsensitive)) {
        Logger::instance().error("Update asset has no sha256 digest; refusing to install an unverified update");
        emit downloadFailed("The release asset has no published checksum; update blocked.");
        return;
    }
    const QString expected = downloadDigest.mid(7).toLower();
    const QString actual = QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    if (actual != expected) {
        Logger::instance().error("Update checksum mismatch: expected " + expected + ", got " + actual);
        emit downloadFailed("The downloaded update failed checksum verification; update blocked.");
        return;
    }

    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    installerPath = dir + "/CopperUpdate_" + latestVersion + ".exe";

    // QSaveFile stages into a temporary file and atomically renames on
    // commit(), so a crash mid-write can never leave a half-written installer
    // under the final name.
    QSaveFile file(installerPath);
    if (!file.open(QIODevice::WriteOnly)) {
        emit downloadFailed("Failed to write update file");
        return;
    }
    if (file.write(data) != data.size() || !file.commit()) {
        file.cancelWriting();
        emit downloadFailed("Failed to write update file");
        return;
    }

    Logger::instance().info("Update downloaded and verified: " + installerPath);
    emit downloadFinished();
}

QString UpdateManager::installerPathOrEmpty() {
    return installerPath;
}

bool UpdateManager::isUpdateAvailable() const {
    return updateAvailableFlag;
}

QString UpdateManager::getLatestVersion() const {
    return latestVersion;
}

QString UpdateManager::getDownloadUrl() const {
    return downloadUrl;
}

QString UpdateManager::getChangelog() const {
    return changelog;
}

bool UpdateManager::isDownloading() const {
    return downloading;
}
