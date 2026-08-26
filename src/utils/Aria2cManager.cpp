#include "utils/Aria2cManager.h"
#include "utils/Logger.h"
#include "db/DatabaseManager.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QOperatingSystemVersion>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFileInfo>
#include <QRegularExpression>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QEventLoop>

Aria2cManager::Aria2cManager() : nextId(1), maxConcurrent(3), isDownloading(false), nam(new QNetworkAccessManager(this)), activeReply(nullptr) {}

Aria2cManager& Aria2cManager::instance() {
    static Aria2cManager instance;
    return instance;
}

QString Aria2cManager::getAria2cPath() {
#ifdef PLATFORM_WINDOWS
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/tools";
#else
    QString dir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.copper/tools";
#endif
    QDir().mkpath(dir);
    return dir + "/aria2c" + (QOperatingSystemVersion::currentType() == QOperatingSystemVersion::Windows ? ".exe" : "");
}

bool Aria2cManager::isInstalled() {
    return QFile::exists(getAria2cPath());
}

QString Aria2cManager::getVersion() {
    if (!isInstalled()) return "Not installed";
    QProcess process;
    process.start(getAria2cPath(), QStringList() << "--version");
    process.waitForFinished(5000);
    return process.readAllStandardOutput().trimmed().split('\n').first();
}

void Aria2cManager::installOrUpdate() {
    if (isDownloading) {
        Logger::instance().info("aria2c installation already in progress");
        return;
    }

    isDownloading = true;
    Logger::instance().info("Installing/updating aria2c...");
    emit installationProgress("Starting download...");

#ifdef PLATFORM_WINDOWS
    QString url = "https://github.com/aria2/aria2/releases/download/release-1.37.0/aria2-1.37.0-win-64bit-build1.zip";
    QString fileName = "aria2-1.37.0-win-64bit-build1.zip";
#else
    QString url = "https://github.com/aria2/aria2/releases/download/release-1.37.0/aria2-1.37.0.tar.bz2";
    QString fileName = "aria2-1.37.0.tar.bz2";
#endif

    emit installationProgress("Downloading aria2c...");
    startDownload(url, fileName);
}

bool Aria2cManager::ensureInstalled() {
    if (isInstalled()) return true;

    Logger::instance().info("aria2c not found, auto-installing...");

#ifdef PLATFORM_WINDOWS
    QString url = "https://github.com/aria2/aria2/releases/download/release-1.37.0/aria2-1.37.0-win-64bit-build1.zip";
    QString fileName = "aria2-1.37.0-win-64bit-build1.zip";
#else
    return false;
#endif

    QString toolsDir = getToolsDir();
    QDir().mkpath(toolsDir);
    QString zipPath = toolsDir + "/" + fileName;

    QEventLoop loop;
    bool downloadOk = false;

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");

    QNetworkReply* reply = nam->get(request);

    connect(reply, &QNetworkReply::downloadProgress, [](qint64 received, qint64 total) {
        if (total > 0) {
            int pct = (int)((received * 100) / total);
            Logger::instance().info("Downloading aria2c: " + QString::number(pct) + "%");
        }
    });

    connect(reply, &QNetworkReply::finished, [&]() {
        if (reply->error() != QNetworkReply::NoError) {
            Logger::instance().error("aria2c download failed: " + reply->errorString());
            reply->deleteLater();
            loop.quit();
            return;
        }

        int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpStatus >= 300 && httpStatus < 400) {
            QUrl redirectUrl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
            reply->deleteLater();
            QNetworkRequest redirRequest(redirectUrl);
            redirRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            redirRequest.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
            reply = nam->get(redirRequest);
            connect(reply, &QNetworkReply::finished, this, [&]() {
                QByteArray data = reply->readAll();
                reply->deleteLater();
                QFile zipFile(zipPath);
                if (zipFile.open(QIODevice::WriteOnly)) {
                    zipFile.write(data);
                    zipFile.close();
                }
                bool ok = extractAria2c(zipPath);
                QFile::remove(zipPath);
                if (ok && isInstalled()) {
                    Logger::instance().info("aria2c auto-installed: " + getVersion());
                    downloadOk = true;
                }
                loop.quit();
            });
            return;
        }

        QByteArray data = reply->readAll();
        reply->deleteLater();

        QFile zipFile(zipPath);
        if (!zipFile.open(QIODevice::WriteOnly)) {
            Logger::instance().error("Failed to write aria2c zip");
            loop.quit();
            return;
        }
        zipFile.write(data);
        zipFile.close();

        bool ok = extractAria2c(zipPath);
        QFile::remove(zipPath);

        if (ok && isInstalled()) {
            Logger::instance().info("aria2c auto-installed: " + getVersion());
            downloadOk = true;
        } else {
            Logger::instance().error("aria2c auto-install failed");
        }

        loop.quit();
    });

    loop.exec();
    return downloadOk;
}

