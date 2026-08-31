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
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QEventLoop>
#include <QRandomGenerator>
#include <QUrl>
#include <QRandomGenerator>
#include <QThread>
#include <QCoreApplication>
#include <QSet>

Aria2cManager::Aria2cManager() : nextId(1), maxConcurrent(3), isDownloading(false), nam(new QNetworkAccessManager(this)), activeReply(nullptr) {
    // Use a persistent RPC secret persisted across sessions. A per-process random
    // token caused endless "aria2 RPC ... Unauthorized" loops: if an old daemon
    // survived on port 6800 (its socket lingering after a restart), its secret
    // differed from the freshly generated one, so every aria2.getVersion during
    // daemon startup was rejected with "Unauthorized" and downloads never started.
    m_token = DatabaseManager::instance().getSetting("aria2Token", QString());
    if (m_token.isEmpty()) {
        m_token = "copper-" + QString::number(QRandomGenerator::global()->generate()) + QString::number(QRandomGenerator::global()->generate());
        DatabaseManager::instance().saveSetting("aria2Token", m_token);
    }
    pollTimer = new QTimer(this);
    connect(pollTimer, &QTimer::timeout, this, &Aria2cManager::poll);
    pollTimer->setInterval(1000);
}

Aria2cManager::~Aria2cManager() {
    shutdownDaemon();
}

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

    if (isInstalled()) {
        Logger::instance().info("aria2c already installed: " + getVersion() + " (skipping download)");
        emit installationProgress("Already installed: " + getVersion());
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

// ---------------------------------------------------------------------------
// RPC daemon lifecycle
// ---------------------------------------------------------------------------

// Kill whatever process is currently bound to the given TCP port. This clears
// stale/orphaned daemons (left running by a crashed or previous session) that
// would otherwise keep port 6800 occupied and cause the new daemon to fail,
// surfacing as "aria2.addTorrent failed: rpc error" on subsequent launches.
// Returns true if at least one foreign process was killed.
bool Aria2cManager::killProcessOnTcpPort(int port) {
    bool killedAny = false;
#ifdef Q_OS_WIN
    QProcess netstat;
    netstat.start("netstat", QStringList() << "-ano");
    if (!netstat.waitForFinished(3000)) return false;
    const QList<QByteArray> lines = netstat.readAllStandardOutput().split('\n');
    QSet<QString> pids;
    const QString portStr = ":" + QString::number(port);
    for (const QByteArray& line : lines) {
        QString l = QString::fromLocal8Bit(line).trimmed();
        if (!l.contains(portStr)) continue;
        QStringList parts = l.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() >= 5 && parts[parts.size() - 1].toInt() > 0) {
            pids.insert(parts[parts.size() - 1]);
        }
    }
    for (const QString& pid : pids) {
        int pidNo = pid.toInt();
        // Never kill our own app process or the daemon we currently manage (a
        // concurrent startDaemonProcess call could otherwise kill the daemon we
        // just launched while it is still binding, producing an endless
        // "daemon exited ... restarting" loop and "aria2.addTorrent failed".
        if (pidNo == QCoreApplication::applicationPid()) continue;
        if (m_daemonProcess && m_daemonProcess->state() != QProcess::NotRunning &&
            m_daemonProcess->processId() == pidNo) continue;
        Logger::instance().warning("Killing stale process on port " + QString::number(port) + " (PID " + pid + ")");
        QProcess taskkill;
        taskkill.start("taskkill", QStringList() << "/F" << "/T" << "/PID" << pid);
        taskkill.waitForFinished(3000);
        killedAny = true;
    }
#endif
    return killedAny;
}

