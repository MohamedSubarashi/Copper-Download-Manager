#ifndef LOCALSERVER_H
#define LOCALSERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHash>
#include <QByteArray>

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
    void handleRequest(QTcpSocket* socket, const QString& method, const QString& path, const QByteArray& body, const QString& origin);
    bool isAllowedOrigin(const QString& origin) const;
    void sendJsonResponse(QTcpSocket* socket, int statusCode, const QJsonObject& json, const QString& origin = QString());
    void sendHtmlResponse(QTcpSocket* socket, int statusCode, const QString& html);

    struct ConnState {
        QByteArray buffer;
        bool processed = false;
    };
    QHash<QTcpSocket*, ConnState> m_conns;

    QTcpServer* server;
    int serverPort;
};

#endif
