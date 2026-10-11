#ifndef NATIVEMESSAGING_H
#define NATIVEMESSAGING_H

#include <QString>
#include <QStandardPaths>
#include <QDir>

// Registers and refreshes the browser native-messaging host manifests so the
// extension can talk to the desktop app via a named pipe instead of the
// copper:// protocol or a localhost HTTP port.
//
// A native messaging host manifest (a small JSON file at a well-known OS
// location per browser) names the host executable ("path") and the set of
// extension IDs allowed to talk to it via chrome.runtime.sendNativeMessage.
class NativeMessaging {
public:
    // Host application id as known to the browsers.
    static QString hostName();

    // Name of the companion executable (sibling of the app exe).
    static QString hostExecutableName();

    // Write/refresh the host manifest for the given browser. For Chrome/Edge the
    // allowed_origins must contain the exact chrome-extension ID; for Firefox
    // allowed_extensions contains the gecko.id. Returns the path written or empty.
    //
    // The manifest is *merged*: any still-valid ID already listed in the file is
    // preserved and the given ids are added, so registering the stable store IDs
    // at startup never clobbers a sideloaded dev ID added through the "register"
    // handshake (or --register-native-extension), and vice versa.
    static QString installManifest(const QString& browser, const QStringList& extensionIds);

    // Record where the host executable lives and the app launcher, so the host
    // can find and start Copper independently of the browser.
    static bool writeHostConfig();

    // Per-user writable directory holding copper_host_config.json. Deterministic
    // and identical from both the app and the native host (independent of each
    // process's application name), so the host can always locate Copper even when
    // the app is installed under a non-writable path (e.g. C:\Program Files\...).
    static inline QString hostConfigPath() {
        QString base;
#ifdef PLATFORM_WINDOWS
        base = qEnvironmentVariable("LOCALAPPDATA");
        if (base.isEmpty()) base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
#endif
        if (base.isEmpty())
            base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        return QDir(base).filePath("Copper/copper_host_config.json");
    }

    // All browser registration locations that installManifest() may write.
    static QStringList manifestPaths();

    // Stable store IDs the app registers on startup so a fresh install works
    // without a manual step. The Chrome Web Store / Edge Add-ons IDs are derived
    // from each package's fixed CRX signing key; the Firefox gecko.id is the
    // add-on's AMO GUID. Sideloaded/dev builds with a different ID are still
    // added through the "register" handshake or --register-native-extension.
    static QString defaultChromeExtensionId();
    static QString edgeExtensionId();
    static QString firefoxExtensionId();
};

#endif
