#ifndef LOCALSERVER_H
#define LOCALSERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>

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
    void handleRequest(QTcpSocket* socket, const QString& method, const QString& path, const QByteArray& body);
    void sendJsonResponse(QTcpSocket* socket, int statusCode, const QJsonObject& json);
    void sendHtmlResponse(QTcpSocket* socket, int statusCode, const QString& html);

    QTcpServer* server;
    int serverPort;
};

#endif
