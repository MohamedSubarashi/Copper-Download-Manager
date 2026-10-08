#ifndef LOCALSERVER_H
#define LOCALSERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHash>
#include <QByteArray>
#include <QJsonObject>
#include <QPointer>

class LocalServer : public QObject {
    Q_OBJECT
public:
    static LocalServer& instance();
    bool start(int port = 24680);
    void stop();
    bool isRunning() const;
    int getPort() const;

signals:
    void downloadRequested(const QString& url, const QString& filename, const QString& path);
    void argumentForwarded(const QString& arg);

private:
    LocalServer();
    void handleConnection(QTcpSocket* socket);
    void handleRequest(QTcpSocket* socket, const QString& method, const QString& path, const QByteArray& body,
                       const QString& origin, const QString& apiToken);
    bool isAllowedOrigin(const QString& origin) const;
    // Responses take a QPointer, not a raw QTcpSocket*: a handler can be
    // suspended (modal dialogs opened by emitted signals run a nested event
    // loop) while the client disconnects and the socket is deleted. A QPointer
    // created while the socket was alive detects that and drops the reply
    // instead of dereferencing freed memory (use-after-free crash).
    // closeAfter=false keeps the connection open so the caller can keep
    // draining an oversized body first (closing mid-send makes Windows reset
    // the connection and the client never sees the status line).
    void sendJsonResponse(QPointer<QTcpSocket> socket, int statusCode, const QJsonObject& json,
                          const QString& origin = QString(), bool closeAfter = true);
    void sendHtmlResponse(QPointer<QTcpSocket> socket, int statusCode, const QString& html);

    struct ConnState {
        QByteArray buffer;
        bool processed = false;
        // >0 after an early 413: bytes of the refused body still to read and
        // drop before the socket may be closed.
        qint64 discardRemaining = 0;
    };
    QHash<QTcpSocket*, ConnState> m_conns;

    QTcpServer* server;
    int serverPort;
};

#endif
