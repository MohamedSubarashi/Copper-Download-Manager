#include "utils/NativeMessaging.h"
#include "utils/ApiToken.h"
#include "utils/Logger.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
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

// Chromium extension IDs are exactly 32 characters drawn from a-p (the hex
// digest mapped onto the alphabet). A malformed entry in allowed_origins is
// dead weight at best; if it is the only entry, native messaging is disabled
// for every extension - which is how the local API token became unreachable.
static bool isValidChromiumExtensionId(const QString& id) {
    static const QRegularExpression re(QStringLiteral("^[a-p]{32}$"));
    return re.match(id).hasMatch();
}

QString NativeMessaging::firefoxExtensionId() {
    // The add-on's AMO GUID (addons.mozilla.org/api/v5/addons/addon/
    // copper-download-manager -> guid). It must match the shipped
    // extensions/Firefox/manifest.json browser_specific_settings.gecko.id or the
    // browser refuses the native-messaging connection.
    return "CopperDownloadManager@mohamedsubarashi";
}

QString NativeMessaging::defaultChromeExtensionId() {
    // Chrome Web Store ID of the packaged build (fixed by its CRX signing key;
    // see README "Browser extensions"). Registering it on startup is what lets
    // the extension read the local-API token over native messaging.
    return "cmokehdedogdpbfhlamopeifgogaihni";
}

QString NativeMessaging::edgeExtensionId() {
    // Edge Add-ons ID of the same packaged build (from its store listing).
    return "fnganciafgifhelimkoeiiapdcdmnnlb";
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

    const bool isFirefox = (browser == "firefox");
    const QString listKey = isFirefox ? QStringLiteral("allowed_extensions")
                                      : QStringLiteral("allowed_origins");

    // Merge, never clobber: keep any ID already registered and add the given
    // ones. The startup call supplies the stable store ID; a sideloaded build
    // adds its own through the "register" handshake - each must survive the
    // other's write.
    QStringList firefoxIds;
    QStringList chromiumIds;
    auto addFirefox = [&firefoxIds](const QString& raw) {
        const QString id = raw.trimmed();
        if (!id.isEmpty() && !firefoxIds.contains(id)) firefoxIds.append(id);
    };
    auto addChromium = [&chromiumIds](const QString& raw) {
        const QString id = raw.trimmed();
        if (isValidChromiumExtensionId(id) && !chromiumIds.contains(id)) chromiumIds.append(id);
    };

    QFile existing(target);
    if (existing.open(QIODevice::ReadOnly)) {
        const QJsonObject prev = QJsonDocument::fromJson(existing.readAll()).object();
        for (const QJsonValue& v : prev.value(listKey).toArray()) {
            QString id = v.toString();
            if (!isFirefox) {
                const QString prefix = QStringLiteral("chrome-extension://");
                if (!id.startsWith(prefix)) continue;
                id = id.mid(prefix.size());
                if (id.endsWith(QLatin1Char('/'))) id.chop(1);
            }
            if (isFirefox) addFirefox(id); else addChromium(id);
        }
        existing.close();
    }
    for (const QString& id : extensionIds) {
        if (isFirefox) addFirefox(id); else addChromium(id);
    }

    QJsonObject manifest;
    manifest["name"] = hostName();
    manifest["description"] = "Copper Download Manager native messaging host";
    manifest["path"] = QDir::toNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath(hostExecutableName()));
    manifest["type"] = "stdio";

    if (isFirefox) {
        if (firefoxIds.isEmpty()) firefoxIds.append(firefoxExtensionId());
        QJsonArray exts;
        for (const QString& id : firefoxIds) exts.append(id);
        manifest["allowed_extensions"] = exts;
    } else {
        if (chromiumIds.isEmpty())
            chromiumIds.append(browser == "edge" ? edgeExtensionId() : defaultChromeExtensionId());
        QJsonArray origins;
        for (const QString& id : chromiumIds) origins.append("chrome-extension://" + id + "/");
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
    //
    // Stored in the per-user writable config dir, NOT next to the .exe: when the
    // app is installed under a protected path (e.g. C:\Program Files\...) the
    // directory is not writable and the write would fail, breaking the host's
    // ability to find/launch Copper from a fresh browser start.
    const QString host = hostExePath();
    QFileInfo fi(host);

    const QString configPath = hostConfigPath();

    // Merge into whatever is already there: ApiToken may have written the
    // apiToken key first, and a full rewrite must not drop keys written by
    // another code path (or vice versa).
    QJsonObject cfg;
    {
        QFile in(configPath);
        if (in.open(QIODevice::ReadOnly)) {
            const QJsonObject existing = QJsonDocument::fromJson(in.readAll()).object();
            if (!existing.isEmpty()) cfg = existing;
        }
    }
    cfg["copperExecutable"] = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    // Hand the local API token to the native host: the extension asks the host
    // for it (action "getToken") and attaches it to its localhost API calls.
    cfg["apiToken"] = ApiToken::token();

    QVariantMap out;

#ifdef PLATFORM_WINDOWS
    // Also record in the registry for convenience/robustness.
    QSettings reg("HKEY_CURRENT_USER\\Software\\Copper", QSettings::NativeFormat);
    reg.setValue("AppPath", QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
    reg.setValue("HostPath", fi.absolutePath());
#endif

    QFileInfo cf(configPath);
    QDir().mkpath(cf.absolutePath());
    // Atomic replace: the native host reads this file at browser startup, so
    // it must never observe a half-written document.
    QSaveFile f(configPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        Logger::instance().error("NativeMessaging: cannot write host config to " + configPath);
        return false;
    }
    f.write(QJsonDocument(cfg).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        Logger::instance().error("NativeMessaging: cannot commit host config to " + configPath);
        return false;
    }
    Logger::instance().info("NativeMessaging: wrote host config to " + configPath);
    return true;
}
