#include "core/PipeServer.h"
#include "utils/Logger.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QDataStream>
#include <QCoreApplication>

namespace {
constexpr int kMaxPipeMessage = 16 * 1024 * 1024;  // 16 MB safety cap
}

PipeServer::PipeServer() : server(nullptr), m_pipeName("copper-dm") {}

PipeServer& PipeServer::instance() {
    static PipeServer instance;
    return instance;
}

bool PipeServer::start() {
    if (server && server->isListening()) {
        return true;
    }

    server = new QLocalServer(this);
    // Let the app talk to the named pipe. On Windows QLocalServer uses a named
    // pipe; on Unix it uses a socket in the user's runtime dir.
    server->setSocketOptions(QLocalServer::UserAccessOption);

    QLocalServer::removeServer(m_pipeName);
    if (!server->listen(m_pipeName)) {
        Logger::instance().error("PipeServer: failed to listen on '" + m_pipeName +
                                 "': " + server->errorString());
        return false;
    }

    connect(server, &QLocalServer::newConnection, this, &PipeServer::onNewConnection);
    Logger::instance().info("PipeServer: listening on '" + m_pipeName + "'");
    return true;
}

void PipeServer::stop() {
    if (server && server->isListening()) {
        server->close();
        Logger::instance().info("PipeServer: stopped");
    }
}

bool PipeServer::isRunning() const {
    return server && server->isListening();
}

void PipeServer::onNewConnection() {
    if (!server->hasPendingConnections()) {
        return;
    }
    QLocalSocket* socket = server->nextPendingConnection();
    if (!socket) {
        return;
    }

    connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);

    connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
        // Read a 4-byte LE length-prefixed JSON message.
        while (socket->bytesAvailable() >= 4) {
            char lenBuf[4];
            socket->peek(lenBuf, 4);
            quint32 len = quint32(uchar(lenBuf[0])) |
                          (quint32(uchar(lenBuf[1])) << 8) |
                          (quint32(uchar(lenBuf[2])) << 16) |
                          (quint32(uchar(lenBuf[3])) << 24);
            if (len > kMaxPipeMessage) {
                Logger::instance().error("PipeServer: message too large (" +
                                         QString::number(len) + " bytes)");
                socket->disconnectFromServer();
                return;
            }
            if (socket->bytesAvailable() < quint32(4 + len)) {
                return;  // wait for more
            }
            QByteArray raw = socket->read(4 + len).mid(4);

            QJsonParseError perr;
            QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
            if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
                Logger::instance().warning("PipeServer: invalid JSON from host: " + perr.errorString());
                writeReply(socket, {{"ok", false}, {"error", "invalid json"}});
                continue;
            }
            handleMessage(socket, doc.object());
        }
    });
}

void PipeServer::handleMessage(QLocalSocket* socket, const QJsonObject& msg) {
    QString action = msg["action"].toString();

    if (action == "ping") {
        emit pingReceived();
        writeReply(socket, {{"ok", true},
                            {"name", "Copper Download Manager"},
                            {"version", QCoreApplication::applicationVersion()}});
        return;
    }

    if (action == "open") {
        emit argumentForwarded("show");
        writeReply(socket, {{"ok", true}, {"message", "Opening Copper"}});
        return;
    }

    if (action == "download") {
        QString url = msg["url"].toString();
        QString filename = msg["filename"].toString();
        QString path = msg["path"].toString();
        if (url.isEmpty()) {
            writeReply(socket, {{"ok", false}, {"error", "URL is required"}});
            return;
        }
        // Build the same copper:// style argument so the intake logic in
        // MainWindow::onArgumentForwarded is reused unchanged. Filename/path are
        // percent-encoded to survive the single-argument handoff.
        const auto enc = [](const QString& v) -> QString {
            QString out;
            const QByteArray b = v.toUtf8();
            for (char c : b) {
                if (c == '%' || c == '&' || c == '=' || c == '+' || c == ' ' ||
                    c == '/' || c == '?' || c == '#' || c == ':' || c == '\\') {
                    out += '%' + QString("%1").arg(uchar(c), 2, 16, QChar('0')).toUpper();
                } else {
                    out += QChar(uchar(c));
                }
            }
            return out;
        };

        QString arg = "copper://download?url=" + enc(url);
        if (!filename.isEmpty()) arg += "&filename=" + enc(filename);
        if (!path.isEmpty()) arg += "&path=" + enc(path);

        emit argumentForwarded(arg);
        writeReply(socket, {{"ok", true}, {"message", "Download accepted"}});
        return;
    }

    if (action == "register") {
        QString browser = msg["browser"].toString();        // "chrome" | "firefox"
        QString extensionId = msg["extensionId"].toString();
        emit registerExtension(browser, extensionId);
        writeReply(socket, {{"ok", true}, {"message", "Registration accepted"}});
        return;
    }

    writeReply(socket, {{"ok", false}, {"error", "unknown action: " + action}});
}

void PipeServer::writeReply(QLocalSocket* socket, const QJsonObject& obj) {
    QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QByteArray frame;
    frame.append(char(body.size() & 0xFF));
    frame.append(char((body.size() >> 8) & 0xFF));
    frame.append(char((body.size() >> 16) & 0xFF));
    frame.append(char((body.size() >> 24) & 0xFF));
    frame.append(body);
    if (socket && socket->isOpen()) {
        socket->write(frame);
        socket->flush();
    }
}
