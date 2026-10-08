#include "core/LocalServer.h"
#include "core/DownloadManager.h"
#include "utils/ApiToken.h"
#include "utils/Logger.h"
#include "utils/FileNameSanitizer.h"
#include "utils/UrlDetector.h"
#include "utils/Aria2cManager.h"
#include "utils/FfmpegManager.h"
#include "utils/YtDlpManager.h"
#include "utils/UpdateManager.h"
#include "db/DatabaseManager.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QPointer>

namespace {
// Local API request caps. A misbehaving or hostile local client must not be
// able to make the server buffer unbounded data before it gets a refusal:
// the request head is bounded (headers), JSON bodies are bounded (413).
constexpr int kMaxHeaderBytes = 64 * 1024;
constexpr qint64 kMaxBodyBytes = 1024 * 1024;  // real bodies are a few KB

QByteArray reasonPhrase(int statusCode) {
    switch (statusCode) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        default: return "Error";
    }
}
}  // namespace

LocalServer::LocalServer() : server(new QTcpServer(this)), serverPort(24680) {}

LocalServer& LocalServer::instance() {
    static LocalServer instance;
    return instance;
}

bool LocalServer::start(int port) {
    serverPort = port;

    if (server->isListening()) {
        server->close();
    }

    // Mint/persist the local API token BEFORE the port opens: a second app
    // instance (or the native host) authenticates by reading the token from
    // the shared config, and it must exist by the time anyone can connect.
    ApiToken::token();

    if (!server->listen(QHostAddress::LocalHost, port)) {
        Logger::instance().error("LocalServer: Failed to start on port " + QString::number(port) + ": " + server->errorString());
        return false;
    }

    Logger::instance().info("LocalServer: Started on http://localhost:" + QString::number(port));

    connect(server, &QTcpServer::newConnection, this, [this]() {
        while (server->hasPendingConnections()) {
            QTcpSocket* socket = server->nextPendingConnection();
            handleConnection(socket);
        }
    });

    return true;
}

void LocalServer::stop() {
    if (server->isListening()) {
        server->close();
        Logger::instance().info("LocalServer: Stopped");
    }
}

bool LocalServer::isRunning() const {
    return server->isListening();
}

int LocalServer::getPort() const {
    return serverPort;
}

