#include "utils/YtDlpManager.h"
#include "utils/Logger.h"
#include "utils/FfmpegManager.h"
#include "db/DatabaseManager.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QOperatingSystemVersion>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>

YtDlpManager::YtDlpManager() : isDownloading(false), nam(new QNetworkAccessManager(this)), activeReply(nullptr) {}

YtDlpManager& YtDlpManager::instance() {
    static YtDlpManager instance;
    return instance;
}

QString YtDlpManager::getToolsDir() {
#ifdef PLATFORM_WINDOWS
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/tools";
#else
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.copper/tools";
#endif
}

QString YtDlpManager::getYtDlpPath() {
    QString dir = getToolsDir();
    QDir().mkpath(dir);
#ifdef PLATFORM_WINDOWS
    return dir + "/yt-dlp.exe";
#else
    return dir + "/yt-dlp";
#endif
}

bool YtDlpManager::isInstalled() {
    return QFile::exists(getYtDlpPath());
}

QString YtDlpManager::getVersion() {
    if (!isInstalled()) return "Not installed";
    QProcess process;
    process.start(getYtDlpPath(), QStringList() << "--version");
    process.waitForFinished(5000);
    return process.readAllStandardOutput().trimmed();
}

void YtDlpManager::installOrUpdate() {
    if (isDownloading) {
        Logger::instance().info("yt-dlp installation already in progress");
        return;
    }

    isDownloading = true;
    Logger::instance().info("Installing/updating yt-dlp...");
    emit installationProgress("Checking latest version...");

    QNetworkRequest request(QUrl("https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest"));
    request.setRawHeader("Accept", "application/vnd.github.v3+json");
    request.setRawHeader("User-Agent", "CopperDownloadManager/1.0");

    QNetworkReply* reply = nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            Logger::instance().error("yt-dlp version check failed: " + reply->errorString());
            emit installationProgress("Version check failed: " + reply->errorString());
            emit errorOccurred(reply->errorString());
            isDownloading = false;
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonObject release = doc.object();
        QString tagName = release["tag_name"].toString();
        QJsonArray assets = release["assets"].toArray();

        // If yt-dlp is already installed, only re-download when a newer release exists.
        if (isInstalled()) {
            QString installed = getVersion().trimmed();
            if (installed == tagName || (installed.compare(tagName) >= 0)) {
                Logger::instance().info("yt-dlp is up to date: " + installed + " == latest " + tagName + " (skipping download)");
                emit installationProgress("Already up to date: " + installed);
                isDownloading = false;
                return;
            }
            Logger::instance().info("yt-dlp update available: installed=" + installed + ", latest=" + tagName);
        }

        QString assetUrl;
        QString fileName;
#ifdef PLATFORM_WINDOWS
        QString assetName = "yt-dlp.exe";
#else
        QString assetName = "yt-dlp";
#endif

        for (const QJsonValue& asset : assets) {
            QJsonObject a = asset.toObject();
            if (a["name"].toString() == assetName) {
                assetUrl = a["browser_download_url"].toString();
                fileName = a["name"].toString();
                break;
            }
        }

        if (assetUrl.isEmpty()) {
            Logger::instance().error("yt-dlp asset not found in release " + tagName);
            emit installationProgress("Asset not found in release");
            isDownloading = false;
            return;
        }

        Logger::instance().info("yt-dlp latest version: " + tagName + ", downloading from: " + assetUrl);
        emit installationProgress("Downloading yt-dlp " + tagName + "...");
        startBinaryDownload(assetUrl, fileName);
    });
}

void YtDlpManager::startBinaryDownload(const QString& url, const QString& fileName) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "Mozilla/5.0 CopperDownloadManager/1.0");
    request.setRawHeader("Accept", "application/octet-stream");

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
            Logger::instance().error("yt-dlp download failed: " + reply->errorString());
            emit installationProgress("Download failed: " + reply->errorString());
            emit errorOccurred(reply->errorString());
            isDownloading = false;
            reply->deleteLater();
            return;
        }

        QByteArray data = reply->readAll();
        reply->deleteLater();

        QString toolsDir = getToolsDir();
        QDir().mkpath(toolsDir);
        QString filePath = toolsDir + "/" + fileName;

        QFile file(filePath);
        if (!file.open(QIODevice::WriteOnly)) {
            emit installationProgress("Write error");
            isDownloading = false;
            return;
        }
        file.write(data);
        file.close();

