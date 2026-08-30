#include "utils/NativeMessaging.h"
#include "utils/Logger.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#endif

QString NativeMessaging::hostName() {
    return "com.copper.dm";
}

QString NativeMessaging::hostExecutableName() {
#ifdef PLATFORM_WINDOWS
    return "copper_native_host.exe";
#else
    return "copper_native_host";
#endif
}

static QString hostExePath() {
    return QDir(QCoreApplication::applicationDirPath()).filePath(
        NativeMessaging::hostExecutableName());
}

QString NativeMessaging::firefoxExtensionId() {
    return "copper-download-manager@copper";
}

QString NativeMessaging::defaultChromeExtensionId() {
    // Extension ID used by the packaged Chrome Web Store build (derived from the
    // CRX signing key). For unpacked/development builds the running ID can differ;
    // the app re-points the manifest via the "register" pipe handshake or the
    // --register-native-extension CLI.
    return QString();
}

QStringList NativeMessaging::manifestPaths() {
    QStringList paths;
#ifdef PLATFORM_WINDOWS
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    const QString appData = qEnvironmentVariable("APPDATA");
    paths
        << localAppData + "/Google/Chrome/User Data/NativeMessagingHosts/" + hostName() + ".json"
        << localAppData + "/Google/Chrome Beta/User Data/NativeMessagingHosts/" + hostName() + ".json"
        << localAppData + "/Google/Chrome SxS/User Data/NativeMessagingHosts/" + hostName() + ".json"
        << localAppData + "/Microsoft/Edge/User Data/NativeMessagingHosts/" + hostName() + ".json"
        << appData + "/Mozilla/NativeMessagingHosts/" + hostName() + ".json";
#elif defined(PLATFORM_MACOS)
    paths
        << QDir::homePath() + "/Library/Application Support/Google/Chrome/NativeMessagingHosts/" + hostName() + ".json"
        << QDir::homePath() + "/Library/Application Support/Microsoft Edge/NativeMessagingHosts/" + hostName() + ".json"
        << QDir::homePath() + "/Library/Application Support/Mozilla/NativeMessagingHosts/" + hostName() + ".json";
#else
    paths
        << QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/google-chrome/NativeMessagingHosts/" + hostName() + ".json"
        << QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/chromium/NativeMessagingHosts/" + hostName() + ".json"
        << QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/microsoft-edge/NativeMessagingHosts/" + hostName() + ".json"
        << QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/mozilla/native-messaging-hosts/" + hostName() + ".json";
#endif
    return paths;
}

QString NativeMessaging::installManifest(const QString& browser, const QStringList& extensionIds) {
    QString target;
#if defined(PLATFORM_WINDOWS)
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    const QString appData = qEnvironmentVariable("APPDATA");
    const QString name = hostName() + ".json";
    if (browser == "chrome") {
        target = localAppData + "/Google/Chrome/User Data/NativeMessagingHosts/" + name;
    } else if (browser == "edge") {
        target = localAppData + "/Microsoft/Edge/User Data/NativeMessagingHosts/" + name;
    } else if (browser == "firefox") {
        target = appData + "/Mozilla/NativeMessagingHosts/" + name;
    } else {
        return QString();
    }
#elif defined(PLATFORM_MACOS)
    const QString name = hostName() + ".json";
    if (browser == "chrome") {
        target = QDir::homePath() + "/Library/Application Support/Google/Chrome/NativeMessagingHosts/" + name;
    } else if (browser == "edge") {
        target = QDir::homePath() + "/Library/Application Support/Microsoft Edge/NativeMessagingHosts/" + name;
    } else if (browser == "firefox") {
        target = QDir::homePath() + "/Library/Application Support/Mozilla/NativeMessagingHosts/" + name;
    } else {
        return QString();
    }
#else
    const QString name = hostName() + ".json";
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (browser == "chrome") {
        target = dataDir + "/google-chrome/NativeMessagingHosts/" + name;
    } else if (browser == "edge") {
        target = dataDir + "/microsoft-edge/NativeMessagingHosts/" + name;
    } else if (browser == "firefox") {
        target = dataDir + "/mozilla/native-messaging-hosts/" + name;
    } else {
        return QString();
    }
#endif

    QFileInfo fi(target);
    QDir().mkpath(fi.absolutePath());

    QJsonObject manifest;
    manifest["name"] = hostName();
    manifest["description"] = "Copper Download Manager native messaging host";
    manifest["path"] = QDir::toNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath(hostExecutableName()));
    manifest["type"] = "stdio";

    if (browser == "firefox") {
        QJsonArray exts;
        for (const QString& id : extensionIds) {
            if (!id.isEmpty()) exts.append(id);
        }
        if (exts.isEmpty()) exts.append(firefoxExtensionId());
        manifest["allowed_extensions"] = exts;
    } else {
        QJsonArray origins;
        for (const QString& id : extensionIds) {
            if (!id.isEmpty()) origins.append("chrome-extension://" + id + "/");
        }
        if (origins.isEmpty()) origins.append("chrome-extension://" + defaultChromeExtensionId() + "/");
        manifest["allowed_origins"] = origins;
    }

    QFile f(target);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        Logger::instance().error("NativeMessaging: cannot write manifest to " + target);
        return QString();
    }
    f.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
    f.close();

    Logger::instance().info("NativeMessaging: wrote manifest for " + browser + " at " + target);
    writeHostConfig();
    return target;
}

bool NativeMessaging::writeHostConfig() {
    // A tiny JSON config telling the host where the app lives, so the host can
    // start Copper even when the browser spawns it without our app running.
    QString host = hostExePath();
    QFileInfo fi(host);

    QJsonObject cfg;
    cfg["copperExecutable"] = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());

    QVariantMap out;

#ifdef PLATFORM_WINDOWS
    // Also record in the registry for convenience/robustness.
    QSettings reg("HKEY_CURRENT_USER\\Software\\Copper", QSettings::NativeFormat);
    reg.setValue("AppPath", QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
    reg.setValue("HostPath", fi.absolutePath());
#endif

    const QString configPath = fi.absolutePath() + "/copper_host_config.json";
    QFile f(configPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        Logger::instance().error("NativeMessaging: cannot write host config to " + configPath);
        return false;
    }
    f.write(QJsonDocument(cfg).toJson(QJsonDocument::Indented));
    f.close();
    Logger::instance().info("NativeMessaging: wrote host config to " + configPath);
    return true;
}
