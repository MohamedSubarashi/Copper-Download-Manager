#include "utils/FfmpegManager.h"
#include "utils/Logger.h"
#include "utils/UserAgent.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QOperatingSystemVersion>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QDirIterator>
#include <QFileInfo>
#include "utils/DependencyVerifier.h"

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

    if (isInstalled()) {
        Logger::instance().info("ffmpeg already installed: " + getVersion() + " (skipping download)");
        emit installationProgress("Already installed: " + getVersion());
        return;
    }

    isDownloading = true;
    Logger::instance().info("Installing/updating ffmpeg...");
    emit installationProgress("Starting download...");

#ifdef PLATFORM_WINDOWS
    // Versioned release package (not a rolling "latest" build) so the SHA-256
    // pin in resources/dependencies.json stays valid between releases.
    QString url = "https://www.gyan.dev/ffmpeg/builds/packages/ffmpeg-9.0.2-essentials_build.zip";
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
    request.setRawHeader("User-Agent", Copper::copperUserAgent().toUtf8());

    activeReply = nam->get(request);

    connect(activeReply, &QNetworkReply::downloadProgress, [this](qint64 received, qint64 total) {
        if (total > 0) {
            int pct = (int)((received * 100) / total);
            emit installationProgress("Downloading: " + QString::number(pct) + "%");
        }
    });

    connect(activeReply, &QNetworkReply::finished, [this, url, fileName]() {
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

        // Abort before extraction if the archive does not match the pinned
        // checksum (the pin is tied to the exact download URL).
        QString verifyError;
        if (!DependencyVerifier::verifyPinned(QStringLiteral("ffmpeg"), url, data, &verifyError)) {
            Logger::instance().error("ffmpeg install blocked: " + verifyError);
            emit installationProgress("Verification failed - install blocked");
            emit errorOccurred(verifyError);
            isDownloading = false;
            return;
        }

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

        // Pull both tools yt-dlp needs out of the archive: ffmpeg to mux the
        // separate video/audio streams and ffprobe to inspect them. They must end
        // up directly in the tools dir, which is what --ffmpeg-location points at.
        for (const QString& exe : {QStringLiteral("ffmpeg.exe"), QStringLiteral("ffprobe.exe")}) {
            QDirIterator it(toolsDir, {exe}, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                QString src = it.next();
                QString dest = toolsDir + "/" + exe;
                if (QFileInfo(src) == QFileInfo(dest)) continue;
                QFile::remove(dest);
                QFile::copy(src, dest);
            }
        }
        QFile::remove(zipPath);
        // The archive extracts into a directory named after the zip; remove it
        // once the binaries have been copied out.
        QDir(toolsDir + "/" + QFileInfo(fileName).baseName()).removeRecursively();
#else
        // Linux gets a .tar.xz and macOS a .zip, neither of which Qt can extract,
        // so shell out to the OS tools. Extract to a scratch dir, copy the two
        // binaries into the tools dir, then clean up.
        QString extractDir = toolsDir + "/ffmpeg_extract";
        QDir(extractDir).removeRecursively();
        QDir().mkpath(extractDir);
        {
            QProcess extract;
            if (fileName.endsWith(".zip")) {
                extract.start("unzip", QStringList() << "-o" << zipPath << "-d" << extractDir);
            } else {
                extract.start("tar", QStringList() << "-xf" << zipPath << "-C" << extractDir);
            }
            extract.waitForFinished(300000);
        }
        for (const QString& exe : {QStringLiteral("ffmpeg"), QStringLiteral("ffprobe")}) {
            QDirIterator it(extractDir, {exe}, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                QString src = it.next();
                QString dest = toolsDir + "/" + exe;
                QFile::remove(dest);
                if (QFile::copy(src, dest)) {
                    QFile::setPermissions(dest, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                   QFileDevice::ExeOwner | QFileDevice::ReadUser |
                                                   QFileDevice::ExeUser | QFileDevice::ReadGroup |
                                                   QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                                   QFileDevice::ExeOther);
                }
            }
        }
        QDir(extractDir).removeRecursively();
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