void LocalServer::handleConnection(QTcpSocket* socket) {
    // The client can send headers and body in separate TCP packets, so we must
    // buffer the request until the complete body (per Content-Length) has
    // arrived before handing it to the handler. Without this, a POST whose body
    // arrives in a later packet is seen with an empty body and rejected as
    // "Invalid JSON".
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        ConnState& state = m_conns[socket];

        if (state.processed) {
            // After an early 413, keep draining the refused body instead of
            // stopping the read: closing while the client is still sending
            // makes Windows reset the connection and the client would never
            // see the status line it is waiting for.
            if (state.discardRemaining > 0) {
                state.discardRemaining -= socket->readAll().size();
                if (state.discardRemaining <= 0 && socket->state() == QAbstractSocket::ConnectedState) {
                    socket->disconnectFromHost();
                }
            }
            return;
        }

        state.buffer += socket->readAll();

        if (!state.buffer.contains("\r\n\r\n")) {
            // Header-accumulation cap: a client that never terminates its
            // headers must not be able to grow this buffer without bound.
            if (state.buffer.size() > kMaxHeaderBytes) {
                state.processed = true;
                sendJsonResponse(QPointer<QTcpSocket>(socket), 431,
                                 {{"error", "Request header fields too large"}});
            }
            return;
        }

        int headerEnd = state.buffer.indexOf("\r\n\r\n");
        if (headerEnd > kMaxHeaderBytes) {
            // Same cap on the completed head: a valid-but-huge header block is
            // refused before any of it is parsed.
            state.processed = true;
            sendJsonResponse(QPointer<QTcpSocket>(socket), 431,
                             {{"error", "Request header fields too large"}});
            return;
        }
        QByteArray header = state.buffer.left(headerEnd);

        qint64 contentLength = -1;
        QString origin;
        QString apiToken;
        const QStringList lines = QString::fromLatin1(header).split("\r\n");
        for (const QString& line : lines) {
            if (line.startsWith("Content-Length:", Qt::CaseInsensitive)) {
                bool ok = false;
                contentLength = line.mid(15).trimmed().toLongLong(&ok);
                if (!ok) contentLength = -1;
            } else if (line.startsWith("Origin:", Qt::CaseInsensitive)) {
                origin = line.mid(7).trimmed();
            } else if (line.startsWith("X-Copper-Token:", Qt::CaseInsensitive)) {
                apiToken = line.mid(16).trimmed();
            }
        }

        const qint64 bodyAvailable = state.buffer.size() - headerEnd - 4;

        // Body cap: refuse BEFORE buffering an oversized payload, then keep
        // reading and dropping what the client already committed to sending so
        // the 413 is actually delivered instead of a connection reset.
        if (contentLength > kMaxBodyBytes) {
            state.processed = true;
            state.discardRemaining = contentLength - bodyAvailable;
            if (state.discardRemaining < 0) state.discardRemaining = 0;
            sendJsonResponse(QPointer<QTcpSocket>(socket), 413,
                             {{"error", "Request body too large"}},
                             QString(), state.discardRemaining == 0);
            return;
        }

        if (bodyAvailable < contentLength) return;  // wait for the rest

        state.processed = true;

        if (lines.isEmpty()) {
            socket->disconnectFromHost();
            return;
        }

        const QStringList requestLine = lines[0].split(" ");
        if (requestLine.size() < 2) {
            socket->disconnectFromHost();
            return;
        }

        const QString method = requestLine[0];
        const QString path = requestLine[1];

        QByteArray body = state.buffer.mid(headerEnd + 4);
        if (contentLength >= 0) body = body.left(contentLength);

        handleRequest(socket, method, path, body, origin, apiToken);
    });

    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
        m_conns.remove(socket);
        socket->deleteLater();
    });
}