#ifdef PLATFORM_UNIX
        QFile::setPermissions(filePath, QFileDevice::ExeUser | QFileDevice::ExeOwner | QFileDevice::ExeOther | QFileDevice::ReadUser | QFileDevice::ReadOwner);
#endif

        if (isInstalled()) {
            emit installationProgress("Installed: " + getVersion());
            Logger::instance().info("yt-dlp installed successfully");
        } else {
            emit installationProgress("Installation failed");
        }

        isDownloading = false;
    });
}

void YtDlpManager::startDownload(const QString& url, const QString& outputPath, int downloadId, const QString& format) {
    if (!isInstalled()) {
        emit downloadFailed(downloadId, "yt-dlp not installed");
        return;
    }

    QProcess* process = new QProcess(this);
    activeProcesses[downloadId] = process;

    QStringList args;

    // Build a valid yt-dlp output. yt-dlp's -o expects an output template; a bare path
    // without a file extension and without a %(...) placeholder makes yt-dlp write a file
    // with no extension (which looks like the download "never happened"). If the caller
    // gave us a path with no extension and no template, output into its parent directory
    // using a proper template so the file gets a correct, named, extended filename.
    QString outputArg = outputPath;
    if (!outputPath.contains("%(")) {
        QString ext = QFileInfo(outputPath).suffix();
        if (ext.isEmpty()) {
            QString outDir = QFileInfo(outputPath).absolutePath();
            if (outDir.isEmpty()) outDir = outputPath;
            outputArg = outDir + "/%(title)s.%(ext)s";
        }
    }
    args << "-o" << outputArg;
    args << "--newline";
    args << "--no-warnings";
    args << "--progress";
    args << "--user-agent" << DatabaseManager::instance().getUserAgent();

    bool needsFfmpeg = true;
    if (format == "mp3") {
        args << "--extract-audio";
        args << "--audio-format" << "mp3";
    } else if (format == "mkv") {
        args << "--merge-output-format" << "mkv";
        needsFfmpeg = false;
    } else if (format == "best") {
        needsFfmpeg = false;
    } else {
        args << "--merge-output-format" << "mp4";
    }

    if (needsFfmpeg && !FfmpegManager::instance().isInstalled()) {
        emit downloadFailed(downloadId,
            "FFmpeg is required for \"" + format + "\" output but is not installed. "
            "Install it from Settings > Tools, then retry the download.");
        return;
    }

    args << url;

    Logger::instance().info("Starting yt-dlp download: " + url + " (format: " + format + ")");

    // Drain BOTH stdout and stderr. yt-dlp routes its --progress lines to stderr when
    // stdout is not a TTY, but it writes a large volume of other informational/merge
    // output to stdout. If stdout is never read, the OS pipe buffer fills and yt-dlp
    // blocks writing to stdout, which hangs the current download and stalls the freed
    // queue (the "stops after N files" bug). processYtDlpOutput() swallows stdout.
    connect(process, &QProcess::readyReadStandardOutput, this, [this, downloadId]() {
        if (!activeProcesses.contains(downloadId)) return;
        processYtDlpOutput(downloadId);
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, downloadId]() {
        if (!activeProcesses.contains(downloadId)) return;
        processYtDlpOutput(downloadId);
    });

    // Stall watchdog: if a child produces no output for a long stretch it is almost
    // certainly hung (e.g. unable to recover from a transient network wait). Force-fail
    // it so the queued playlist advances instead of stalling forever.
    armStallWatchdog(downloadId);

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, downloadId](int exitCode, QProcess::ExitStatus) {
        if (!activeProcesses.contains(downloadId)) return;

        stopStallWatchdog(downloadId);

        // Drain any remaining buffered stdout/stderr before emitting so trailing
        // progress/output lines are captured (especially useful for error messages).
        processYtDlpOutput(downloadId);
        QProcess* done = activeProcesses.value(downloadId);
        QString tail = QString();
        if (done) {
            tail = QString::fromUtf8(done->readAllStandardOutput()).trimmed();
            QString tailErr = QString::fromUtf8(done->readAllStandardError()).trimmed();
            if (tailErr.isEmpty()) tailErr = tail;
            else if (!tail.isEmpty()) tailErr = tailErr + "\n" + tail;
            tail = tailErr;
        }

        if (exitCode == 0) {
            Logger::instance().info("yt-dlp download finished: " + QString::number(downloadId));
            emit downloadFinished(downloadId);
        } else {
            QString err = tail;
            if (err.isEmpty()) err = QString("yt-dlp exited with code %1").arg(exitCode);
            Logger::instance().error("yt-dlp download failed: " + err);
            emit downloadFailed(downloadId, err);
        }

        if (done) done->deleteLater();
        activeProcesses.remove(downloadId);
    });

    process->start(getYtDlpPath(), args);
}