bool Aria2cManager::startDaemonProcess() {
    if (!isInstalled()) {
        if (!ensureInstalled()) {
            emit errorOccurred("aria2c could not be installed");
            return false;
        }
    }

    // Clear any orphaned daemon still holding the RPC port from a previous
    // session so that the freshly launched daemon can bind successfully.
    // After killing a listener the OS socket often lingers in TIME_WAIT, which
    // briefly blocks rebinding port 6800 and makes the new daemon exit right
    // away (an endless "daemon exited ... restarting" loop). If we had to kill
    // a stale listener, wait briefly so the port is actually free to rebind.
    bool killedStale = killProcessOnTcpPort(6800);
    QThread::msleep(killedStale ? 1200 : 50);

    int seedTime = DatabaseManager::instance().getSetting("seedTime", "30").toInt();
    QString seedStr = QString::number(seedTime == -1 ? 0 : (seedTime <= 0 ? -1 : seedTime));
    int maxConc = maxConcurrent;

    QStringList args;
    args << "--enable-rpc"
         << "--rpc-listen-all=false"
         << "--rpc-listen-port=6800"
         << "--rpc-secret=" + m_token
         << "--rpc-max-request-size=20M"
         << "--seed-time=" + seedStr
         << "--bt-detach-seed-only=true"
         << "--enable-dht=true"
         << "--dht-listen-port=6881-6999"
         << "--bt-enable-lpd=true"
         << "--enable-peer-exchange=true"
         << "--max-concurrent-downloads=" + QString::number(maxConc)
         << "--console-log-level=warn"
         << "--summary-interval=0"
         << "--file-allocation=none";

    QString defaultDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QDir().mkpath(defaultDir);
    args << "--dir=" + defaultDir;

    Logger::instance().info("Starting aria2c RPC daemon on port 6800");
    m_daemonProcess = new QProcess(this);
    connect(m_daemonProcess, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus) {
        Logger::instance().warning("aria2c daemon exited (code " + QString::number(exitCode) + ") stderr: " +
            (m_daemonProcess ? QString::fromUtf8(m_daemonProcess->readAllStandardError()).trimmed() : QString("")));
        if (m_daemonProcess) {
            m_daemonProcess->deleteLater();
            m_daemonProcess = nullptr;
        }
        m_daemonRunning = false;
        m_daemonStarting = false;
        emit daemonStateChanged(false);
    });
    m_daemonProcess->start(getAria2cPath(), args);
    return m_daemonProcess->waitForStarted(5000) || m_daemonProcess->state() == QProcess::Running;
}

bool Aria2cManager::ensureDaemon() {
    if (m_daemonRunning) {
        // Trust but verify: the cached "running" flag may be stale if the
        // daemon died, or the port may be held by an orphaned process instead.
        QJsonObject check = rpcCall("aria2.getVersion", QJsonArray() << ("token:" + m_token), 1500);
        if (!check.contains("__error") && check.contains("version")) return true;
        Logger::instance().warning("aria2c daemon unresponsive despite cached state; restarting");
        m_daemonRunning = false;
        shutdownDaemon();
    }
    if (m_daemonStarting) return false;

    m_daemonStarting = true;
    if (!startDaemonProcess()) {
        m_daemonStarting = false;
        emit errorOccurred("Failed to start aria2c daemon");
        return false;
    }

    // Wait for RPC availability
    int unauthorizedRestarts = 0;
    for (int i = 0; i < 90; i++) {
        // If the daemon process exited during startup (e.g. it failed to bind
        // the port before the stale holder was fully cleared), restart it once.
        if (!m_daemonProcess) {
            Logger::instance().warning("aria2c daemon exited during startup; restarting");
            if (!startDaemonProcess()) {
                m_daemonStarting = false;
                emit errorOccurred("Failed to restart aria2c daemon");
                return false;
            }
        }
        QJsonObject r = rpcCall("aria2.getVersion", QJsonArray() << ("token:" + m_token), 1500);
        if (!r.contains("__error") && r.contains("version")) {
            m_daemonRunning = true;
            m_daemonStarting = false;
            pollTimer->start();
            Logger::instance().info("aria2c RPC daemon ready");
            emit daemonStateChanged(true);
            return true;
        }
        // If the responder on port 6800 is a stale daemon holding a different
        // secret (reported as "Unauthorized"), kill everything on the port and
        // relaunch once. The loop would otherwise spin for the remaining
        // iterations against the wrong daemon and never recover, leaving every
        // torrent stuck at 0% ("Unauthorized" added to Downloads).
        if (m_rpcUnauthorized && unauthorizedRestarts < 2) {
            unauthorizedRestarts++;
            Logger::instance().warning("aria2c RPC Unauthorized; forcing daemon restart to clear stale secret holder");
            if (m_daemonProcess) {
                m_daemonProcess->kill();
                m_daemonProcess->waitForFinished(1000);
                m_daemonProcess->deleteLater();
                m_daemonProcess = nullptr;
            }
            killProcessOnTcpPort(6800);
            m_daemonRunning = false;
            if (!startDaemonProcess()) {
                m_daemonStarting = false;
                emit errorOccurred("Failed to restart aria2c daemon after Unauthorized");
                return false;
            }
            continue;
        }
        QThread::msleep(500);
        QCoreApplication::processEvents();
    }

    m_daemonStarting = false;
    if (m_daemonProcess) {
        m_daemonProcess->kill();
        m_daemonProcess->deleteLater();
        m_daemonProcess = nullptr;
    }
    emit errorOccurred("aria2c daemon did not respond");
    return false;
}

