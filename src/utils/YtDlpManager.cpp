#include "utils/YtDlpManager.h"
#include "utils/Logger.h"
#include "utils/FfmpegManager.h"
#include "utils/UserAgent.h"
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
#include <algorithm>

namespace {

// Progress/print protocol shared with yt-dlp. A custom --progress-template
// replaces yt-dlp's default progress line, which is what lets a single playlist
// process report per-video numbers the table can attribute to individual rows.
//   COPPER|<state>|<downloaded>|<total>|<speed>|<eta>|<info.id>|<frag idx>|<frag count>
//   COPPERPATH|<playlist_index or -1>|<final path>
const char* kProgressTemplate =
    "download:COPPER|%(progress.status)s|%(progress.downloaded_bytes)s|%(progress.total_bytes)s|"
    "%(progress.speed)s|%(progress.eta)s|%(info.id)s|%(progress.fragment_index)s|%(progress.fragment_count)s";

// yt-dlp prints "NA" for unknown values in templates.
qint64 parseNum(const QString& s, qint64 def = -1) {
    if (s.isEmpty() || s == "NA") return def;
    bool ok = false;
    double v = s.toDouble(&ok);
    if (!ok) return def;
    if (v < 0) return def;
    return (qint64)v;
}

int parseInt(const QString& s, int def = -1) {
    if (s.isEmpty() || s == "NA") return def;
    bool ok = false;
    int v = s.toInt(&ok);
    return ok ? v : def;
}

// Kill the child and its descendants. On Windows terminate()/kill() only reach
// the direct process, which would orphan the ffmpeg child yt-dlp spawns for
// merging; taskkill /T takes the whole tree down.
void killProcessTree(QProcess* p) {
    if (!p || p->state() == QProcess::NotRunning) return;
#ifdef PLATFORM_WINDOWS
    if (p->processId() > 0) {
        QProcess::execute("taskkill", QStringList() << "/T" << "/F" << "/PID" << QString::number(p->processId()));
    }
    p->waitForFinished(3000);
#else
    p->terminate();
    if (!p->waitForFinished(3000)) p->kill();
#endif
}

}  // namespace

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
    request.setRawHeader("User-Agent", Copper::bareUserAgent().toUtf8());

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
    request.setRawHeader("User-Agent", Copper::copperUserAgent().toUtf8());
    request.setRawHeader("Accept", "application/octet-stream");

    activeReply = nam->get(request);

    connect(activeReply, &QNetworkReply::downloadProgress, [this](qint64 received, qint64 total) {
        if (total > 0) {
            int pct = (int)((received * 100) / total);
            emit installationProgress("Downloading: " + QString::number(pct) + "%");
        }
    });

    connect(activeReply, &QNetworkReply::finished, this, [this, fileName]() {
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

// ---------------------------------------------------------------------------
// Downloads
// ---------------------------------------------------------------------------

bool YtDlpManager::hasFfmpegFor(const QString& format) const {
    return format != "mkv" && format != "best" && FfmpegManager::instance().isInstalled();
}

QString YtDlpManager::getOutputTemplate(const QString& outputPath) const {
    // A bare path with no extension and no placeholder makes yt-dlp write an
    // extension-less file, which looks like the download never happened. Fall
    // back to a template in the parent directory so the file is named properly.
    if (outputPath.contains("%(")) return outputPath;
    if (!QFileInfo(outputPath).suffix().isEmpty()) return outputPath;
    QString outDir = QFileInfo(outputPath).absolutePath();
    if (outDir.isEmpty()) outDir = outputPath;
    return outDir + "/%(title)s.%(ext)s";
}

void YtDlpManager::startDownload(const QString& url, const QString& outputPath, int downloadId, const QString& format) {
    if (!isInstalled()) {
        emit downloadFailed(downloadId, "yt-dlp not installed");
        return;
    }

    if (!hasFfmpegFor(format)) {
        emit downloadFailed(downloadId,
            "FFmpeg is required for \"" + format + "\" output but is not installed. "
            "Install it from Settings > Tools, then retry the download.");
        return;
    }

    YtDlpJobSpec spec;
    spec.url = url;
    spec.outputTemplate = getOutputTemplate(outputPath);
    spec.format = format;
    spec.isPlaylist = false;
    launchJob(downloadId, spec);
}

void YtDlpManager::startPlaylistJob(int jobId, const QString& playlistUrl, const QString& outputDir,
                                    int trackNumberWidth, const QString& format, int fragments,
                                    const QVector<int>& selectedItems, bool trackNumbers) {
    // These two are reported through playlistFinished(), not downloadFailed():
    // DownloadManager ignores downloadFailed for folder rows (a folder reports
    // through playlistFinished), so emitting it here would leave the job sitting
    // in "Downloading" forever with no reason shown.
    if (!isInstalled()) {
        emit playlistFinished(jobId, false, "yt-dlp not installed");
        return;
    }

    if (!hasFfmpegFor(format)) {
        emit playlistFinished(jobId, false,
            "FFmpeg is required for \"" + format + "\" output but is not installed. "
            "Install it from Settings > Tools, then retry the download.");
        return;
    }

    int width = trackNumberWidth > 0 ? trackNumberWidth : 3;
    YtDlpJobSpec spec;
    spec.url = playlistUrl;
    // yt-dlp can only number by playlist position. With track numbers on, this
    // template is only the shape the file is written in: DownloadManager renames
    // it to its number in the user's selection once it lands. Without them the
    // prefix must not appear at all (the checkbox used to do nothing here).
    spec.outputTemplate = trackNumbers
                              ? QString("%1/%(playlist_index)0%2d.%(title)s.%(ext)s")
                                    .arg(outputDir)
                                    .arg(width, 2, 10, QChar('0'))
                              : outputDir + "/%(title)s.%(ext)s";
    spec.format = format;
    spec.isPlaylist = true;
    spec.fragments = qBound(1, fragments, 16);
    spec.archivePath = outputDir + "/.copper-archive.txt";
    // Playlist positions are 1-based; drop duplicates and anything out of range.
    QVector<int> items;
    for (int idx : selectedItems) {
        if (idx > 0 && !items.contains(idx)) items.append(idx);
    }
    std::sort(items.begin(), items.end());
    spec.selectedItems = items;
    launchJob(jobId, spec);
}

void YtDlpManager::launchJob(int id, const YtDlpJobSpec& spec) {
    // A restart of the same id replaces the old child (e.g. resume after pause).
    if (jobs.contains(id)) destroyJob(id);

    Logger::instance().info("Starting yt-dlp job: " + spec.url + " (format: " + spec.format +
                            (spec.isPlaylist ? ", playlist" : "") + ")");

    QProcess* process = new QProcess(this);

    YtDlpJob job;
    job.process = process;
    job.spec = spec;
    jobs.insert(id, job);

    QStringList args;
    args << "-o" << spec.outputTemplate;
    args << "--newline";
    args << "--no-warnings";
    args << "--progress";
    // Resume partial .part files instead of restarting the transfer.
    args << "--continue";
    args << "--user-agent" << DatabaseManager::instance().getUserAgent();
#ifdef PLATFORM_WINDOWS
    // Match the app's own filename sanitizing so the file on disk is the file the
    // table (and the resume check on restart) expects.
    args << "--windows-filenames";
#endif
    // Per-video progress + the final path of every finished file.
    args << "--progress-template" << kProgressTemplate;
    args << "--print" << (spec.isPlaylist
                              ? QString("after_move:COPPERPATH|%(playlist_index)s|%(filepath)s")
                              : QString("after_move:COPPERPATH|-1|%(filepath)s"));

    if (spec.isPlaylist) {
        // A single unavailable/removed item must not cancel the whole playlist,
        // and a bounded socket timeout keeps a dead TCP connection from hanging
        // the job forever.
        args << "--ignore-errors";
        args << "--no-abort-on-error";
        args << "--socket-timeout" << "30";
        if (spec.fragments > 1) {
            args << "-N" << QString::number(spec.fragments);
        }
        if (!spec.archivePath.isEmpty()) {
            // Resuming/restarting a job must not re-download items that already
            // finished, which is exactly what this archive makes yt-dlp skip.
            args << "--download-archive" << spec.archivePath;
        }
        if (!spec.selectedItems.isEmpty()) {
            // Only fetch the videos the user actually picked. Without this, pointing
            // yt-dlp at the playlist URL downloads every item and leaves files on
            // disk that no table row refers to.
            QStringList positions;
            for (int idx : spec.selectedItems) positions << QString::number(idx);
            args << "--playlist-items" << positions.join(",");
        }
    }

    if (spec.format == "mp3") {
        args << "--extract-audio";
        args << "--audio-format" << "mp3";
    } else if (spec.format == "mkv") {
        args << "--merge-output-format" << "mkv";
    } else if (spec.format != "best") {
        args << "--merge-output-format" << "mp4";
    }

    args << spec.url;

    // Drain BOTH channels continuously. If stdout is never read the OS pipe
    // buffer fills and yt-dlp blocks, which hangs the transfer.
    connect(process, &QProcess::readyReadStandardOutput, this, [this, id]() { drainOutput(id, false); });
    connect(process, &QProcess::readyReadStandardError, this, [this, id]() { drainOutput(id, true); });

    armStallWatchdog(id);

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, id](int exitCode, QProcess::ExitStatus) { finalizeJob(id, exitCode); });

    // Log the exact command line. "Was the user's selection honoured?" can only
    // be answered from what was actually asked for, and the URL alone does not
    // say it.
    QStringList printable;
    for (const QString& a : args) {
        printable << (a.contains(' ') ? "\"" + a + "\"" : a);
    }
    Logger::instance().info("yt-dlp argv: " + printable.join(" "));

    process->start(getYtDlpPath(), args);
}

