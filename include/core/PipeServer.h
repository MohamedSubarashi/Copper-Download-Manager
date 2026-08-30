#ifndef PIPESERVER_H
#define PIPESERVER_H

#include <QObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonObject>

// Named-pipe intake used by the native-messaging host. On Windows this is a
// named pipe (\\.\pipe\copper-dm); elsewhere it is a Unix domain socket with
// the same name. This replaces the browser-facing localhost HTTP intake so the
// extension can talk to the desktop app without a TCP port or a custom
// copper:// protocol.
class PipeServer : public QObject {
    Q_OBJECT
public:
    static PipeServer& instance();

    bool start();
    void stop();
    bool isRunning() const;
    QString pipeName() const { return m_pipeName; }

signals:
    void argumentForwarded(const QString& arg);
    // A native-messaging host manifest registration request from the extension.
    // extensionId is the browser-provided runtime.id (fixed gecko.id for
    // Firefox; runtime.id reported at runtime for Chrome).
    void registerExtension(const QString& browser, const QString& extensionId);
    void pingReceived();

private slots:
    void onNewConnection();

private:
    PipeServer();
    void handleMessage(QLocalSocket* socket, const QJsonObject& msg);
    void writeReply(QLocalSocket* socket, const QJsonObject& obj);

    QLocalServer* server;
    QString m_pipeName;
};

#endif