QString Aria2cManager::getToolsDir() {
#ifdef PLATFORM_WINDOWS
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/tools";
#else
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/.copper/tools";
#endif
}

void Aria2cManager::startDownload(const QString& url, const QString& fileName) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");

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
            Logger::instance().error("aria2c download failed: " + reply->errorString());
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
            startDownload(redirectUrl.toString(), fileName);
            return;
        }

        QByteArray data = reply->readAll();
        reply->deleteLater();

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

        emit installationProgress("Extracting...");
        bool ok = extractAria2c(zipPath);
        QFile::remove(zipPath);

        if (ok && isInstalled()) {
            emit installationProgress("Installed: " + getVersion());
            Logger::instance().info("aria2c installed successfully");
        } else {
            emit installationProgress("Installation failed");
            emit errorOccurred("Installation failed");
        }

        isDownloading = false;
    });
}

bool Aria2cManager::extractAria2c(const QString& zipPath) {
#ifdef PLATFORM_WINDOWS
    QString toolsDir = getToolsDir();
    QString extractDir = toolsDir + "/aria2_tmp";

    QDir dir;
    if (dir.exists(extractDir)) dir.removeRecursively();
    dir.mkpath(extractDir);

    QString psCmd = "Expand-Archive -Path '" + zipPath + "' -DestinationPath '" + extractDir + "' -Force";
    QProcess process;
    process.start("powershell", QStringList() << "-NoProfile" << "-Command" << psCmd);
    process.waitForFinished(120000);

    if (process.exitCode() != 0) {
        Logger::instance().error("PowerShell error: " + process.readAllStandardError());
        return false;
    }

    QDirIterator it(extractDir, QDir::Files, QDirIterator::Subdirectories);
    bool found = false;
    while (it.hasNext()) {
        QString fp = it.next();
        QString fn = QFileInfo(fp).fileName();
        if (fn == "aria2c.exe") {
            QString dest = toolsDir + "/aria2c.exe";
            QFile::remove(dest);
            if (QFile::copy(fp, dest)) {
                QFile::setPermissions(dest, QFileDevice::ExeUser | QFileDevice::ExeOwner | QFileDevice::ExeOther);
                found = true;
            }
        }
    }

    dir.removeRecursively();
    return found;
#else
    return false;
#endif
}