void YtDlpManager::processYtDlpOutput(int id) {
    if (!activeProcesses.contains(id)) return;
    QProcess* process = activeProcesses[id];

    // Any activity (data arriving on either channel) means the child is alive, so
    // re-arm the stall watchdog.
    armStallWatchdog(id);

    // Parse --progress lines from stderr.
    process->setReadChannel(QProcess::StandardError);
    while (process->canReadLine()) {
        QString line = QString::fromUtf8(process->readLine()).trimmed();

        QRegularExpression progressRegex("\\[download\\]\\s+(\\d+\\.?\\d*)%\\s+of\\s+~?(\\d+\\.?\\d*)(\\w+)");
        QRegularExpressionMatch match = progressRegex.match(line);

        if (match.hasMatch()) {
            double percent = match.captured(1).toDouble();
            double totalSize = match.captured(2).toDouble();
            QString unit = match.captured(3);

            qint64 totalBytes = 0;
            if (unit == "MiB" || unit == "MB") totalBytes = (qint64)(totalSize * 1024 * 1024);
            else if (unit == "KiB" || unit == "KB") totalBytes = (qint64)(totalSize * 1024);
            else if (unit == "GiB" || unit == "GB") totalBytes = (qint64)(totalSize * 1024 * 1024 * 1024);
            else totalBytes = (qint64)totalSize;

            qint64 downloaded = (qint64)((percent / 100.0) * totalBytes);
            emit downloadProgress(id, downloaded, totalBytes);
        }
    }

    // Drain all remaining stdout (and any partial stderr) so the OS pipe buffers never
    // fill up and cause yt-dlp to block. This is the key fix for downloads stalling
    // after a number of files.
    (void)process->readAllStandardOutput();
    (void)process->readAllStandardError();
}

void YtDlpManager::armStallWatchdog(int id) {
    if (!activeProcesses.contains(id)) return;
    QTimer*& timer = stallTimers[id];
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        const int kStallTimeoutMs = 60 * 1000; // 60s with no output == stalled
        timer->setInterval(kStallTimeoutMs);
        connect(timer, &QTimer::timeout, this, [this, id]() {
            if (!activeProcesses.contains(id)) return;
            stopStallWatchdog(id);
            QProcess* p = activeProcesses.take(id);
            QString info;
            if (p) {
                info = QString::fromUtf8(p->readAllStandardOutput()).trimmed();
                QString infoErr = QString::fromUtf8(p->readAllStandardError()).trimmed();
                if (!infoErr.isEmpty()) info = info.isEmpty() ? infoErr : infoErr + "\n" + info;
                if (p->state() != QProcess::NotRunning) {
                    p->kill();
                    p->waitForFinished(2000);
                }
                p->deleteLater();
            }
            Logger::instance().error("yt-dlp download stalled and was killed: " + QString::number(id));
            emit downloadFailed(id,
                "yt-dlp produced no progress for a long time and was stopped. " +
                (info.isEmpty() ? QString() : QString("\n") + info));
        });
    }
    timer->start();
}

void YtDlpManager::stopStallWatchdog(int id) {
    if (auto it = stallTimers.find(id); it != stallTimers.end()) {
        if (it.value()) it.value()->stop();
        it.value()->deleteLater();
        stallTimers.erase(it);
    }
}

