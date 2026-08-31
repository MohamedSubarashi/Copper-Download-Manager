#include "core/LocalServer.h"
#include "core/DownloadManager.h"
#include "utils/Logger.h"
#include "utils/FileNameSanitizer.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCoreApplication>
#include <QStandardPaths>

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
        state.buffer += socket->readAll();

        if (state.processed) return;
        if (!state.buffer.contains("\r\n\r\n")) return;

        int headerEnd = state.buffer.indexOf("\r\n\r\n");
        QByteArray header = state.buffer.left(headerEnd);

        qint64 contentLength = -1;
        QString headerText = QString::fromLatin1(header);
        for (const QString& line : headerText.split("\r\n")) {
            if (line.startsWith("Content-Length:", Qt::CaseInsensitive)) {
                bool ok = false;
                contentLength = line.mid(15).trimmed().toLongLong(&ok);
                if (!ok) contentLength = -1;
                break;
            }
        }

        qint64 bodyAvailable = state.buffer.size() - headerEnd - 4;
        if (bodyAvailable < contentLength) return;

        state.processed = true;

        QString request = headerText;
        QStringList lines = headerText.split("\r\n");
        if (lines.isEmpty()) {
            socket->disconnectFromHost();
            return;
        }

        QStringList requestLine = lines[0].split(" ");
        if (requestLine.size() < 2) {
            socket->disconnectFromHost();
            return;
        }

        QString method = requestLine[0];
        QString path = requestLine[1];

        QString origin;
        for (int i = 1; i < lines.size(); i++) {
            if (lines[i].startsWith("Origin:", Qt::CaseInsensitive)) {
                origin = lines[i].mid(7).trimmed();
                break;
            }
        }

        QByteArray body;
        if (headerEnd >= 0) {
            body = state.buffer.mid(headerEnd + 4);
            if (contentLength >= 0) {
                body = body.left(contentLength);
            }
        }

        handleRequest(socket, method, path, body, origin);
    });

    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
        m_conns.remove(socket);
        socket->deleteLater();
    });
}

bool LocalServer::isAllowedOrigin(const QString& origin) const {
    if (origin.isEmpty()) return true;  // non-browser local client (curl, native tests)
    return origin.startsWith("chrome-extension://") || origin.startsWith("moz-extension://");
}

void LocalServer::handleRequest(QTcpSocket* socket, const QString& method, const QString& path, const QByteArray& body, const QString& origin) {
    Logger::instance().info("LocalServer: " + method + " " + path);

    if (!isAllowedOrigin(origin)) {
        Logger::instance().warning("LocalServer: rejected request from disallowed origin: " + origin);
        sendJsonResponse(socket, 403, {{"error", "Forbidden"}});
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
        response += "Access-Control-Allow-Headers: Content-Type\r\n";
        response += "Access-Control-Max-Age: 3600\r\n";
        response += "Connection: close\r\n";
        response += "\r\n";
        socket->write(response);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    if (path == "/api/ping" && method == "GET") {
        QJsonObject json;
        json["status"] = "ok";
        json["version"] = QCoreApplication::applicationVersion();
        sendJsonResponse(socket, 200, json, allowedOrigin);
        return;
    }

    if (path == "/api/version" && method == "GET") {
        QJsonObject json;
        json["name"] = "Copper Download Manager";
        json["version"] = QCoreApplication::applicationVersion();
        json["status"] = "running";
        sendJsonResponse(socket, 200, json, allowedOrigin);
        return;
    }

    if (path == "/api/download" && method == "POST") {
        QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            sendJsonResponse(socket, 400, {{"error", "Invalid JSON"}}, allowedOrigin);
            return;
        }

        QJsonObject obj = doc.object();
        QString url = obj["url"].toString();
        QString filename = obj["filename"].toString();
        QString savePath = obj["path"].toString();

        if (url.isEmpty()) {
            sendJsonResponse(socket, 400, {{"error", "URL is required"}}, allowedOrigin);
            return;
        }
        if (!url.startsWith("http") && !url.startsWith("ftp") && !url.startsWith("magnet:?")) {
            sendJsonResponse(socket, 400, {{"error", "Unsupported URL scheme"}}, allowedOrigin);
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
            id = DownloadManager::instance().addDownload(url, fullSavePath, "HTTP");
        }

        QJsonObject response;
        response["success"] = true;
        response["id"] = id;
        response["message"] = "Download added successfully";
        sendJsonResponse(socket, 200, response, allowedOrigin);

        emit downloadRequested(url, filename, savePath);
        return;
    }

    if (path == "/api/torrent" && method == "POST") {
        QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            sendJsonResponse(socket, 400, {{"error", "Invalid JSON"}}, allowedOrigin);
            return;
        }

        QJsonObject obj = doc.object();
        QString url = obj["url"].toString();
        QString savePath = obj["path"].toString();

        if (url.isEmpty()) {
            sendJsonResponse(socket, 400, {{"error", "URL is required"}}, allowedOrigin);
            return;
        }
        if (!url.startsWith("http") && !url.startsWith("magnet:?")) {
            sendJsonResponse(socket, 400, {{"error", "Unsupported URL scheme"}}, allowedOrigin);
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
        sendJsonResponse(socket, 200, response, allowedOrigin);
        return;
    }

    if (path == "/api/forward" && method == "POST") {
        QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            sendJsonResponse(socket, 400, {{"error", "Invalid JSON"}}, allowedOrigin);
            return;
        }

        QJsonObject obj = doc.object();
        QString argument = obj["argument"].toString();

        if (argument.isEmpty()) {
            sendJsonResponse(socket, 400, {{"error", "Argument is required"}}, allowedOrigin);
            return;
        }

        Logger::instance().info("Forward received: " + argument.left(100));
        emit argumentForwarded(argument);

        QJsonObject response;
        response["success"] = true;
        response["message"] = "Argument forwarded";
        sendJsonResponse(socket, 200, response, allowedOrigin);
        return;
    }

    if (path == "/api/downloads" && method == "GET") {
        QJsonArray downloadsArray;
        QVector<DownloadItem> downloads = DownloadManager::instance().getDownloads();

        for (const DownloadItem& item : downloads) {
            QJsonObject dlObj;
            dlObj["id"] = item.id;
            dlObj["url"] = item.url;
            dlObj["fileName"] = item.fileName;
            dlObj["status"] = item.status;
            dlObj["progress"] = item.progress;
            dlObj["downloadedSize"] = item.downloadedSize;
            dlObj["totalSize"] = item.totalSize;
            dlObj["speed"] = item.speed;
            dlObj["type"] = item.type;
            downloadsArray.append(dlObj);
        }

        QJsonObject response;
        response["downloads"] = downloadsArray;
        response["count"] = downloads.size();
        sendJsonResponse(socket, 200, response, allowedOrigin);
        return;
    }

    sendJsonResponse(socket, 404, {{"error", "Not found"}}, allowedOrigin);
}

void LocalServer::sendJsonResponse(QTcpSocket* socket, int statusCode, const QJsonObject& json, const QString& origin) {
    QByteArray body = QJsonDocument(json).toJson(QJsonDocument::Compact);
    QByteArray response;
    response += "HTTP/1.1 " + QByteArray::number(statusCode) + " OK\r\n";
    response += "Content-Type: application/json\r\n";
    if (!origin.isEmpty()) {
        response += "Access-Control-Allow-Origin: " + origin.toUtf8() + "\r\n";
    }
    response += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    response += "Access-Control-Allow-Headers: Content-Type\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";
    response += body;

    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
}

void LocalServer::sendHtmlResponse(QTcpSocket* socket, int statusCode, const QString& html) {
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