int Aria2cManager::addTorrent(const QString& magnetOrFile, const QString& savePath) {
    if (!isInstalled()) {
        Logger::instance().error("aria2c not installed");
        return -1;
    }

    QDir().mkpath(savePath);
    int id = getNextId();

    Aria2cDownloadTask task;
    task.id = id;
    task.url = magnetOrFile;
    task.outputPath = savePath;
    task.process = nullptr;
    task.isRunning = false;
    task.downloadedBytes = 0;
    task.totalBytes = 0;
    task.speed = 0;

    tasks[id] = task;

    QStringList args;
    args << "--dir=" + savePath;
    args << "--seed-time=0";
    args << "--summary-interval=1";
    args << "--console-log-level=warn";

    QString trackerStr = DatabaseManager::instance().getSetting("defaultTrackers", "");
    QStringList trackerList = trackerStr.isEmpty() ?
        QStringList() << "udp://tracker.opentrackr.org:1337/announce" << "udp://open.stealth.si:80/announce" << "udp://tracker.torrent.eu.org:451/announce"
        : trackerStr.split("\n", Qt::SkipEmptyParts);
    QString trackers = trackerList.join(",");
    if (!trackers.isEmpty()) {
        args << "--bt-tracker=" + trackers;
    }

    if (magnetOrFile.startsWith("magnet:?")) {
        args << magnetOrFile;
    } else if (QFile::exists(magnetOrFile)) {
        args << magnetOrFile;
    } else {
        Logger::instance().error("Torrent file not found: " + magnetOrFile);
        return -1;
    }

    QProcess* process = new QProcess(this);
    task.process = process;
    task.isRunning = true;
    tasks[id] = task;

    connect(process, &QProcess::readyReadStandardOutput, this, [this, id]() {
        if (!tasks.contains(id)) return;
        QByteArray data = tasks[id].process->readAllStandardOutput();
        QStringList lines = QString::fromUtf8(data).split(QRegularExpression("[\\r\\n]"), Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            parseProgress(line.trimmed(), id);
        }
    });

    connect(process, &QProcess::readyReadStandardError, this, [this, id]() {
        if (!tasks.contains(id)) return;
        QByteArray data = tasks[id].process->readAllStandardError();
        QStringList lines = QString::fromUtf8(data).split(QRegularExpression("[\\r\\n]"), Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            parseProgress(line.trimmed(), id);
        }
    });

    connect(process, &QProcess::finished, this, [this, id](int exitCode, QProcess::ExitStatus) {
        if (!tasks.contains(id)) return;
        tasks[id].isRunning = false;

        if (exitCode == 0) {
            emit downloadFinished(id);
        } else {
            QString err = tasks[id].process ? QString::fromUtf8(tasks[id].process->readAllStandardError()) : "Unknown error";
            emit downloadFailed(id, err);
        }

        tasks[id].process->deleteLater();
        tasks.remove(id);
    });

    process->start(getAria2cPath(), args);
    Logger::instance().info("aria2c started, id=" + QString::number(id));
    return id;
}

int Aria2cManager::addTorrentWithSelection(const QString& magnetOrFile, const QString& savePath, const QVector<int>& selectedIndices) {
    if (!isInstalled()) {
        Logger::instance().error("aria2c not installed");
        return -1;
    }

    QDir().mkpath(savePath);
    int id = getNextId();

    Aria2cDownloadTask task;
    task.id = id;
    task.url = magnetOrFile;
    task.outputPath = savePath;
    task.process = nullptr;
    task.isRunning = false;
    task.downloadedBytes = 0;
    task.totalBytes = 0;
    task.speed = 0;

    tasks[id] = task;

    QStringList args;
    args << "--dir=" + savePath;
    args << "--seed-time=0";
    args << "--summary-interval=1";
    args << "--console-log-level=warn";

    QString trackerStr = DatabaseManager::instance().getSetting("defaultTrackers", "");
    QStringList trackerList = trackerStr.isEmpty() ?
        QStringList() << "udp://tracker.opentrackr.org:1337/announce" << "udp://open.stealth.si:80/announce" << "udp://tracker.torrent.eu.org:451/announce"
        : trackerStr.split("\n", Qt::SkipEmptyParts);
    QString trackers = trackerList.join(",");
    if (!trackers.isEmpty()) {
        args << "--bt-tracker=" + trackers;
    }

    bool isMagnet = magnetOrFile.startsWith("magnet:?");

    if (!selectedIndices.isEmpty()) {
        QStringList idxStrs;
        for (int idx : selectedIndices) {
            idxStrs.append(QString::number(idx));
        }
        args << "--select-file=" + idxStrs.join(",");
    }

    if (isMagnet) {
        args << magnetOrFile;
    } else if (QFile::exists(magnetOrFile)) {
        args << magnetOrFile;
    } else {
        Logger::instance().error("Torrent file not found: " + magnetOrFile);
        return -1;
    }

    Logger::instance().info("Starting torrent download: " + magnetOrFile.left(80) + " selected=" + QString::number(selectedIndices.size()) + " files, save=" + savePath);

    QProcess* process = new QProcess(this);
    task.process = process;
    task.isRunning = true;
    tasks[id] = task;

    connect(process, &QProcess::readyReadStandardOutput, this, [this, id]() {
        if (!tasks.contains(id)) return;
        QByteArray data = tasks[id].process->readAllStandardOutput();
        QStringList lines = QString::fromUtf8(data).split(QRegularExpression("[\\r\\n]"), Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            parseProgress(line.trimmed(), id);
        }
    });

    connect(process, &QProcess::readyReadStandardError, this, [this, id]() {
        if (!tasks.contains(id)) return;
        QByteArray data = tasks[id].process->readAllStandardError();
        QStringList lines = QString::fromUtf8(data).split(QRegularExpression("[\\r\\n]"), Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            parseProgress(line.trimmed(), id);
        }
    });

    connect(process, &QProcess::finished, this, [this, id](int exitCode, QProcess::ExitStatus) {
        if (!tasks.contains(id)) return;
        tasks[id].isRunning = false;

        if (exitCode == 0) {
            emit downloadFinished(id);
        } else {
            QString err = tasks[id].process ? QString::fromUtf8(tasks[id].process->readAllStandardError()) : "Unknown error";
            emit downloadFailed(id, err);
        }

        tasks[id].process->deleteLater();
        tasks.remove(id);
    });

    process->start(getAria2cPath(), args);
    Logger::instance().info("aria2c torrent started, id=" + QString::number(id) + ", selected=" + QString::number(selectedIndices.size()) + ", save=" + savePath);
    return id;
}

