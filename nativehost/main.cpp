// Copper native messaging host.
//
// This small console executable is spawned by Chrome/Firefox through the native
// messaging host manifest installed by the desktop app. It speaks the browser's
// stdio protocol (4-byte little-endian length + JSON) on stdin/stdout and
// forwards download/register requests to the desktop app over a named pipe
// (QLocalServer "copper-dm"). If the desktop app is not running, it launches it
// first so the extension's drop behaves like IDM.
//
// Build: Qt Core only (no GUI), console subsystem.

#include <QCoreApplication>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <memory>
#include "utils/NativeMessaging.h"

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

namespace {

constexpr int kMaxMessage = 8 * 1024 * 1024;

QByteArray g_pipeName = "copper-dm";

// Read one native messaging message (4-byte LE length + JSON) from stdin.
bool readMessage(QJsonObject& out) {
    unsigned char hdr[4];
    size_t got = 0;
    while (got < 4) {
        size_t r = fread(hdr + got, 1, 4 - got, stdin);
        if (r == 0) return false;
        got += r;
    }
    quint32 len = quint32(hdr[0]) | (quint32(hdr[1]) << 8) |
                  (quint32(hdr[2]) << 16) | (quint32(hdr[3]) << 24);
    if (len == 0 || len > kMaxMessage) return false;
    QByteArray body(int(len), Qt::Uninitialized);
    got = 0;
    while (got < len) {
        size_t r = fread(body.data() + got, 1, size_t(len) - got, stdin);
        if (r == 0) return false;
        got += r;
    }
    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    out = doc.object();
    return true;
}

void writeMessage(const QJsonObject& obj) {
    QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QByteArray frame;
    frame.append(char(body.size() & 0xFF));
    frame.append(char((body.size() >> 8) & 0xFF));
    frame.append(char((body.size() >> 16) & 0xFF));
    frame.append(char((body.size() >> 24) & 0xFF));
    frame.append(body);
    fwrite(frame.constData(), 1, size_t(frame.size()), stdout);
    fflush(stdout);
}

QString appExecutablePath() {
    // Look for the per-user config written by the desktop app. The app writes it
    // to a writable user dir (not next to the exe) so this works even when the
    // app is installed under a protected path such as C:\Program Files\...
    const QString configPath = NativeMessaging::hostConfigPath();
    QFile cfg(configPath);
    if (cfg.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(cfg.readAll());
        QString path = doc.object()["copperExecutable"].toString();
        if (!path.isEmpty() && QFile::exists(path)) return path;
        cfg.close();
    }
    // Fallback: sibling directory (host ships next to the app exe).
    QDir dir = QDir(QCoreApplication::applicationDirPath());
    QString sibling = dir.filePath(
#ifdef _WIN32
        "CopperDownloadManager.exe"
#else
        "CopperDownloadManager"
#endif
    );
    if (QFile::exists(sibling)) return sibling;
#ifdef _WIN32
    // Fallback: registry hint.
    QSettings reg("HKEY_CURRENT_USER\\Software\\Copper", QSettings::NativeFormat);
    QString rp = reg.value("AppPath").toString();
    if (!rp.isEmpty() && QFile::exists(rp)) return rp;
#endif
    return QString();
}

// Ensure the desktop app is running, then connect to the named pipe and send a
// single request. Returns the app's reply object or an error object.
QJsonObject exchangeWithApp(const QJsonObject& request) {
    QString app = appExecutablePath();
    if (app.isEmpty()) {
        return {{"ok", false}, {"error", "Copper Download Manager not found"}};
    }

    auto tryConnect = [&](int msec) -> std::unique_ptr<QLocalSocket> {
        auto s = std::make_unique<QLocalSocket>();
        s->connectToServer(QString::fromLatin1(g_pipeName));
        if (!s->waitForConnected(msec)) {
            return nullptr;
        }
        return s;
    };

    // First, probe for an already-running app. Only launch a fresh instance if
    // nobody is listening, to avoid racing/duplicate-launching the GUI app.
    auto socket = tryConnect(400);
    if (!socket) {
        QProcess::startDetached(app, QStringList());
        // Give the freshly launched app time to bring up its pipe server.
        for (int i = 0; i < 60 && !socket; ++i) {
            QThread::msleep(250);
            socket = tryConnect(250);
        }
        if (!socket) {
            return {{"ok", false}, {"error", "Cannot reach Copper Download Manager"}};
        }
    }

    QByteArray body = QJsonDocument(request).toJson(QJsonDocument::Compact);
    QByteArray frame;
    frame.append(char(body.size() & 0xFF));
    frame.append(char((body.size() >> 8) & 0xFF));
    frame.append(char((body.size() >> 16) & 0xFF));
    frame.append(char((body.size() >> 24) & 0xFF));
    frame.append(body);

    socket->write(frame);
    socket->flush();
    if (!socket->waitForBytesWritten(4000)) {
        return {{"ok", false}, {"error", "Failed to send request to Copper"}};
    }

    // Read the 4-byte length prefix, then the full body.
    if (!socket->waitForReadyRead(5000)) {
        return {{"ok", false}, {"error", "No reply from Copper Download Manager"}};
    }
    while (socket->bytesAvailable() < 4) {
        if (!socket->waitForReadyRead(2000)) {
            return {{"ok", false}, {"error", "No reply from Copper Download Manager"}};
        }
    }
    QByteArray lenBuf = socket->read(4);
    quint32 len = quint32(uchar(lenBuf[0])) |
                  (quint32(uchar(lenBuf[1])) << 8) |
                  (quint32(uchar(lenBuf[2])) << 16) |
                  (quint32(uchar(lenBuf[3])) << 24);
    while (socket->bytesAvailable() < qint64(len)) {
        if (!socket->waitForReadyRead(2000)) {
            return {{"ok", false}, {"error", "No reply from Copper Download Manager"}};
        }
    }
    QByteArray reply = socket->read(len);
    QJsonDocument doc = QJsonDocument::fromJson(reply);
    if (!doc.isObject()) {
        return {{"ok", false}, {"error", "Invalid reply from Copper"}};
    }
    return doc.object();
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("CopperNativeHost");
    app.setOrganizationName("Copper");

#ifdef _WIN32
    // Switch stdin/stdout to binary mode so length prefixes are not mangled.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    // Process one native messaging message, synchronously.
    QJsonObject msg;
    if (!readMessage(msg)) {
        writeMessage({{"ok", false}, {"error", "invalid native messaging message"}});
        return 0;
    }

    QString action = msg["action"].toString();

    if (action == "download") {
        QJsonObject req;
        req["action"] = "download";
        req["url"] = msg["url"].toString();
        req["filename"] = msg["filename"].toString();
        req["path"] = msg["path"].toString();
        req["format"] = msg["format"].toString();
        writeMessage(exchangeWithApp(req));
        return 0;
    }

    if (action == "ping") {
        QJsonObject rep = exchangeWithApp({{"action", "ping"}});
        rep["running"] = rep.value("ok").toBool();
        writeMessage(rep);
        return 0;
    }

    if (action == "register") {
        QJsonObject req;
        req["action"] = "register";
        req["browser"] = msg["browser"].toString();
        req["extensionId"] = msg["extensionId"].toString();
        writeMessage(exchangeWithApp(req));
        return 0;
    }

    writeMessage({{"ok", false}, {"error", "unknown action: " + action}});
    return 0;
}