void YtDlpManager::pauseDownload(int id) {
    if (!activeProcesses.contains(id)) return;
#ifdef PLATFORM_WINDOWS
    QProcess::execute("powershell", QStringList() << "-Command" << "(Get-Process -Id " + QString::number(activeProcesses[id]->processId()) + ").Suspend()");
#else
    kill(activeProcesses[id]->processId(), SIGSTOP);
#endif
    Logger::instance().info("yt-dlp download paused: " + QString::number(id));
}

void YtDlpManager::resumeDownload(int id) {
    if (!activeProcesses.contains(id)) return;
#ifdef PLATFORM_WINDOWS
    QProcess::execute("powershell", QStringList() << "-Command" << "(Get-Process -Id " + QString::number(activeProcesses[id]->processId()) + ").Resume()");
#else
    kill(activeProcesses[id]->processId(), SIGCONT);
#endif
    Logger::instance().info("yt-dlp download resumed: " + QString::number(id));
}

void YtDlpManager::cancelDownload(int id) {
    if (!activeProcesses.contains(id)) return;
    QProcess* process = activeProcesses.take(id);
    QObject::disconnect(process, nullptr, this, nullptr);
    process->kill();
    process->deleteLater();
    Logger::instance().info("yt-dlp download cancelled: " + QString::number(id));
}

void YtDlpManager::fetchVideoInfo(const QString& url, std::function<void(const QString&)> callback) {
    if (!isInstalled()) {
        callback("yt-dlp not installed");
        return;
    }

    QProcess* process = new QProcess(this);
    QStringList args;
    args << "-j" << "--no-playlist" << url;

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [process, callback](int, QProcess::ExitStatus) {
        QString output = QString::fromUtf8(process->readAllStandardOutput());
        callback(output);
        process->deleteLater();
    });

    process->start(getYtDlpPath(), args);
}

void YtDlpManager::fetchPlaylistInfo(const QString& url, std::function<void(const QVector<PlaylistEntry>&)> callback) {
    if (!isInstalled()) {
        Logger::instance().error("yt-dlp not installed");
        callback(QVector<PlaylistEntry>());
        return;
    }

    QProcess* process = new QProcess(this);
    QStringList args;
    args << "--flat-playlist" << "-J" << url;

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [process, callback](int exitCode, QProcess::ExitStatus) {
        QVector<PlaylistEntry> entries;

        if (exitCode == 0) {
            QByteArray output = process->readAllStandardOutput();
            QJsonDocument doc = QJsonDocument::fromJson(output);

            if (doc.isObject()) {
                QJsonObject obj = doc.object();

                if (obj.contains("entries")) {
                    QJsonArray arr = obj["entries"].toArray();
                    for (int i = 0; i < arr.size(); i++) {
                        QJsonObject entryObj = arr[i].toObject();
                        PlaylistEntry entry;
                        entry.index = i + 1;
                        entry.url = entryObj["url"].toString();
                        if (entry.url.isEmpty()) {
                            entry.url = entryObj["id"].toString();
                            if (!entry.url.isEmpty()) {
                                entry.url = "https://www.youtube.com/watch?v=" + entry.url;
                            }
                        }
                        entry.title = entryObj["title"].toString();
                        if (entry.title.isEmpty()) entry.title = "Video " + QString::number(i + 1);
                        entry.extension = "mp4";
                        entry.fileSize = entryObj["duration_string"].toString();
                        entry.selected = true;
                        entries.append(entry);
                    }
                } else {
                    PlaylistEntry entry;
                    entry.index = 1;
                    entry.url = obj["webpage_url"].toString();
                    entry.title = obj["title"].toString();
                    entry.extension = "mp4";
                    entry.selected = true;
                    entries.append(entry);
                }
            }
        }

        Logger::instance().info("Fetched " + QString::number(entries.size()) + " playlist entries");
        callback(entries);
        process->deleteLater();
    });

    process->start(getYtDlpPath(), args);
}

bool YtDlpManager::isRunning(int id) const {
    return activeProcesses.contains(id);
}
