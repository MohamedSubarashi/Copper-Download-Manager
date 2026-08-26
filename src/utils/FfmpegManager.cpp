#include "utils/FfmpegManager.h"
#include "utils/Logger.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QOperatingSystemVersion>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QDirIterator>

FfmpegManager::FfmpegManager() : isDownloading(false), converting(false), nam(new QNetworkAccessManager(this)), activeReply(nullptr), convertProcess(nullptr) {}

FfmpegManager& FfmpegManager::instance() {
    static FfmpegManager instance;
    return instance;
}

QString FfmpegManager::getToolsDir() {
#ifdef PLATFORM_WINDOWS
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/tools";
#else
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.copper/tools";
#endif
}

QString FfmpegManager::getFfmpegPath() {
    QString dir = getToolsDir();
    QDir().mkpath(dir);
#ifdef PLATFORM_WINDOWS
    return dir + "/ffmpeg.exe";
#else
    return dir + "/ffmpeg";
#endif
}

bool FfmpegManager::isInstalled() {
    return QFile::exists(getFfmpegPath());
}

QString FfmpegManager::getVersion() {
    if (!isInstalled()) return "Not installed";
    QProcess process;
    process.start(getFfmpegPath(), QStringList() << "-version");
    process.waitForFinished(5000);
    QString output = process.readAllStandardOutput().trimmed();
    QRegularExpression re("ffmpeg version (\\S+)");
    QRegularExpressionMatch match = re.match(output);
    return match.hasMatch() ? match.captured(1) : output.split('\n').first();
}

QString FfmpegManager::getDownloadPath() {
    return getToolsDir();
}

void FfmpegManager::installOrUpdate() {
    if (isDownloading) {
        Logger::instance().info("ffmpeg installation already in progress");
        return;
    }

    isDownloading = true;
    Logger::instance().info("Installing/updating ffmpeg...");
    emit installationProgress("Starting download...");

#ifdef PLATFORM_WINDOWS
    QString url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip";
    QString fileName = "ffmpeg-master-latest-win64-gpl.zip";
#elif defined(PLATFORM_LINUX)
    QString url = "https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz";
    QString fileName = "ffmpeg-release-amd64-static.tar.xz";
#else
    QString url = "https://evermeet.cx/ffmpeg/getrelease/zip";
    QString fileName = "ffmpeg.zip";
#endif

    emit installationProgress("Downloading ffmpeg...");
    startBinaryDownload(url, fileName);
}

void FfmpegManager::startBinaryDownload(const QString& url, const QString& fileName) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "Mozilla/5.0 CopperDownloadManager/1.0");

    activeReply = nam->get(request);

    connect(activeReply, &QNetworkReply::downloadProgress, [this](qint64 received, qint64 total) {
        if (total > 0) {
            int pct = (int)((received * 100) / total);
            emit installationProgress("Downloading: " + QString::number(pct) + "%");
        }
    });

    connect(activeReply, &QNetworkReply::finished, [this, fileName]() {
        QNetworkReply* reply = activeReply;
        activeReply = nullptr;

        if (!reply) return;

        if (reply->error() != QNetworkReply::NoError) {
            Logger::instance().error("ffmpeg download failed: " + reply->errorString());
            emit installationProgress("Download failed: " + reply->errorString());
            emit errorOccurred(reply->errorString());
            isDownloading = false;
            reply->deleteLater();
            return;
        }

        int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpStatus >= 300 && httpStatus < 400) {
            QUrl redirectUrl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
            reply->deleteLater();
            startBinaryDownload(redirectUrl.toString(), fileName);
            return;
        }

        QByteArray data = reply->readAll();
        reply->deleteLater();

        emit installationProgress("Extracting...");

        QString toolsDir = getToolsDir();
        QDir().mkpath(toolsDir);
        QString zipPath = toolsDir + "/" + fileName;

        QFile zipFile(zipPath);
        if (!zipFile.open(QIODevice::WriteOnly)) {
            emit installationProgress("Write error");
            isDownloading = false;
            return;
        }
        zipFile.write(data);
        zipFile.close();

#ifdef PLATFORM_WINDOWS
        QString psCmd = "Expand-Archive -Path '" + zipPath + "' -DestinationPath '" + toolsDir + "' -Force";
        QProcess process;
        process.start("powershell", QStringList() << "-NoProfile" << "-Command" << psCmd);
        process.waitForFinished(120000);

        QDirIterator it(toolsDir, {"ffmpeg.exe"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString src = it.next();
            QString dest = toolsDir + "/ffmpeg.exe";
            QFile::remove(dest);
            QFile::copy(src, dest);
        }
        QFile::remove(zipPath);
        QDir(toolsDir + "/ffmpeg-master-latest-win64-gpl").removeRecursively();
#else
        QFile::remove(zipPath);
#endif

        if (isInstalled()) {
            emit installationProgress("Installed: " + getVersion());
            Logger::instance().info("ffmpeg installed successfully");
        } else {
            emit installationProgress("Installation failed - please install manually");
        }

        isDownloading = false;
    });
}

void FfmpegManager::convert(const QString& input, const QString& output, const QString& format) {
    if (!isInstalled()) {
        emit errorOccurred("ffmpeg not installed");
        return;
    }

    converting = true;
    convertProcess = new QProcess(this);

    QStringList args;
    args << "-i" << input << "-c:v" << format << output;

    connect(convertProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        QString output = QString::fromUtf8(convertProcess->readAllStandardOutput());
        emit conversionProgress(output.trimmed());
    });

    connect(convertProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this](int exitCode, QProcess::ExitStatus) {
        converting = false;
        if (exitCode == 0) {
            emit conversionFinished();
        } else {
            emit errorOccurred("Conversion failed with code " + QString::number(exitCode));
        }
        convertProcess->deleteLater();
        convertProcess = nullptr;
    });

    convertProcess->start(getFfmpegPath(), args);
}

bool FfmpegManager::isConverting() const {
    return converting;
}