bool LocalServer::isAllowedOrigin(const QString& origin) const {
    if (origin.isEmpty()) return true;  // non-browser local client (curl, native tests)
    // Exact allowlist, not a prefix match: the Origin must be a syntactically
    // valid browser-extension origin. startsWith() accepted any string that
    // merely began with the scheme ("chrome-extension://anything"), which is
    // exactly the kind of loose check that later gets relied on for more than
    // it can deliver. Chromium/Edge extension IDs are 32 characters in a-p
    // (hex letters mapped from the key hash); Firefox's are UUIDs.
    static const QRegularExpression chromeExtensionRe(
        QStringLiteral("^chrome-extension://[a-p]{32}$"));
    static const QRegularExpression mozExtensionRe(QStringLiteral(
        "^moz-extension://[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}"
        "-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return chromeExtensionRe.match(origin).hasMatch() ||
           mozExtensionRe.match(origin).hasMatch();
}

void LocalServer::handleRequest(QTcpSocket* socket, const QString& method, const QString& path, const QByteArray& body,
                                const QString& origin, const QString& apiToken) {
    Logger::instance().info("LocalServer: " + method + " " + path);

    // Take the guard while the socket is known alive (this handler only runs
    // from the socket's own readyRead). Emitted signals below can open modal UI
    // whose nested event loop lets this client disconnect and its socket be
    // deleted; sendJsonResponse then checks the guard instead of crashing.
    const QPointer<QTcpSocket> guard(socket);

    if (!isAllowedOrigin(origin)) {
        Logger::instance().warning("LocalServer: rejected request from disallowed origin: " + origin);
        sendJsonResponse(guard, 403, {{"error", "Forbidden"}});
        return;
    }

    QString allowedOrigin = origin.isEmpty() ? QString() : origin;

    if (method == "OPTIONS") {
        QByteArray response;
        response += "HTTP/1.1 204 No Content\r\n";
        if (!allowedOrigin.isEmpty()) {
            response += "Access-Control-Allow-Origin: " + allowedOrigin.toUtf8() + "\r\n";
        }
        response += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
        response += "Access-Control-Allow-Headers: Content-Type, X-Copper-Token\r\n";
        response += "Access-Control-Max-Age: 3600\r\n";
        response += "Connection: close\r\n";
        response += "\r\n";
        socket->write(response);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    // Local API token (0.4.0): every non-OPTIONS request must prove it was
    // made by a Copper component that could read the shared config file. A web
    // page cannot read it (same-origin policy), and a preflighted cross-origin
    // request cannot smuggle this header without the Allow-Headers above - so
    // this, together with the exact Origin allowlist, is what turns the local
    // API from "anything on the machine" into "only Copper itself".
    if (!ApiToken::matches(apiToken)) {
        Logger::instance().warning("LocalServer: rejected request without a valid API token (" +
                                   method + " " + path + ", origin: " +
                                   (origin.isEmpty() ? QStringLiteral("<none>") : origin) + ")");
        sendJsonResponse(guard, 401, {{"error", "Unauthorized"}}, allowedOrigin);
        return;
    }

    if (path == "/api/ping" && method == "GET") {
        QJsonObject json;
        json["status"] = "ok";
        json["version"] = QCoreApplication::applicationVersion();
        sendJsonResponse(guard, 200, json, allowedOrigin);
        return;
    }

    if (path == "/api/version" && method == "GET") {
        QJsonObject json;
        json["name"] = "Copper Download Manager";
        json["version"] = QCoreApplication::applicationVersion();
        json["status"] = "running";
        sendJsonResponse(guard, 200, json, allowedOrigin);
        return;
    }

    if (path == "/api/download-filters" && method == "GET") {
        QJsonObject json;
        json["enabled"] = DownloadManager::instance().fileFormatFilterEnabled();
        QJsonArray inc, exc;
        for (const QString& s : DownloadManager::instance().includeFileFormats()) inc.append(s);
        for (const QString& s : DownloadManager::instance().excludeFileFormats()) exc.append(s);
        json["include"] = inc;
        json["exclude"] = exc;
        sendJsonResponse(guard, 200, json, allowedOrigin);
        return;
    }

    if (path == "/api/download" && method == "POST") {
        QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            sendJsonResponse(guard, 400, {{"error", "Invalid JSON"}}, allowedOrigin);
            return;
        }

        QJsonObject obj = doc.object();
        QString url = obj["url"].toString();
        QString filename = obj["filename"].toString();
        QString savePath = obj["path"].toString();
        QString format = obj["format"].toString();

        if (url.isEmpty()) {
            sendJsonResponse(guard, 400, {{"error", "URL is required"}}, allowedOrigin);
            return;
        }
        if (!url.startsWith("http") && !url.startsWith("ftp") && !url.startsWith("magnet:?")) {
            sendJsonResponse(guard, 400, {{"error", "Unsupported URL scheme"}}, allowedOrigin);
            return;
        }

        int id;
        if (url.startsWith("magnet:?")) {
            id = DownloadManager::instance().addDownload(url, savePath, "Torrent");
        } else {
            QString fullSavePath = savePath;
            if (!filename.isEmpty() && !savePath.isEmpty()) {
                if (!savePath.endsWith('/') && !savePath.endsWith('\\')) {
                    fullSavePath = savePath + "/" + sanitizeFileName(filename);
                } else {
                    fullSavePath = savePath + sanitizeFileName(filename);
                }
            }
            // Route video/playlist sites through yt-dlp (honoring the requested
            // mp4/mp3/playlist format); everything else uses the HTTP engine.
            bool wantMp3 = format.compare("mp3", Qt::CaseInsensitive) == 0;
            bool wantPlaylistMp3 = format.compare("playlist-mp3", Qt::CaseInsensitive) == 0;
            if (wantMp3 || format.startsWith("playlist-", Qt::CaseInsensitive) ||
                UrlDetector::isYtDlpUrl(url) || UrlDetector::isPlaylistUrl(url)) {
                id = DownloadManager::instance().addDownload(url, fullSavePath, "YtDlp", 16,
                    (wantMp3 || wantPlaylistMp3) ? "mp3" : "mp4");
            } else {
                id = DownloadManager::instance().addDownload(url, fullSavePath, "HTTP");
            }
        }

        if (id < 0) {
            sendJsonResponse(guard, 400, {{"error", "Blocked by download file-format filter"}}, allowedOrigin);
            return;
        }

        QJsonObject response;
        response["success"] = true;
        response["id"] = id;
        response["message"] = "Download added successfully";
        // Respond BEFORE emitting: argumentForwarded can open modal UI (a
        // nested event loop) during which this client may disconnect and its
        // socket be destroyed - the reply must be written while it is alive.
        sendJsonResponse(guard, 200, response, allowedOrigin);

        emit argumentForwarded("show");
        emit downloadRequested(url, filename, savePath);
        return;
    }

    if (path == "/api/torrent" && method == "POST") {
        QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            sendJsonResponse(guard, 400, {{"error", "Invalid JSON"}}, allowedOrigin);
            return;
        }

        QJsonObject obj = doc.object();
        QString url = obj["url"].toString();
        QString savePath = obj["path"].toString();

        if (url.isEmpty()) {
            sendJsonResponse(guard, 400, {{"error", "URL is required"}}, allowedOrigin);
            return;
        }
        if (!url.startsWith("http") && !url.startsWith("magnet:?")) {
            sendJsonResponse(guard, 400, {{"error", "Unsupported URL scheme"}}, allowedOrigin);
            return;
        }

        if (savePath.isEmpty()) {
            savePath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        }

        int id = DownloadManager::instance().addDownload(url, savePath, "Torrent");

        QJsonObject response;
        response["success"] = true;
        response["id"] = id;
        response["message"] = "Torrent download added";
        sendJsonResponse(guard, 200, response, allowedOrigin);
        return;
    }

    if (path == "/api/forward" && method == "POST") {
        QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            sendJsonResponse(guard, 400, {{"error", "Invalid JSON"}}, allowedOrigin);
            return;
        }

        QJsonObject obj = doc.object();
        QString argument = obj["argument"].toString();

        if (argument.isEmpty()) {
            sendJsonResponse(guard, 400, {{"error", "Argument is required"}}, allowedOrigin);
            return;
        }

        Logger::instance().info("Forward received: " + argument.left(100));

        // Respond BEFORE emitting argumentForwarded: its slot (e.g. the torrent
        // file picker) runs a modal nested event loop for as long as it takes,
        // during which this client may disconnect and its socket be destroyed.
        // Sending first guarantees the reply is written while the socket is
        // still alive - replying afterwards was a use-after-free crash.
        QJsonObject response;
        response["success"] = true;
        response["message"] = "Argument forwarded";
        sendJsonResponse(guard, 200, response, allowedOrigin);

        emit argumentForwarded(argument);
        return;
    }

    if (path == "/api/downloads" && method == "GET") {
        QJsonArray downloadsArray;
        QVector<DownloadItem> downloads = DownloadManager::instance().getDownloads();

        for (const DownloadItem& item : downloads) {
            QJsonObject dlObj;
            dlObj["id"] = item.id;
            dlObj["url"] = item.url;
            // Where the row writes. For a folder job this is the directory its
            // files land in - the only way a client can tell where to look for
            // them, since the folder is named after the playlist.
            dlObj["filePath"] = item.filePath;
            dlObj["fileName"] = item.fileName;
            dlObj["status"] = item.status;
            dlObj["progress"] = item.progress;
            dlObj["downloadedSize"] = item.downloadedSize;
            dlObj["totalSize"] = item.totalSize;
            dlObj["speed"] = item.speed;
            dlObj["type"] = item.type;
            // A playlist/torrent folder owns its items, so consumers need the link
            // and the folder flag to tell "one job with N items" from N transfers.
            dlObj["parentId"] = item.parentId;
            dlObj["isFolder"] = item.isFolder;
            dlObj["attempts"] = item.attempts;
            dlObj["error"] = item.error;
            // Playlist attribution: which site video a row mirrors and where that
            // video sits in its playlist. Both are what let a job tell its items
            // apart when yt-dlp only reports progress for one at a time.
            dlObj["videoId"] = item.videoId;
            dlObj["trackIndex"] = item.trackIndex;
            // What the job was asked to fetch (the positions the user ticked) and
            // whether files get a track-number prefix. This is the contract for
            // "download exactly what was selected", so it is asserted from outside.
            {
                QJsonArray positions;
                for (int position : item.selectedIndices) positions.append(position);
                dlObj["selectedPositions"] = positions;
            }
            dlObj["trackNumbers"] = item.trackNumbers;
            // The job that really controls this row, or -1 when the row is its own
            // transfer. A playlist item reports its job here, which is what the UI
            // uses to label the controls and what tells a client that pausing this
            // row pauses the whole playlist.
            dlObj["controlledByJob"] = DownloadManager::instance().controllingJobId(item.id);
            downloadsArray.append(dlObj);
        }

        QJsonObject response;
        response["downloads"] = downloadsArray;
        response["count"] = downloads.size();
        sendJsonResponse(guard, 200, response, allowedOrigin);
        return;
    }

    // One-shot environment report for support/diagnostics: versions, tool
    // state, daemon state and paths - enough to triage a bug report without
    // asking the user to reproduce anything first.
    if (path == "/api/diagnostics" && method == "GET") {
        QJsonObject json;
        json["app"] = "Copper Download Manager";
        json["version"] = QCoreApplication::applicationVersion();
        json["schemaVersion"] = DatabaseManager::instance().getSchemaVersion();
        json["profile"] = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        json["toolsDir"] = Aria2cManager::instance().toolsDir();
        json["aria2Installed"] = Aria2cManager::instance().isInstalled();
        json["aria2DaemonRunning"] = Aria2cManager::instance().daemonRunning();
        json["ffmpegInstalled"] = FfmpegManager::instance().isInstalled();
        json["ffmpegVersion"] = FfmpegManager::instance().getVersion();
        json["ytDlpInstalled"] = YtDlpManager::instance().isInstalled();
        json["ytDlpVersion"] = YtDlpManager::instance().getVersion();
        json["updateAvailable"] = UpdateManager::instance().isUpdateAvailable();
        json["updateLatestVersion"] = UpdateManager::instance().getLatestVersion();
        sendJsonResponse(guard, 200, json, allowedOrigin);
        return;
    }

    sendJsonResponse(guard, 404, {{"error", "Not found"}}, allowedOrigin);
}

