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
#include <string>
#endif

// ---------------------------------------------------------------------------
// Registration locations
// ---------------------------------------------------------------------------

// Per-user directory that owns the host manifest and the copper_host_config,
// deliberately outside any browser's data dir: the app writes the manifest once
// and points every browser at it through the registry, so nothing breaks when a
// browser profile moves or a second profile is added.
#ifdef PLATFORM_WINDOWS
static QString windowsManifestDir() {
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return base + "/Copper/Copper Download Manager/NativeMessagingHosts";
}
#endif

// The file installManifest() writes (and the registry points at) for a browser.
// Empty means the browser is not supported on this platform.
static QString manifestTargetPath(const QString& browser) {
#if defined(PLATFORM_WINDOWS)
    if (browser == "chrome" || browser == "edge" || browser == "firefox")
        return QDir(windowsManifestDir()).filePath(browser + "/" +
                                                   NativeMessaging::hostName() + ".json");
    return QString();
#elif defined(PLATFORM_MACOS)
    const QString name = NativeMessaging::hostName() + ".json";
    if (browser == "chrome")
        return QDir::homePath() + "/Library/Application Support/Google/Chrome/NativeMessagingHosts/" + name;
    if (browser == "edge")
        return QDir::homePath() + "/Library/Application Support/Microsoft Edge/NativeMessagingHosts/" + name;
    if (browser == "firefox")
        return QDir::homePath() + "/Library/Application Support/Mozilla/NativeMessagingHosts/" + name;
    return QString();
#else
    const QString name = NativeMessaging::hostName() + ".json";
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (browser == "chrome")
        return dataDir + "/google-chrome/NativeMessagingHosts/" + name;
    if (browser == "edge")
        return dataDir + "/microsoft-edge/NativeMessagingHosts/" + name;
    if (browser == "firefox")
        return dataDir + "/mozilla/native-messaging-hosts/" + name;
    return QString();
#endif
}

#ifdef PLATFORM_WINDOWS
namespace {
// On Windows a browser does not scan its profile for host manifests: it resolves
// <name> through HKCU\Software\<vendor>\NativeMessagingHosts\<name> and reads the
// path to the manifest from that key's default (unnamed) value. The JSON-only
// registration the app used to do was therefore invisible to Chrome/Edge/Firefox
// (User Data\NativeMessagingHosts is a macOS/Linux convention), so the host was
// never found and the extension never obtained the API token.
bool writeRegistryDefaultString(const QString& subKey, const QString& value) {
    const std::wstring key = subKey.toStdWString();
    HKEY handle = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr,
                              REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &handle, nullptr);
    if (rc != ERROR_SUCCESS) return false;
    const std::wstring data = QDir::toNativeSeparators(value).toStdWString();
    const DWORD bytes = static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t));
    rc = RegSetValueExW(handle, nullptr, 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(data.c_str()), bytes);
    RegCloseKey(handle);
    return rc == ERROR_SUCCESS;
}

// HKCU subkeys whose default value must point at the browser's manifest. In test
// mode they are redirected under Software\Copper\... so a test run can never
// re-point the developer's real browser at a throwaway profile manifest.
QStringList registryHostKeys(const QString& browser) {
    const QString prefix = QStandardPaths::isTestModeEnabled()
        ? QStringLiteral("Software\\Copper\\TestNativeMessagingHosts\\")
        : QStringLiteral("Software\\");
    const QString name = NativeMessaging::hostName();
    QStringList keys;
    if (browser == "chrome") {
        keys << prefix + "Google\\Chrome\\NativeMessagingHosts\\" + name;
    } else if (browser == "edge") {
        keys << prefix + "Microsoft\\Edge\\NativeMessagingHosts\\" + name;
    } else if (browser == "firefox") {
        keys << prefix + "Mozilla\\NativeMessagingHosts\\" + name;
    }
    return keys;
}
}  // namespace
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
    paths << manifestTargetPath("chrome") << manifestTargetPath("edge")
          << manifestTargetPath("firefox");
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
    const QString target = manifestTargetPath(browser);
    if (target.isEmpty()) return QString();

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

#ifdef PLATFORM_WINDOWS
    // Point the browser at the manifest. This registry value - not the JSON file
    // alone - is what makes the browser find the host on Windows.
    for (const QString& subKey : registryHostKeys(browser)) {
        if (writeRegistryDefaultString(subKey, target))
            Logger::instance().info("NativeMessaging: registered host for " + browser +
                                    " at HKCU\\" + subKey);
        else
            Logger::instance().warning("NativeMessaging: failed to register host at HKCU\\" + subKey);
    }
#endif

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

bool NativeMessaging::isExtensionAllowed(const QString& browser, const QString& extensionId) {
    const QString id = extensionId.trimmed();
    if (id.isEmpty()) return false;
    const QString target = manifestTargetPath(browser);
    if (target.isEmpty()) return false;
    QFile f(target);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonObject manifest = QJsonDocument::fromJson(f.readAll()).object();
    const bool isFirefox = (browser == "firefox");
    const QJsonArray list = manifest.value(isFirefox ? QStringLiteral("allowed_extensions")
                                                     : QStringLiteral("allowed_origins")).toArray();
    for (const QJsonValue& v : list) {
        const QString entry = v.toString();
        if (isFirefox) {
            if (entry == id) return true;
        } else if (entry == "chrome-extension://" + id + "/") {
            return true;
        }
    }
    return false;
}
