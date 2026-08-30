#ifndef NATIVEMESSAGING_H
#define NATIVEMESSAGING_H

#include <QString>

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
    static QString installManifest(const QString& browser, const QStringList& extensionIds);

    // Record where the host executable lives and the app launcher, so the host
    // can find and start Copper independently of the browser.
    static bool writeHostConfig();

    // All browser registration locations that installManifest() may write.
    static QStringList manifestPaths();

    // Default extension IDs the app registers on startup (Firefox gecko.id,
    // plus any configured stable Chrome ID). Browser names: "chrome", "firefox".
    static QString defaultChromeExtensionId();
    static QString firefoxExtensionId();
};

#endif