void Aria2cManager::fetchTorrentFiles(const QString& magnetOrFile, std::function<void(const QVector<PlaylistEntry>&, const TorrentInfo&)> callback) {
    if (!isInstalled()) {
        Logger::instance().info("aria2c not found, attempting auto-install...");
        bool installed = ensureInstalled();
        if (!installed) {
            Logger::instance().error("aria2c could not be installed automatically");
            callback(QVector<PlaylistEntry>(), TorrentInfo());
            return;
        }
    }

    bool isMagnet = magnetOrFile.startsWith("magnet:?");
    bool isTorrentFile = QFile::exists(magnetOrFile) && magnetOrFile.endsWith(".torrent", Qt::CaseInsensitive);

    if (isMagnet) {
        fetchMagnetMetadata(magnetOrFile, callback);
    } else if (isTorrentFile) {
        fetchTorrentFileList(magnetOrFile, callback);
    } else {
        Logger::instance().error("Invalid torrent source: " + magnetOrFile);
        callback(QVector<PlaylistEntry>(), TorrentInfo());
    }
}

void Aria2cManager::fetchMagnetMetadata(const QString& magnet, std::function<void(const QVector<PlaylistEntry>&, const TorrentInfo&)> callback) {
    QString tmpDir = QDir::tempPath() + "/copper_torrent_meta";
    QDir().mkpath(tmpDir);

    QProcess* process = new QProcess();
    QStringList args;
    args << "--bt-metadata-only=true";
    args << "--bt-save-metadata=true";
    args << "--bt-stop-timeout=60";
    args << "--seed-time=0";
    args << "--summary-interval=0";
    args << "--dir=" + tmpDir;
    args << magnet;

    Logger::instance().info("Fetching magnet metadata...");

    connect(process, &QProcess::finished, this, [this, process, callback, tmpDir, magnet](int exitCode, QProcess::ExitStatus) {
        QByteArray allOutput = process->readAllStandardOutput() + process->readAllStandardError();
        QString data = QString::fromUtf8(allOutput);
        Logger::instance().info("Magnet metadata fetch output:\n" + data.left(3000));
        process->deleteLater();

        TorrentInfo info;
        info.magnetUri = magnet;

        QRegularExpression hashRegex("btih:([A-Fa-f0-9]{40})", QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatch hashMatch = hashRegex.match(magnet);
        if (hashMatch.hasMatch()) {
            info.infoHash = hashMatch.captured(1).toLower();
        }

        QRegularExpression dnRegex("dn=([^&]+)");
        QRegularExpressionMatch dnMatch = dnRegex.match(magnet);
        if (dnMatch.hasMatch()) {
            info.name = QUrl::fromPercentEncoding(dnMatch.captured(1).toUtf8());
        }

        if (exitCode != 0) {
            Logger::instance().error("Failed to fetch magnet metadata");
            callback(QVector<PlaylistEntry>(), info);
            return;
        }

        QStringList torrentFiles;
        QDirIterator it(tmpDir, QStringList() << "*.torrent", QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            torrentFiles.append(it.next());
        }

        if (!info.infoHash.isEmpty()) {
            QString expected = tmpDir + "/" + info.infoHash + ".torrent";
            if (QFile::exists(expected)) {
                torrentFiles.clear();
                torrentFiles.append(expected);
            }
        }

        if (torrentFiles.isEmpty()) {
            Logger::instance().error("No .torrent metadata file found in " + tmpDir);
            callback(QVector<PlaylistEntry>(), info);
            return;
        }

        QString torrentPath = torrentFiles.first();
        Logger::instance().info("Metadata saved to: " + torrentPath + ", now fetching file list...");
        fetchTorrentFileList(torrentPath, callback);
    });

    process->start(getAria2cPath(), args);
}

void Aria2cManager::fetchTorrentFileList(const QString& torrentPath, std::function<void(const QVector<PlaylistEntry>&, const TorrentInfo&)> callback) {
    QProcess* process = new QProcess();
    QStringList args;
    args << "--show-files";
    args << "--dry-run";
    args << "--seed-time=0";
    args << "--summary-interval=0";
    args << torrentPath;

    connect(process, &QProcess::finished, this, [this, process, callback](int exitCode, QProcess::ExitStatus) {
        QByteArray output = process->readAllStandardOutput();
        QByteArray errOutput = process->readAllStandardError();
        QByteArray allOutput = output + errOutput;
        process->deleteLater();

        QString data = QString::fromUtf8(allOutput);
        QStringList lines = data.split("\n");

        Logger::instance().info("aria2c --show-files output:\n" + data.left(5000));

        TorrentInfo info;
        QVector<PlaylistEntry> entries;
        bool inFilesSection = false;
        bool inAnnounceSection = false;

        for (int i = 0; i < lines.size(); i++) {
            QString line = lines[i];
            QString trimmed = line.trimmed();

            if (!inFilesSection) {
                if (trimmed.startsWith("Name:")) {
                    info.name = trimmed.mid(5).trimmed();
                } else if (trimmed.startsWith("Info Hash:")) {
                    info.infoHash = trimmed.mid(10).trimmed();
                } else if (trimmed.startsWith("Total Length:")) {
                    info.totalSize = trimmed.mid(13).trimmed();
                } else if (trimmed.startsWith("Piece Length:")) {
                    QString pl = trimmed.mid(13).trimmed();
                    QRegularExpression plRegex("(\\d+)");
                    QRegularExpressionMatch plMatch = plRegex.match(pl);
                    if (plMatch.hasMatch()) info.pieceLength = plMatch.captured(1).toInt();
                } else if (trimmed.startsWith("The Number of Pieces:")) {
                    QString np = trimmed.mid(21).trimmed();
                    bool ok;
                    int n = np.toInt(&ok);
                    if (ok) info.numberOfPieces = n;
                } else if (trimmed.startsWith("Announce:") || trimmed.startsWith("Announce List:")) {
                    inAnnounceSection = true;
                    QString after = trimmed.mid(trimmed.indexOf(":") + 1).trimmed();
                    if (!after.isEmpty()) {
                        info.trackers.append(after);
                    }
                } else if (inAnnounceSection) {
                    if (trimmed.startsWith("udp://") || trimmed.startsWith("http://") || trimmed.startsWith("https://") || trimmed.startsWith("wss://")) {
                        info.trackers.append(trimmed);
                    } else {
                        inAnnounceSection = false;
                    }
                }

                if (trimmed == "Files:" || trimmed.startsWith("idx|path")) {
                    inFilesSection = true;
                    continue;
                }
                continue;
            }

            if (trimmed.startsWith("===") || trimmed.startsWith("---")) {
                continue;
            }
            if (trimmed.startsWith(">>>")) continue;

            QRegularExpression idxRegex("^\\s*(\\d+)\\|(.+)$");
            QRegularExpressionMatch idxMatch = idxRegex.match(line);
            if (idxMatch.hasMatch()) {
                int fileIdx = idxMatch.captured(1).toInt();
                QString filePath = idxMatch.captured(2).trimmed();
                QString fileName = QFileInfo(filePath).fileName();

                QString fileSize = "Unknown";
                qint64 fileSizeBytes = 0;
                if (i + 1 < lines.size()) {
                    QString nextLine = lines[i + 1].trimmed();
                    QRegularExpression sizeRegex("^\\|(.+)$");
                    QRegularExpressionMatch sizeMatch = sizeRegex.match(nextLine);
                    if (sizeMatch.hasMatch()) {
                        fileSize = sizeMatch.captured(1).trimmed();
                        QRegularExpression bytesRegex("\\((\\d+)\\)");
                        QRegularExpressionMatch bytesMatch = bytesRegex.match(fileSize);
                        if (bytesMatch.hasMatch()) {
                            fileSizeBytes = bytesMatch.captured(1).toLongLong();
                        }
                    }
                }

                PlaylistEntry entry;
                entry.index = fileIdx;
                entry.title = fileName;
                entry.fileSize = fileSize;
                entry.fileSizeBytes = fileSizeBytes;
                entry.selected = true;
                entries.append(entry);
            }
        }

        if (entries.isEmpty()) {
            Logger::instance().warning("Could not parse file list from aria2c output, using fallback parsing");
            for (const QString& line : lines) {
                QString trimmed = line.trimmed();
                if (trimmed.isEmpty()) continue;

                QRegularExpression fallbackRegex("^(\\d+)\\|(.+)$");
                QRegularExpressionMatch fallbackMatch = fallbackRegex.match(trimmed);
                if (fallbackMatch.hasMatch()) {
                    int idx = fallbackMatch.captured(1).toInt();
                    QString path = fallbackMatch.captured(2).trimmed();
                    QString name = QFileInfo(path).fileName();
                    if (!name.isEmpty()) {
                        PlaylistEntry entry;
                        entry.index = idx;
                        entry.title = name;
                        entry.fileSize = "Unknown";
                        entry.selected = true;
                        entries.append(entry);
                    }
                }
            }
        }

        info.fileCount = entries.size();
        Logger::instance().info("Parsed " + QString::number(entries.size()) + " file(s) from torrent, name=" + info.name);
        callback(entries, info);
    });

    process->start(getAria2cPath(), args);
}

void Aria2cManager::parseProgress(const QString& line, int id) {
    if (!tasks.contains(id)) return;
    if (line.contains('#') || line.contains('%') || line.contains("DL:")) {
        Logger::instance().info("aria2c[" + QString::number(id) + "]: " + line);
    }

    QRegularExpression progressRegex("\\((\\d+)%\\)");
    QRegularExpressionMatch match = progressRegex.match(line);

    if (match.hasMatch()) {
        double percent = match.captured(1).toDouble();

        QRegularExpression sizeRegex("([\\d.]+)(B|KiB|MiB|GiB|TiB)/([\\d.]+)(B|KiB|MiB|GiB|TiB)");
        QRegularExpressionMatch sizeMatch = sizeRegex.match(line);
        if (sizeMatch.hasMatch()) {
            auto parseSize = [](const QString& val, const QString& unit) -> qint64 {
                double num = val.toDouble();
                if (unit == "B") return (qint64)num;
                if (unit == "KiB") return (qint64)(num * 1024);
                if (unit == "MiB") return (qint64)(num * 1024 * 1024);
                if (unit == "GiB") return (qint64)(num * 1024 * 1024 * 1024);
                if (unit == "TiB") return (qint64)(num * 1024.0 * 1024 * 1024 * 1024);
                return (qint64)num;
            };
            qint64 downloaded = parseSize(sizeMatch.captured(1), sizeMatch.captured(2));
            qint64 total = parseSize(sizeMatch.captured(3), sizeMatch.captured(4));
            tasks[id].downloadedBytes = downloaded;
            tasks[id].totalBytes = total;
            emit downloadProgress(id, downloaded, total, tasks[id].speed);
        } else {
            qint64 totalBytes = tasks[id].totalBytes;
            qint64 downloadedBytes = (qint64)((percent / 100.0) * totalBytes);
            tasks[id].downloadedBytes = downloadedBytes;
            emit downloadProgress(id, downloadedBytes, totalBytes, tasks[id].speed);
        }
    }

    QRegularExpression speedRegex("DL:([\\d.]+)(B|KiB|MiB|GiB)/s");
    QRegularExpressionMatch speedMatch = speedRegex.match(line);
    if (speedMatch.hasMatch()) {
        double spd = speedMatch.captured(1).toDouble();
        QString unit = speedMatch.captured(2);

        if (unit == "KiB") tasks[id].speed = (qint64)(spd * 1024);
        else if (unit == "MiB") tasks[id].speed = (qint64)(spd * 1024 * 1024);
        else if (unit == "GiB") tasks[id].speed = (qint64)(spd * 1024 * 1024 * 1024);
        else tasks[id].speed = (qint64)spd;
    }

    QRegularExpression cnRegex("CN:(\\d+)");
    QRegularExpressionMatch cnMatch = cnRegex.match(line);
    if (cnMatch.hasMatch()) {
        tasks[id].connectedPeers = cnMatch.captured(1).toInt();
    }

    QRegularExpression sdRegex("SD:(\\d+)");
    QRegularExpressionMatch sdMatch = sdRegex.match(line);
    if (sdMatch.hasMatch()) {
        tasks[id].seeds = sdMatch.captured(1).toInt();
    }
}

int Aria2cManager::getNextId() {
    return nextId++;
}

void Aria2cManager::pauseDownload(int id) {
    if (!tasks.contains(id)) return;
    if (tasks[id].process && tasks[id].isRunning) {
#ifdef PLATFORM_WINDOWS
        QProcess::execute("powershell", QStringList() << "-Command" << "(Get-Process -Id " + QString::number(tasks[id].process->processId()) + ").Suspend()");
#else
        kill(tasks[id].process->processId(), SIGSTOP);
#endif
        tasks[id].isRunning = false;
        Logger::instance().info("aria2c download paused: " + QString::number(id));
    }
}

void Aria2cManager::resumeDownload(int id) {
    if (!tasks.contains(id)) return;
    if (tasks[id].process && !tasks[id].isRunning) {
#ifdef PLATFORM_WINDOWS
        QProcess::execute("powershell", QStringList() << "-Command" << "(Get-Process -Id " + QString::number(tasks[id].process->processId()) + ").Resume()");
#else
        kill(tasks[id].process->processId(), SIGCONT);
#endif
        tasks[id].isRunning = true;
        Logger::instance().info("aria2c download resumed: " + QString::number(id));
    }
}

void Aria2cManager::removeDownload(int id) {
    if (!tasks.contains(id)) return;
    if (tasks[id].process) {
        tasks[id].process->kill();
        tasks[id].process->deleteLater();
    }
    tasks.remove(id);
}

void Aria2cManager::addTrackers(int torrentId, const QStringList& trackers) {
    Logger::instance().info("Adding trackers to torrent " + QString::number(torrentId) + ": " + QString::number(trackers.size()) + " trackers");
}

bool Aria2cManager::isRunning(int id) const {
    return tasks.contains(id) && tasks[id].isRunning;
}

int Aria2cManager::getConnectedPeers(int id) const {
    return tasks.contains(id) ? tasks[id].connectedPeers : 0;
}

int Aria2cManager::getSeeds(int id) const {
    return tasks.contains(id) ? tasks[id].seeds : 0;
}
