#ifndef UPDATEMANAGER_H
#define UPDATEMANAGER_H

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

class UpdateManager : public QObject {
    Q_OBJECT
public:
    static UpdateManager& instance();

    void checkForUpdates(bool silent = false);
    bool isUpdateAvailable() const;
    QString getLatestVersion() const;
    QString getDownloadUrl() const;
    QString getChangelog() const;
    void downloadAndInstall();
    QString installerPathOrEmpty();
    bool isDownloading() const;

signals:
    void checkFinished();
    void updateAvailable(const QString& version);
    void noUpdateAvailable();
    void downloadProgress(int percent);
    void downloadFinished();
    void downloadFailed(const QString& error);
    void errorOccurred(const QString& error);

private slots:
    void onVersionCheckFinished(QNetworkReply* reply);
    void onDownloadFinished(QNetworkReply* reply);

private:
    UpdateManager();
    int compareVersions(const QString& a, const QString& b) const;

    QNetworkAccessManager* nam;
    bool updateAvailableFlag;
    bool silentCheck;
    QString latestVersion;
    QString downloadUrl;
    QString changelog;
    QString installerPath;
    bool downloading;
};

#endif