void YtDlpManager::drainOutput(int id, bool isStdErr) {
    {
        auto it = jobs.find(id);
        if (it == jobs.end()) return;
        YtDlpJob& job = it.value();
        if (!job.process) return;

        const QByteArray data = isStdErr ? job.process->readAllStandardError()
                                         : job.process->readAllStandardOutput();
        if (data.isEmpty()) return;

        // First byte of output from the child proves it is alive and working.
        if (!job.sawOutput) {
            job.sawOutput = true;
            armStallWatchdog(id);
        }

        QString& buf = isStdErr ? job.errBuf : job.outBuf;
        buf += QString::fromUtf8(data);
        // Keep a bounded tail so a long line cannot grow without limit.
        if (buf.size() > 16384) buf = buf.right(4096);
    }

    // Split and dispatch outside the map lookup: a signal handler reached from
    // handleProtocolLine() can add or remove jobs, which may rehash the map and
    // invalidate any reference into it.
    QStringList lines;
    {
        auto it = jobs.find(id);
        if (it == jobs.end()) return;
        QString& buf = isStdErr ? it.value().errBuf : it.value().outBuf;
        int nl;
        while ((nl = buf.indexOf('\n')) >= 0) {
            lines << buf.left(nl).trimmed();
            buf.remove(0, nl + 1);
        }
    }

    for (const QString& line : lines) {
        if (line.isEmpty()) continue;
        handleLine(id, line, isStdErr);
    }
}