bool Aria2cManager::daemonRunning() const {
    return m_daemonRunning;
}

void Aria2cManager::shutdownDaemon() {
    if (m_daemonRunning) {
        rpcCall("aria2.shutdown", QJsonArray() << ("token:" + m_token), 1500);
    }
    if (m_daemonProcess) {
        m_daemonProcess->kill();
        m_daemonProcess->waitForFinished(1500);
        m_daemonProcess->deleteLater();
        m_daemonProcess = nullptr;
    }
    m_daemonRunning = false;
    m_daemonStarting = false;
    pollTimer->stop();
    emit daemonStateChanged(false);
}

QJsonValue Aria2cManager::rpcResult(const QString& method, const QJsonArray& params, int timeoutMs) {
    if (!isInstalled()) return QJsonValue(QJsonValue::Undefined);

    QNetworkRequest request(QUrl("http://127.0.0.1:6800/jsonrpc"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject req;
    req["jsonrpc"] = "2.0";
    req["id"] = QString::number(QRandomGenerator::global()->bounded(2000000000));
    req["method"] = method;
    req["params"] = params;

    QNetworkReply* reply = nam->post(request, QJsonDocument(req).toJson(QJsonDocument::Compact));

    QEventLoop loop;
    bool done = false;
    connect(reply, &QNetworkReply::finished, &loop, [&]() { done = true; loop.quit(); });

    if (timeoutMs > 0) {
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    }
    loop.exec();

    if (!done) {
        QObject::disconnect(reply, &QNetworkReply::finished, &loop, nullptr);
        reply->abort();
        reply->deleteLater();
        return QJsonValue(QJsonValue::Undefined);
    }

    QByteArray data = reply->readAll();
    reply->deleteLater();

    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        return QJsonValue(QJsonValue::Undefined);
    }

    QJsonObject obj = doc.object();
    if (obj.contains("error")) {
        QJsonObject errObj = obj.value("error").toObject();
        QString msg = errObj.value("message").toString();
        Logger::instance().error("aria2 RPC " + method + " error: " + msg);
        // Record whether the failure was a secret mismatch so ensureDaemon can
        // force a port clear + relaunch instead of looping forever against a
        // stale daemon that holds a different secret.
        m_rpcUnauthorized = msg.contains("Unauthorized", Qt::CaseInsensitive);
        return QJsonValue(QJsonValue::Undefined);
    }
    m_rpcUnauthorized = false;
    return obj.value("result");
}

QJsonObject Aria2cManager::rpcCall(const QString& method, const QJsonArray& params, int timeoutMs) {
    QJsonValue result = rpcResult(method, params, timeoutMs);
    if (result.isUndefined() || result.isNull()) {
        QJsonObject err;
        err["__error"] = "rpc error";
        return err;
    }
    if (result.isObject()) {
        return result.toObject();
    }
    // Non-object result (string gid, etc.): wrap for compatibility.
    QJsonObject wrapped;
    if (result.isString()) wrapped["gid"] = result.toString();
    return wrapped;
}

// ---------------------------------------------------------------------------
// Downloads (RPC backed)
// ---------------------------------------------------------------------------

QString Aria2cManager::seedTimeArg() const {
    int seedTime = DatabaseManager::instance().getSetting("seedTime", "30").toInt();
    return QString::number(seedTime == -1 ? 0 : (seedTime <= 0 ? -1 : seedTime));
}

static QString resolveCachedTorrent(const QString& magnetOrFile, QString* torrentName) {
    if (!magnetOrFile.startsWith("magnet:?")) return QString();
    QRegularExpression hashRegex("btih:([A-Fa-f0-9]{40})", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch hashMatch = hashRegex.match(magnetOrFile);
    if (!hashMatch.hasMatch()) return QString();
    QString tmpDir = QDir::tempPath() + "/copper_torrent_meta";
    QString saved = tmpDir + "/" + hashMatch.captured(1).toLower() + ".torrent";
    return QFile::exists(saved) ? saved : QString();
}

int Aria2cManager::addTorrent(const QString& magnetOrFile, const QString& savePath) {
    return addTorrentWithSelection(magnetOrFile, savePath, QVector<int>());
}

int Aria2cManager::addTorrentWithSelection(const QString& magnetOrFile, const QString& savePath, const QVector<int>& selectedIndices) {
    if (!isInstalled()) {
        Logger::instance().error("aria2c not installed");
        return -1;
    }
    if (!ensureDaemon()) {
        Logger::instance().error("aria2c daemon unavailable");
        return -1;
    }

    QDir().mkpath(savePath);
    int id = getNextId();

    Aria2cDownloadTask task;
    task.id = id;
    task.url = magnetOrFile;
    task.outputPath = savePath;
    task.process = nullptr;
    task.isRunning = true;
    task.isTorrent = true;

    QJsonObject options;
    options["dir"] = savePath;
    options["seed-time"] = seedTimeArg();

    QString torrentName;
    QString trackerStr = DatabaseManager::instance().getSetting("defaultTrackers", "");
    QStringList trackerList = trackerStr.isEmpty() ?
        QStringList() << "udp://tracker.opentrackr.org:1337/announce" << "udp://open.stealth.si:80/announce" << "udp://tracker.torrent.eu.org:451/announce"
        : trackerStr.split("\n", Qt::SkipEmptyParts);
    if (!trackerList.isEmpty()) {
        options["bt-tracker"] = trackerList.join(",");
        task.trackers = trackerList;
    }

    bool isMagnet = magnetOrFile.startsWith("magnet:?");
    QByteArray metainfo;

    if (isMagnet) {
        QString cached = resolveCachedTorrent(magnetOrFile, &torrentName);
        if (!cached.isEmpty()) {
            QFile f(cached);
            if (f.open(QIODevice::ReadOnly)) {
                metainfo = f.readAll();
            }
            Logger::instance().info("RPC addTorrent: using cached metadata " + cached);
        } else {
            Logger::instance().info("RPC addUri: bare magnet (fetching metadata via magnet)");
        }
    } else if (QFile::exists(magnetOrFile)) {
        QFile f(magnetOrFile);
        if (f.open(QIODevice::ReadOnly)) {
            metainfo = f.readAll();
        }
        torrentName = QFileInfo(magnetOrFile).completeBaseName();
    } else {
        Logger::instance().error("Torrent file not found: " + magnetOrFile);
        return -1;
    }

    if (!selectedIndices.isEmpty()) {
        QStringList zeroBased;
        for (int idx : selectedIndices) {
            zeroBased.append(QString::number(idx - 1));  // aria2 bt-select-file is 0-based
        }
        options["bt-select-file"] = zeroBased.join(",");
    }

    // aria2.addTorrent requires actual base64 bencoded metainfo; a bare magnet
    // link cannot be decoded as metainfo (aria2 rejects it with "Bencode
    // decoding failed"). For magnets without cached metainfo, use aria2.addUri,
    // which handles magnet links natively.
    bool isBareMagnet = isMagnet && metainfo.isEmpty();

    if (isBareMagnet) {
        QJsonArray params;
        params.append("token:" + m_token);
        params.append(QJsonArray() << magnetOrFile);
        params.append(options);

        QJsonObject res = rpcCall("aria2.addUri", params);
        if (res.contains("__error")) {
            Logger::instance().error("aria2.addUri failed: " + res.value("__error").toString());
            return -1;
        }

        task.trackers = trackerList;
        QString gid = res.value("gid").toString();
        if (gid.isEmpty()) {
            Logger::instance().error("aria2.addUri returned no gid");
            return -1;
        }
        task.gid = gid;
        tasks[id] = task;
        taskByGid[gid] = id;

        Logger::instance().info("aria2c RPC magnet started, id=" + QString::number(id) + ", gid=" + gid + ", save=" + savePath);
        return id;
    }

    QJsonArray params;
    params.append("token:" + m_token);
    params.append(QString::fromLatin1(metainfo.toBase64()));
    if (isMagnet) {
        params.append(QJsonArray() << magnetOrFile);
    } else {
        params.append(QJsonArray());
    }
    params.append(options);

    QJsonObject res = rpcCall("aria2.addTorrent", params);
    if (res.contains("__error")) {
        Logger::instance().error("aria2.addTorrent failed: " + res.value("__error").toString());
        return -1;
    }

    QString gid = res.value("gid").toString();
    if (gid.isEmpty()) {
        Logger::instance().error("aria2.addTorrent returned no gid");
        return -1;
    }

    task.gid = gid;
    if (!torrentName.isEmpty()) task.torrentName = torrentName;
    tasks[id] = task;
    taskByGid[gid] = id;

    Logger::instance().info("aria2c RPC torrent started, id=" + QString::number(id) + ", gid=" + gid + ", files=" + QString::number(selectedIndices.size()) + ", save=" + savePath);
    return id;
}

// ---------------------------------------------------------------------------
// Status polling
// ---------------------------------------------------------------------------

void Aria2cManager::poll() {
    if (tasks.isEmpty()) return;
    if (!m_daemonRunning) {
        // attempt to restart once
        if (!m_daemonStarting) {
            m_daemonStarting = true;
            bool ok = startDaemonProcess();
            m_daemonStarting = false;
            if (ok) m_daemonRunning = true;
        }
        return;
    }

    pollTick++;

    for (int id : tasks.keys()) {
        Aria2cDownloadTask& t = tasks[id];
        if (t.gid.isEmpty() || t.terminal) continue;

        QJsonArray fields;
        const QStringList keys = {
            "gid","status","totalLength","completedLength","downloadSpeed","uploadSpeed",
            "connections","numSeeders","seeder","uploadLength","infoHash","dir","errorMessage","announceList"
        };
        for (const QString& k : keys) fields.append(k);

        QJsonObject res = rpcCall("aria2.tellStatus", QJsonArray() << ("token:" + m_token) << t.gid << fields, 3000);
        if (res.contains("__error")) {
            Logger::instance().warning("tellStatus[" + QString::number(id) + "] error: " + res.value("__error").toString());
            continue;
        }

        parseTorrentStatus(id, res);

        if (pollTick % 3 == 0) {
            parsePeers(id);
        }
        if (!tasks[id].terminal) {
            emit torrentStateUpdated(id);
        }
    }
}

void Aria2cManager::parseTorrentStatus(int id, const QJsonObject& status) {
    if (!tasks.contains(id)) return;
    Aria2cDownloadTask& task = tasks[id];

    QString st = status.value("status").toString();
    task.totalBytes = status.value("totalLength").toString().toLongLong();
    task.downloadedBytes = status.value("completedLength").toString().toLongLong();
    task.speed = status.value("downloadSpeed").toString().toLongLong();
    task.uploadSpeed = status.value("uploadSpeed").toString().toLongLong();
    task.uploadedBytes = status.value("uploadLength").toString().toLongLong();
    task.connectedPeers = status.value("connections").toInt();
    task.seeds = status.value("numSeeders").toInt();
    task.infoHash = status.value("infoHash").toString();

    if (status.contains("announceList")) {
        QStringList trackerUrls;
        QJsonArray list = status.value("announceList").toArray();
        for (const QJsonValue& tierVal : list) {
            QJsonArray tier = tierVal.toArray();
            for (const QJsonValue& entry : tier) {
                QJsonArray pair = entry.toArray();
                if (pair.size() >= 2) {
                    trackerUrls.append(pair.at(0).toString());
                }
            }
        }
        if (!trackerUrls.isEmpty()) task.trackers = trackerUrls;
    }

    if (st == "complete" && !task.finishedEmitted) {
        task.finishedEmitted = true;
        task.isRunning = false;
        task.terminal = true;
        Logger::instance().info("Torrent download complete: id=" + QString::number(id) + ", gid=" + task.gid);
        emit downloadProgress(id, task.totalBytes, task.totalBytes, 0);
        emit downloadFinished(id);
    } else if (st == "error" && !task.failedEmitted) {
        task.failedEmitted = true;
        task.isRunning = false;
        task.terminal = true;
        QString err = status.value("errorMessage").toString();
        if (err.isEmpty()) err = "aria2 error";
        Logger::instance().error("Torrent download failed: id=" + QString::number(id) + " - " + err);
        emit downloadFailed(id, err);
    } else if (st == "removed") {
        task.terminal = true;
        task.isRunning = false;
        if (!task.finishedEmitted && !task.failedEmitted) {
            task.finishedEmitted = true;
        }
    }

    if (!task.terminal) {
        task.isRunning = (st == "active" || st == "waiting");
        emit downloadProgress(id, task.downloadedBytes, task.totalBytes, task.speed);
    }
}

void Aria2cManager::parsePeers(int id) {
    if (!tasks.contains(id)) return;
    Aria2cDownloadTask& task = tasks[id];
    if (task.gid.isEmpty()) return;

    QJsonObject res = rpcCall("aria2.getPeers", QJsonArray() << ("token:" + m_token) << task.gid, 3000);
    if (res.contains("__error")) return;

    QJsonArray peersArr = res.value("peers").toArray();
    QVector<PeerInfo> peers;
    int leecherCount = 0;
    for (const QJsonValue& val : peersArr) {
        QJsonObject p = val.toObject();
        PeerInfo pi;
        pi.ip = p.value("ip").toString();
        pi.port = p.value("port").toInt();
        pi.seeder = p.value("seeder").toString() == "true";
        pi.amChoking = p.value("amChoking").toString() == "true";
        pi.peerChoking = p.value("peerChoking").toString() == "true";
        pi.downloadSpeed = p.value("downloadSpeed").toString().toLongLong();
        pi.uploadSpeed = p.value("uploadSpeed").toString().toLongLong();
        pi.peerId = p.value("peerId").toString();
        pi.connectedVia = p.value("connectedVia").toString();
        if (!pi.seeder) leecherCount++;
        peers.append(pi);
    }
    task.peers = peers;
    task.leechers = leecherCount;
    if (task.seeds <= 0) {
        int seedCount = 0;
        for (const PeerInfo& p : peers) if (p.seeder) seedCount++;
        task.seeds = seedCount;
    }
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

void Aria2cManager::pauseDownload(int id) {
    if (!tasks.contains(id)) return;
    if (tasks[id].gid.isEmpty()) return;
    rpcCall("aria2.pause", QJsonArray() << ("token:" + m_token) << tasks[id].gid);
    tasks[id].isRunning = false;
    Logger::instance().info("Torrent paused: id=" + QString::number(id));
}

void Aria2cManager::resumeDownload(int id) {
    if (!tasks.contains(id)) return;
    if (tasks[id].gid.isEmpty()) return;
    rpcCall("aria2.unpause", QJsonArray() << ("token:" + m_token) << tasks[id].gid);
    tasks[id].isRunning = true;
    Logger::instance().info("Torrent resumed: id=" + QString::number(id));
}

void Aria2cManager::removeDownload(int id) {
    if (!tasks.contains(id)) return;
    if (!tasks[id].gid.isEmpty()) {
        rpcCall("aria2.forceRemove", QJsonArray() << ("token:" + m_token) << tasks[id].gid);
        taskByGid.remove(tasks[id].gid);
    }
    tasks.remove(id);
    Logger::instance().info("Torrent removed: id=" + QString::number(id));
}

bool Aria2cManager::isRunning(int id) const {
    return tasks.contains(id) && tasks[id].isRunning;
}

int Aria2cManager::getConnectedPeers(int id) const {
    return tasks.contains(id) ? tasks[id].connectedPeers : 0;
}

int Aria2cManager::getLeechers(int id) const {
    return tasks.contains(id) ? tasks[id].leechers : 0;
}

int Aria2cManager::getSeeds(int id) const {
    return tasks.contains(id) ? tasks[id].seeds : 0;
}

qint64 Aria2cManager::getUploadSpeed(int id) const {
    return tasks.contains(id) ? tasks[id].uploadSpeed : 0;
}

qint64 Aria2cManager::getUploadedBytes(int id) const {
    return tasks.contains(id) ? tasks[id].uploadedBytes : 0;
}

qint64 Aria2cManager::getTotalBytes(int id) const {
    return tasks.contains(id) ? tasks[id].totalBytes : 0;
}

qint64 Aria2cManager::getDownloadedBytes(int id) const {
    return tasks.contains(id) ? tasks[id].downloadedBytes : 0;
}

qint64 Aria2cManager::getSpeed(int id) const {
    return tasks.contains(id) ? tasks[id].speed : 0;
}

QString Aria2cManager::getInfoHash(int id) const {
    return tasks.contains(id) ? tasks[id].infoHash : QString();
}

QVector<PeerInfo> Aria2cManager::getPeers(int id) const {
    return tasks.contains(id) ? tasks[id].peers : QVector<PeerInfo>();
}

QStringList Aria2cManager::getTrackers(int id) const {
    return tasks.contains(id) ? tasks[id].trackers : QStringList();
}

QStringList Aria2cManager::getTrackerList(int id) const {
    return tasks.contains(id) ? tasks[id].trackers : QStringList();
}

QString Aria2cManager::getGid(int id) const {
    return tasks.contains(id) ? tasks[id].gid : QString();
}

void Aria2cManager::addTrackers(int torrentId, const QStringList& trackers) {
    for (const QString& t : trackers) {
        addTrackerToTorrent(torrentId, t);
    }
}

void Aria2cManager::addTrackerToTorrent(int torrentId, const QString& tracker) {
    if (!tasks.contains(torrentId) || tasks[torrentId].gid.isEmpty()) return;
    if (tracker.trimmed().isEmpty()) return;
    QJsonObject res = rpcCall("aria2.changeUri", QJsonArray()
        << ("token:" + m_token) << tasks[torrentId].gid
        << QJsonArray() << (QJsonArray() << tracker.trimmed()));
    if (!res.contains("__error")) {
        Logger::instance().info("Tracker added to torrent " + QString::number(torrentId) + ": " + tracker.trimmed());
    } else {
        Logger::instance().error("Failed to add tracker: " + res.value("__error").toString());
    }
}

void Aria2cManager::removeTrackerFromTorrent(int torrentId, const QString& tracker) {
    if (!tasks.contains(torrentId) || tasks[torrentId].gid.isEmpty()) return;
    QStringList cur = tasks[torrentId].trackers;
    for (int i = 0; i < cur.size(); i++) {
        if (cur[i] == tracker) {
            QJsonArray delUris;
            delUris.append(tracker);
            QJsonArray positions;
            positions.append(i);
            rpcCall("aria2.changeUri", QJsonArray()
                << ("token:" + m_token) << tasks[torrentId].gid
                << delUris << QJsonArray() << positions);
            Logger::instance().info("Tracker removed from torrent " + QString::number(torrentId) + ": " + tracker);
            return;
        }
    }
}

void Aria2cManager::seedTorrent(int torrentId, int seedTimeMinutes) {
    if (!tasks.contains(torrentId) || tasks[torrentId].gid.isEmpty()) return;
    QString value = QString::number(seedTimeMinutes == -1 ? 0 : (seedTimeMinutes <= 0 ? -1 : seedTimeMinutes));
    rpcCall("aria2.changeOption", QJsonArray() << ("token:" + m_token) << tasks[torrentId].gid << QJsonObject{{"seed-time", value}});
    Logger::instance().info("Torrent seed-time set: id=" + QString::number(torrentId) + " -> " + value);
}

void Aria2cManager::cancelSeeding(int torrentId) {
    if (!tasks.contains(torrentId) || tasks[torrentId].gid.isEmpty()) return;
    rpcCall("aria2.forcePause", QJsonArray() << ("token:" + m_token) << tasks[torrentId].gid);
    Logger::instance().info("Torrent seeding cancelled: id=" + QString::number(torrentId));
}

int Aria2cManager::getNextId() {
    return nextId++;
}

// ---------------------------------------------------------------------------
// File list fetch (kept one-shot, used by dialog)
// ---------------------------------------------------------------------------

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
    bool isLocalTorrent = QFile::exists(magnetOrFile) && magnetOrFile.endsWith(".torrent", Qt::CaseInsensitive);
    QUrl u(magnetOrFile);
    bool isRemoteTorrent = !isMagnet
        && (u.scheme() == "http" || u.scheme() == "https" || u.scheme() == "ftp")
        && u.path().endsWith(".torrent", Qt::CaseInsensitive);

    if (isMagnet) {
        fetchMagnetMetadata(magnetOrFile, callback);
    } else if (isLocalTorrent) {
        fetchTorrentFileList(magnetOrFile, callback);
    } else if (isRemoteTorrent) {
        fetchRemoteTorrentFile(magnetOrFile, callback);
    } else {
        Logger::instance().error("Invalid torrent source: " + magnetOrFile);
        callback(QVector<PlaylistEntry>(), TorrentInfo());
    }
}

void Aria2cManager::fetchRemoteTorrentFile(const QString& url, std::function<void(const QVector<PlaylistEntry>&, const TorrentInfo&)> callback) {
    downloadTorrentFile(url, [this, callback, url](const QString& localPath, const QString& err) {
        if (localPath.isEmpty()) {
            emit errorOccurred(err.isEmpty() ? ("Failed to download .torrent from " + url) : err);
            callback(QVector<PlaylistEntry>(), TorrentInfo());
            return;
        }
        fetchTorrentFileList(localPath, callback);
    });
}

void Aria2cManager::resolveTorrentSource(const QString& url, std::function<void(const QString& localPath, const QString& error)> callback) {
    if (url.startsWith("magnet:?")) {
        callback(QString(), QString());
        return;
    }
    QUrl u(url);
    bool isRemote = (u.scheme() == "http" || u.scheme() == "https" || u.scheme() == "ftp")
                    && u.path().endsWith(".torrent", Qt::CaseInsensitive);
    if (!isRemote) {
        callback(url, QString());  // local torrent file path
        return;
    }
    downloadTorrentFile(url, callback);
}

void Aria2cManager::downloadTorrentFile(const QString& url, std::function<void(const QString& localPath, const QString& error)> callback) {
    QString tmpDir = QDir::tempPath() + "/copper_torrent_meta";
    QDir().mkpath(tmpDir);

    QFileInfo fi(QUrl(url).path());
    QString fileName = fi.fileName();
    if (fileName.isEmpty()) fileName = "remote.torrent";
    QString dest = tmpDir + "/" + fileName;

    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("User-Agent", DatabaseManager::instance().getUserAgent().toUtf8());
    QNetworkReply* reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, callback, dest, url]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            callback(QString(), "Failed to download .torrent from " + url + ": " + reply->errorString());
            return;
        }
        QByteArray data = reply->readAll();
        if (data.isEmpty()) {
            callback(QString(), "Empty .torrent received from " + url);
            return;
        }
        QFile f(dest);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            callback(QString(), "Could not write downloaded .torrent to " + dest);
            return;
        }
        f.write(data);
        f.close();
        Logger::instance().info("Downloaded .torrent from URL to " + dest +
                                " (" + QString::number(data.size()) + " bytes)");
        callback(dest, QString());
    });
}