void LocalServer::sendJsonResponse(QPointer<QTcpSocket> socket, int statusCode, const QJsonObject& json,
                                   const QString& origin, bool closeAfter) {
    if (!socket) {
        // The client went away while the handler was suspended (modal event
        // loop inside an emitted signal); dropping the reply is the safe path.
        Logger::instance().info("Response dropped: client closed the connection before the reply was sent");
        return;
    }
    if (socket->state() != QAbstractSocket::ConnectedState) return;
    QByteArray body = QJsonDocument(json).toJson(QJsonDocument::Compact);
    QByteArray response;
    response += "HTTP/1.1 " + QByteArray::number(statusCode) + " " + reasonPhrase(statusCode) + "\r\n";
    response += "Content-Type: application/json\r\n";
    if (!origin.isEmpty()) {
        response += "Access-Control-Allow-Origin: " + origin.toUtf8() + "\r\n";
    }
    response += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    response += "Access-Control-Allow-Headers: Content-Type, X-Copper-Token\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";
    response += body;

    socket->write(response);
    socket->flush();
    if (closeAfter) socket->disconnectFromHost();
}

void LocalServer::sendHtmlResponse(QPointer<QTcpSocket> socket, int statusCode, const QString& html) {
    if (!socket) return;
    if (socket->state() != QAbstractSocket::ConnectedState) return;
    QByteArray body = html.toUtf8();
    QByteArray response;
    response += "HTTP/1.1 " + QByteArray::number(statusCode) + " OK\r\n";
    response += "Content-Type: text/html\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";
    response += body;

    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
}