void YtDlpManager::handleLine(int id, const QString& line, bool isStdErr) {
    if (line.isEmpty()) return;
    auto it = jobs.find(id);
    if (it == jobs.end()) return;
    YtDlpJob& job = it.value();

    if (isStdErr) {
        job.errorTail += line + "\n";
        if (job.errorTail.size() > 4000) job.errorTail = job.errorTail.right(2000);
        return;
    }

    handleProtocolLine(id, line);

    // Anything printed at all means the child is alive, so re-arm the watchdog.
    // Only *consecutive* quiet periods count toward giving up: a long playlist
    // spans many videos, each with its own quiet moments (format negotiation,
    // throttled retry, merge), and treating two of those spread across the run
    // as a hang would kill a perfectly healthy job.
    if (auto jt = jobs.find(id); jt != jobs.end()) jt.value().stallTrips = 0;
    armStallWatchdog(id);
}

void YtDlpManager::handleProtocolLine(int id, const QString& line) {
    if (line.startsWith("COPPERPATH|")) {
        const QStringList parts = line.split('|');
        if (parts.size() >= 3) {
            int index = parseInt(parts[1]);
            emit videoPath(id, index, parts.mid(2).join('|'));
        }
        return;
    }

    if (line.startsWith("COPPER|")) {
        const QStringList parts = line.split('|');
        if (parts.size() < 9) return;
        emit videoProgress(id, parts[6], parts[1],
                           parseNum(parts[2]), parseNum(parts[3]),
                           parseNum(parts[4]), parseNum(parts[5]),
                           parseInt(parts[7]), parseInt(parts[8]));
        return;
    }

    // "[download] Destination: <path>" gives the real final file name, and for
    // playlist jobs the leading track number identifies which row it belongs to.
    if (line.startsWith("[download] Destination: ")) {
        QString path = line.mid(QString("[download] Destination: ").size()).trimmed();
        if (path.isEmpty()) return;
        int index = -1;
        int dot = path.indexOf('.');
        if (dot > 0) {
            bool allDigits = true;
            for (int i = 0; i < dot; i++) {
                if (!path.at(i).isDigit()) { allDigits = false; break; }
            }
            if (allDigits) index = path.left(dot).toInt();
        }
        emit videoPath(id, index, path);
        return;
    }

    static const QRegularExpression itemRe("^\\[download\\] Downloading item (\\d+) of (\\d+)");
    QRegularExpressionMatch m = itemRe.match(line);
    if (m.hasMatch()) {
        emit playlistItemStarted(id, m.captured(1).toInt(), m.captured(2).toInt());
    }
}

