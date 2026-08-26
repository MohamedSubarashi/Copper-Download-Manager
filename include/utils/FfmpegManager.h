#ifndef FFMPEGMANAGER_H
#define FFMPEGMANAGER_H

#include <QObject>
#include <QString>
#include <QProcess>

class QNetworkAccessManager;
class QNetworkReply;

class FfmpegManager : public QObject {
    Q_OBJECT
public:
    static FfmpegManager& instance();
    bool isInstalled();
    QString getVersion();
    void installOrUpdate();
    QString getFfmpegPath();
    QString getDownloadPath();
    void convert(const QString& input, const QString& output, const QString& format);
    bool isConverting() const;

signals:
    void installationProgress(const QString& status);
    void conversionProgress(const QString& progress);
    void conversionFinished();
    void errorOccurred(const QString& error);

private:
    FfmpegManager();
    QString getToolsDir();
    void startBinaryDownload(const QString& url, const QString& fileName);

    bool isDownloading;
    bool converting;
    QNetworkAccessManager* nam;
    QNetworkReply* activeReply;
    QProcess* convertProcess;
};

#endif
