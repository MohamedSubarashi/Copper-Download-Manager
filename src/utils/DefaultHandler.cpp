#include "utils/DefaultHandler.h"
#include "utils/Logger.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QDesktopServices>
#include <QUrl>

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#include <QSettings>
#endif

#ifdef PLATFORM_LINUX
#include <QStandardPaths>
#endif

DefaultHandler::DefaultHandler() {}

DefaultHandler& DefaultHandler::instance() {
    static DefaultHandler instance;
    return instance;
}

void DefaultHandler::registerAsDefault() {
    QString appPath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    QString appDir = QCoreApplication::applicationDirPath();
    QString exeName = QFileInfo(appPath).fileName();

#ifdef PLATFORM_WINDOWS
    // 1. Register copper:// protocol
    QSettings copperIcon("HKEY_CURRENT_USER\\Software\\Classes\\copper", QSettings::NativeFormat);
    copperIcon.setValue(".", "URL:Copper Download Manager Protocol");
    copperIcon.setValue("URL Protocol", "");

    QSettings copperCmd("HKEY_CURRENT_USER\\Software\\Classes\\copper\\shell\\open\\command", QSettings::NativeFormat);
    copperCmd.setValue(".", "\"" + appPath + "\" \"%1\"");

    QSettings copperDefaultIcon("HKEY_CURRENT_USER\\Software\\Classes\\copper\\DefaultIcon", QSettings::NativeFormat);
    copperDefaultIcon.setValue(".", "\"" + appPath + "\",0");

    // 2. Register ProgIds for HTTP/HTTPS/FTP/Magnet
    auto registerProgId = [&](const QString& progId, const QString& description) {
        QSettings cls("HKEY_CURRENT_USER\\Software\\Classes\\" + progId, QSettings::NativeFormat);
        cls.setValue(".", description);
        cls.setValue("URL Protocol", "");

        QSettings cmd("HKEY_CURRENT_USER\\Software\\Classes\\" + progId + "\\shell\\open\\command", QSettings::NativeFormat);
        cmd.setValue(".", "\"" + appPath + "\" \"%1\"");

        QSettings icon("HKEY_CURRENT_USER\\Software\\Classes\\" + progId + "\\DefaultIcon", QSettings::NativeFormat);
        icon.setValue(".", "\"" + appPath + "\",0");
    };

    registerProgId("CopperHTTP", "Copper Download Manager HTTP");
    registerProgId("CopperHTTPS", "Copper Download Manager HTTPS");
    registerProgId("CopperFTP", "Copper Download Manager FTP");
    registerProgId("CopperMagnet", "Copper Download Manager Magnet");
    registerProgId("CopperCopper", "Copper Download Manager");
    registerProgId("CopperTorrent", "Copper Download Manager Torrent File");

    // Register right-click "Open with Copper" for .torrent files
    QSettings torrentOpen("HKEY_CURRENT_USER\\Software\\Classes\\CopperTorrent\\shell\\open", QSettings::NativeFormat);
    torrentOpen.setValue(".", "Open with Copper Download Manager");
    QSettings torrentOpenCmd("HKEY_CURRENT_USER\\Software\\Classes\\CopperTorrent\\shell\\open\\command", QSettings::NativeFormat);
    torrentOpenCmd.setValue(".", "\"" + appPath + "\" \"%1\"");

    // 3. Register .torrent file association
    QSettings torrentExt("HKEY_CURRENT_USER\\Software\\Classes\\.torrent", QSettings::NativeFormat);
    torrentExt.setValue(".", "CopperTorrent");
    QSettings torrentIcon("HKEY_CURRENT_USER\\Software\\Classes\\.torrent\\DefaultIcon", QSettings::NativeFormat);
    torrentIcon.setValue(".", "\"" + appPath + "\",0");

    // 4. Register app capabilities
    QString capBase = "Software\\Copper\\Capabilities";
    QSettings caps("HKEY_CURRENT_USER\\" + capBase, QSettings::NativeFormat);
    caps.setValue("ApplicationName", "Copper Download Manager");
    caps.setValue("ApplicationDescription", "High-speed download manager with chunked downloads, torrent support, and yt-dlp integration");

    QSettings urlAssoc("HKEY_CURRENT_USER\\" + capBase + "\\UrlAssociations", QSettings::NativeFormat);
    urlAssoc.setValue("http", "CopperHTTP");
    urlAssoc.setValue("https", "CopperHTTPS");
    urlAssoc.setValue("ftp", "CopperFTP");
    urlAssoc.setValue("magnet", "CopperMagnet");
    urlAssoc.setValue("copper", "CopperCopper");

    QSettings fileAssoc("HKEY_CURRENT_USER\\" + capBase + "\\FileAssociations", QSettings::NativeFormat);
    fileAssoc.setValue(".torrent", "CopperTorrent");

    // 4. Register in RegisteredApplications
    QSettings regApps("HKEY_CURRENT_USER\\Software\\RegisteredApplications", QSettings::NativeFormat);
    regApps.setValue("Copper Download Manager", "Software\\Copper\\Capabilities");

    // 5. Update status in registry so isRegistered() works
    QSettings status("HKEY_CURRENT_USER\\Software\\Copper", QSettings::NativeFormat);
    status.setValue("RegisteredAsDefault", true);

    Logger::instance().info("Registered as default downloader (Windows RegisteredApplications + Capabilities)");

    // 6. Open Windows Default Apps settings
    QDesktopServices::openUrl(QUrl("ms-settings:defaultapps"));
#endif

#ifdef PLATFORM_LINUX
    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/applications";
    QDir().mkpath(desktopPath);

    QString desktopFile = desktopPath + "/copper-download.desktop";
    QFile file(desktopFile);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write("[Desktop Entry]\n");
        file.write("Type=Application\n");
        file.write("Name=Copper Download Manager\n");
        file.write("Exec=" + appPath.toUtf8() + " %u\n");
        file.write("Icon=copper-download\n");
        file.write("Categories=Network;\n");
        file.write("MimeType=x-scheme-handler/http;x-scheme-handler/https;x-scheme-handler/magnet;x-scheme-handler/copper;\n");
        file.write("Terminal=false\n");
        file.close();
    }

    Logger::instance().info("Registered as default downloader (Linux .desktop file)");