void Aria2cManager::fetchMagnetMetadata(const QString& magnet, std::function<void(const QVector<PlaylistEntry>&, const TorrentInfo&)> callback, int attempt) {
    QString tmpDir = QDir::tempPath() + "/copper_torrent_meta";
    QDir().mkpath(tmpDir);

    QProcess* process = new QProcess(this);
    QStringList args;
    args << "--bt-metadata-only=true";
    args << "--bt-save-metadata=true";
    args << "--bt-stop-timeout=60";
    args << "--seed-time=0";
    args << "--summary-interval=0";
    args << "--dir=" + tmpDir;
    args << magnet;

    Logger::instance().info("Fetching magnet metadata...");

    connect(process, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
        [this, process, callback, tmpDir, magnet, attempt](int exitCode, QProcess::ExitStatus) {
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

        if (exitCode != 0 && attempt < 1) {
            // retry once
            fetchMagnetMetadata(magnet, callback, attempt + 1);
            return;
        }
        if (exitCode != 0) {
            QString err;
            QRegularExpression errRegex("errorCode[^\\n]*|Exception[^\\n]*");
            QRegularExpressionMatch m = errRegex.match(data);
            if (m.hasMatch()) err = m.captured(0).trimmed();
            if (err.isEmpty()) err = "Failed to fetch magnet metadata (exit " + QString::number(exitCode) + ")";
            Logger::instance().error(err);
            emit errorOccurred(err);
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
            QString err = "aria2c did not produce metadata for the magnet link";
            Logger::instance().error(err);
            emit errorOccurred(err);
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
    QProcess* process = new QProcess(this);
    QStringList args;
    args << "--show-files";
    args << "--dry-run";
    args << "--seed-time=0";
    args << "--summary-interval=0";
    args << torrentPath;

    connect(process, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
        [this, process, callback](int exitCode, QProcess::ExitStatus) {
        QByteArray output = process->readAllStandardOutput();
        QByteArray errOutput = process->readAllStandardError();
        QByteArray allOutput = output + errOutput;
        process->deleteLater();

        QString data = QString::fromUtf8(allOutput);
        QStringList lines = data.split("\n");

        if (exitCode != 0) {
            QString errLine;
            for (const QString& l : lines) {
                QString t = l.trimmed();
                if (t.startsWith("Exception:") || t.startsWith("errorCode:") || t.startsWith("Failed to open")) {
                    errLine = t;
                    break;
                }
            }
            if (errLine.isEmpty()) errLine = "aria2c --show-files exited with code " + QString::number(exitCode);
            Logger::instance().error("Torrent file list parse failed: " + errLine);
            emit errorOccurred("Failed to parse torrent: " + errLine);
            callback(QVector<PlaylistEntry>(), TorrentInfo());
            return;
        }

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

        info.fileCount = entries.size();
        Logger::instance().info("Parsed " + QString::number(entries.size()) + " file(s) from torrent, name=" + info.name);
        callback(entries, info);
    });

    process->start(getAria2cPath(), args);
}