void YtDlpManager::finalizeJob(int id, int exitCode) {
    // Flush whatever is still buffered so the last progress/path lines and the
    // error text are not lost.
    drainOutput(id, false);
    drainOutput(id, true);

    QString tail;
    bool wasPaused = false;
    bool wasCancelled = false;
    bool isPlaylist = false;
    {
        auto it = jobs.find(id);
        if (it == jobs.end()) return;
        tail = it.value().errorTail.trimmed();
        wasPaused = it.value().paused;
        wasCancelled = it.value().cancelled;
        isPlaylist = it.value().spec.isPlaylist;
        stopStallWatchdog(id);
        if (it.value().process) {
            it.value().process->deleteLater();
            it.value().process = nullptr;
        }
    }
    jobs.remove(id);

    // A pause or a cancel is not a failure: the caller restarts or drops the job.
    if (wasPaused || wasCancelled) {
        Logger::instance().info("yt-dlp job " + QString::number(id) + " stopped (" +
                                (wasPaused ? "paused" : "cancelled") + ")");
        return;
    }

    if (exitCode == 0) {
        Logger::instance().info("yt-dlp job finished: " + QString::number(id));
        emit downloadFinished(id);
        if (isPlaylist) emit playlistFinished(id, true, QString());
    } else {
        QString err = tail;
        if (err.isEmpty()) err = QString("yt-dlp exited with code %1").arg(exitCode);
        Logger::instance().error("yt-dlp job failed: " + err);
        emit downloadFailed(id, err);
        if (isPlaylist) emit playlistFinished(id, false, err);
    }
}

void YtDlpManager::destroyJob(int id) {
    auto it = jobs.find(id);
    if (it == jobs.end()) return;
    stopStallWatchdog(id);
    if (it.value().process) {
        QObject::disconnect(it.value().process, nullptr, this, nullptr);
        killProcessTree(it.value().process);
        it.value().process->deleteLater();
    }
    jobs.erase(it);
}