#endif

#ifdef PLATFORM_MACOS
    Logger::instance().info("macOS handler registration requires Info.plist configuration");
#endif
}

void DefaultHandler::unregisterAsDefault() {
#ifdef PLATFORM_WINDOWS
    // Remove copper protocol
    QSettings copperCmd("HKEY_CURRENT_USER\\Software\\Classes\\copper\\shell\\open\\command", QSettings::NativeFormat);
    copperCmd.remove("");
    QSettings copperIcon("HKEY_CURRENT_USER\\Software\\Classes\\copper", QSettings::NativeFormat);
    copperIcon.remove("");

    // Remove ProgIds
    auto removeProgId = [](const QString& progId) {
        QSettings cmd("HKEY_CURRENT_USER\\Software\\Classes\\" + progId + "\\shell\\open\\command", QSettings::NativeFormat);
        cmd.remove("");
        QSettings cls("HKEY_CURRENT_USER\\Software\\Classes\\" + progId, QSettings::NativeFormat);
        cls.remove("");
    };
    removeProgId("CopperHTTP");
    removeProgId("CopperHTTPS");
    removeProgId("CopperFTP");
    removeProgId("CopperMagnet");
    removeProgId("CopperCopper");
    removeProgId("CopperTorrent");

    // Remove .torrent file association and context menu
    QSettings torrentExt("HKEY_CURRENT_USER\\Software\\Classes\\.torrent", QSettings::NativeFormat);
    torrentExt.remove("");
    QSettings torrentOpen("HKEY_CURRENT_USER\\Software\\Classes\\CopperTorrent\\shell\\open\\command", QSettings::NativeFormat);
    torrentOpen.remove("");

    // Remove capabilities
    QSettings caps("HKEY_CURRENT_USER\\Software\\Copper\\Capabilities", QSettings::NativeFormat);
    caps.remove("");
    QSettings urlAssoc("HKEY_CURRENT_USER\\Software\\Copper\\Capabilities\\UrlAssociations", QSettings::NativeFormat);
    urlAssoc.remove("");
    QSettings fileAssoc("HKEY_CURRENT_USER\\Software\\Copper\\Capabilities\\FileAssociations", QSettings::NativeFormat);
    fileAssoc.remove("");

    // Remove from RegisteredApplications
    QSettings regApps("HKEY_CURRENT_USER\\Software\\RegisteredApplications", QSettings::NativeFormat);
    regApps.remove("Copper Download Manager");

    // Clear status
    QSettings status("HKEY_CURRENT_USER\\Software\\Copper", QSettings::NativeFormat);
    status.remove("RegisteredAsDefault");

    Logger::instance().info("Unregistered as default downloader");
#endif
}

bool DefaultHandler::isRegistered() {
#ifdef PLATFORM_WINDOWS
    QSettings status("HKEY_CURRENT_USER\\Software\\Copper", QSettings::NativeFormat);
    return status.value("RegisteredAsDefault", false).toBool();
#endif
#ifdef PLATFORM_LINUX
    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/applications/copper-download.desktop";
    return QFile::exists(desktopPath);
#endif
    return false;
}

QString DefaultHandler::getRegisteredProtocol() const {
    return "copper, http, https, ftp, magnet";
}

void DefaultHandler::autoUpdateRegistryPath() {
#ifdef PLATFORM_WINDOWS
    // Always keep the copper:// protocol registration pointing at the current
    // executable, even if the app was never formally "registered as default" —
    // this heals partial registrations (e.g. after the exe was moved) so the
    // copper:// flow keeps working. The "heavier" HTTP/HTTPS/FTP/magnet/ProgId
    // registrations are only refreshed when a full registration exists.
    QString currentPath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    QString quotedCmd = "\"" + currentPath + "\" \"%1\"";

    auto updateCmd = [&](const QString& progId) {
        QSettings cmd("HKEY_CURRENT_USER\\Software\\Classes\\" + progId + "\\shell\\open\\command", QSettings::NativeFormat);
        QString existing = cmd.value(".").toString();
        if (existing != quotedCmd) {
            cmd.setValue(".", quotedCmd);
            Logger::instance().info("Updated registry path for " + progId);
        }
    };

    updateCmd("copper");
    // Also repair the copper protocol's base key if it is missing entirely.
    QSettings copperIcon("HKEY_CURRENT_USER\\Software\\Classes\\copper", QSettings::NativeFormat);
    if (copperIcon.value("URL Protocol").toString().isEmpty()) {
        copperIcon.setValue(".", "URL:Copper Download Manager Protocol");
        copperIcon.setValue("URL Protocol", "");
        Logger::instance().info("Re-registered copper:// protocol base key");
    }

    if (!isRegistered()) return;

    updateCmd("CopperHTTP");
    updateCmd("CopperHTTPS");
    updateCmd("CopperFTP");
    updateCmd("CopperMagnet");
    updateCmd("CopperCopper");
    updateCmd("CopperTorrent");
#endif
}