void YtDlpManager::armStallWatchdog(int id) {
    auto it = jobs.find(id);
    if (it == jobs.end()) return;
    YtDlpJob& job = it.value();

    if (job.paused || job.cancelled) return;

    if (!job.stallTimer) {
        job.stallTimer = new QTimer(this);
        job.stallTimer->setSingleShot(true);
        connect(job.stallTimer, &QTimer::timeout, this, [this, id]() {
            auto jt = jobs.find(id);
            if (jt == jobs.end()) return;
            if (jt.value().paused || jt.value().cancelled) return;

            jt.value().stallTrips++;
            if (jt.value().stallTrips == 1) {
                // yt-dlp is routinely silent for minutes while it extracts a page,
                // negotiates formats, waits out a throttled retry or runs ffmpeg.
                // Killing on the first quiet period is what used to abort healthy
                // playlist downloads, so the first trip only warns.
                Logger::instance().warning("yt-dlp job " + QString::number(id) +
                                           " has produced no output for a long time (still running)");
                armStallWatchdog(id);
                return;
            }

            Logger::instance().error("yt-dlp job " + QString::number(id) + " stalled repeatedly, killing it");
            stopStallWatchdog(id);
            // Let finalizeJob() report it as a genuine failure with a message that
            // explains itself, rather than leaving the row stuck on "Downloading".
            jt.value().errorTail = "yt-dlp produced no output for over 6 minutes and was stopped.\n"
                                   "(The partial file is kept - use Resume to continue it.)";
            if (jt.value().process) killProcessTree(jt.value().process);
        });
    }

    // Before the first byte of output the child may still be extracting; measured
    // silence of >2 minutes is normal, so that window is much wider.
    const int kSteadyStallMs = 180 * 1000;
    const int kStartupStallMs = 300 * 1000;
    job.stallTimer->start(job.sawOutput ? kSteadyStallMs : kStartupStallMs);
}

void YtDlpManager::stopStallWatchdog(int id) {
    auto it = jobs.find(id);
    if (it == jobs.end()) return;
    if (it.value().stallTimer) {
        it.value().stallTimer->stop();
        it.value().stallTimer->deleteLater();
        it.value().stallTimer = nullptr;
    }
}

void YtDlpManager::pauseDownload(int id) {
    auto it = jobs.find(id);
    if (it == jobs.end()) return;
    YtDlpJob& job = it.value();
    if (job.paused) return;

    job.paused = true;
    stopStallWatchdog(id);
    // Terminate rather than suspend the process: a suspended child stops writing
    // output (which used to trip the stall watchdog and get it killed), and its
    // sockets/ffmpeg children go stale. The .part files stay on disk and yt-dlp
    // continues them with --continue when the job is restarted.
    if (job.process) killProcessTree(job.process);
    Logger::instance().info("yt-dlp job paused: " + QString::number(id));
}

void YtDlpManager::resumeDownload(int id) {
    auto it = jobs.find(id);
    if (it == jobs.end()) return;
    YtDlpJob& job = it.value();
    if (!job.paused) return;
    launchJob(id, job.spec);
    Logger::instance().info("yt-dlp job resumed: " + QString::number(id));
}

void YtDlpManager::cancelDownload(int id) {
    auto it = jobs.find(id);
    if (it == jobs.end()) {
        return;
    }
    it.value().cancelled = true;
    Logger::instance().info("yt-dlp job cancelled: " + QString::number(id));
    destroyJob(id);
}

bool YtDlpManager::isRunning(int id) const {
    auto it = jobs.constFind(id);
    if (it == jobs.constEnd()) return false;
    return it.value().process && it.value().process->state() != QProcess::NotRunning;
}

bool YtDlpManager::isPaused(int id) const {
    auto it = jobs.constFind(id);
    if (it == jobs.constEnd()) return false;
    return it.value().paused;
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
                        entry.videoId = entryObj["id"].toString();

                        // Flat-playlist entries carry a full watch URL on current
                        // yt-dlp versions, but older ones only had the bare id.
                        // Accept both so a child row is never left with a URL that
                        // yt-dlp cannot resolve.
                        QString entryUrl = entryObj["url"].toString();
                        if (entryUrl.startsWith("http://") || entryUrl.startsWith("https://")) {
                            entry.url = entryUrl;
                        } else if (entryObj.contains("webpage_url")) {
                            entry.url = entryObj["webpage_url"].toString();
                        } else if (!entry.videoId.isEmpty()) {
                            entry.url = "https://www.youtube.com/watch?v=" + entry.videoId;
                        } else {
                            entry.url = entryUrl;
                        }

                        entry.title = entryObj["title"].toString();
                        if (entry.title.isEmpty()) entry.title = "Video " + QString::number(i + 1);
                        entry.extension = "mp4";
                        entry.fileSize = entryObj["duration_string"].toString();
                        entry.duration = (qint64)entryObj["duration"].toDouble();
                        entry.selected = true;
                        entries.append(entry);
                    }
                } else {
                    PlaylistEntry entry;
                    entry.index = 1;
                    entry.url = obj["webpage_url"].toString();
                    entry.videoId = obj["id"].toString();
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
