#include "ui/SettingsDialog.h"
#include "utils/Logger.h"
#include "utils/ThemeManager.h"
#include "utils/YtDlpManager.h"
#include "utils/FfmpegManager.h"
#include "utils/Aria2cManager.h"
#include "utils/DefaultHandler.h"
#include "utils/UpdateManager.h"
#include "db/DatabaseManager.h"
#include "core/DownloadManager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QGroupBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QFileDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QSpinBox>
#include <QGridLayout>
#include <QCoreApplication>
#include <QSettings>
#include <QProcess>

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Settings");
    setWindowIcon(QIcon(":/icons/Settings.png"));
    resize(650, 550);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    tabWidget = new QTabWidget();

    // General Tab
    QWidget* generalTab = new QWidget();
    QVBoxLayout* generalLayout = new QVBoxLayout(generalTab);

    QGroupBox* themeGroup = new QGroupBox("Appearance");
    QVBoxLayout* themeBoxLayout = new QVBoxLayout(themeGroup);
    QHBoxLayout* themeLayout = new QHBoxLayout();
    themeLayout->addWidget(new QLabel("Theme:"));
    QComboBox* themeCombo = new QComboBox();
    themeCombo->addItem(QIcon(":/icons/LightMode.png"), "Light");
    themeCombo->addItem(QIcon(":/icons/DarkMode.png"), "Dark");
    themeCombo->addItem(QIcon(":/icons/FollowSystem.png"), "System");
    QString currentTheme = DatabaseManager::instance().getSetting("theme", "System");
    themeCombo->setCurrentText(currentTheme);
    connect(themeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [themeCombo]() {
        ThemeManager::instance().applyTheme(ThemeManager::instance().stringToTheme(themeCombo->currentText()));
        DatabaseManager::instance().saveSetting("theme", themeCombo->currentText());
    });
    themeLayout->addWidget(themeCombo);
    themeBoxLayout->addLayout(themeLayout);
    generalLayout->addWidget(themeGroup);

    QGroupBox* folderGroup = new QGroupBox("Download Folder");
    QVBoxLayout* folderBoxLayout = new QVBoxLayout(folderGroup);
    QHBoxLayout* folderLayout = new QHBoxLayout();
    downloadPathEdit = new QLineEdit();
    downloadPathEdit->setReadOnly(true);
    downloadPathEdit->setText(DatabaseManager::instance().getSetting("downloadPath", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)));
    QPushButton* browseBtn = new QPushButton("Browse...");
    connect(browseBtn, &QPushButton::clicked, this, &SettingsDialog::onBrowseDownloadPath);
    folderLayout->addWidget(downloadPathEdit);
    folderLayout->addWidget(browseBtn);
    folderBoxLayout->addLayout(folderLayout);
    generalLayout->addWidget(folderGroup);

    generalLayout->addStretch();
    tabWidget->addTab(generalTab, "General");

    // Downloads Tab
    QWidget* downloadsTab = new QWidget();
    QVBoxLayout* downloadsLayout = new QVBoxLayout(downloadsTab);

    QGroupBox* chunksGroup = new QGroupBox("Download Options");
    QVBoxLayout* chunksBoxLayout = new QVBoxLayout(chunksGroup);
    QHBoxLayout* chunkLayout = new QHBoxLayout();
    chunkLayout->addWidget(new QLabel("Default download chunks:"));
    chunkCombo = new QComboBox();
    for (int i : {4, 8, 12, 16, 24, 32}) chunkCombo->addItem(QString::number(i));
    chunkCombo->setCurrentText(DatabaseManager::instance().getSetting("chunks", "16"));
    chunkLayout->addWidget(chunkCombo);
    chunksBoxLayout->addLayout(chunkLayout);
    downloadsLayout->addWidget(chunksGroup);

    QGroupBox* fileTypeFilterGroup = new QGroupBox("Download Type Filter");
    QVBoxLayout* fileTypeFilterLayout = new QVBoxLayout(fileTypeFilterGroup);

    QHBoxLayout* filterModeLayout = new QHBoxLayout();
    filterModeLayout->addWidget(new QLabel("Mode:"));
    typeFilterModeCombo = new QComboBox();
    typeFilterModeCombo->addItem("Disabled", "disabled");
    typeFilterModeCombo->addItem("Include selected types", "include");
    typeFilterModeCombo->addItem("Exclude selected types", "exclude");
    QString savedFilterMode = DatabaseManager::instance().getSetting("downloadTypeFilterMode", "disabled");
    int filterModeIndex = typeFilterModeCombo->findData(savedFilterMode);
    if (filterModeIndex >= 0) typeFilterModeCombo->setCurrentIndex(filterModeIndex);
    filterModeLayout->addWidget(typeFilterModeCombo);
    fileTypeFilterLayout->addLayout(filterModeLayout);

    QStringList knownCategories = {"all", "image", "video", "audio", "document", "archive", "executable", "torrent"};
    QStringList savedSelected = DatabaseManager::instance().getSetting("downloadTypeFilterTypes", "all").split(';', Qt::SkipEmptyParts);
    QGridLayout* typeGrid = new QGridLayout();
    int col = 0;
    for (const QString& name : knownCategories) {
        QCheckBox* check = new QCheckBox(name.toUpper()[0] + name.mid(1));
        check->setChecked(savedSelected.isEmpty() || savedSelected.contains(name) || (savedSelected.contains("all") && name == "all"));
        typeFilterChecks.push_back(check);
        typeGrid->addWidget(check, col / 2, col % 2);
        ++col;
    }
    fileTypeFilterLayout->addLayout(typeGrid);
    fileTypeFilterLayout->addWidget(new QLabel("When enabled, URLs matching the selected categories are allowed or blocked based on the chosen mode."));
    downloadsLayout->addWidget(fileTypeFilterGroup);

    QGroupBox* speedGroup = new QGroupBox("Speed Limiter");
    QVBoxLayout* speedBoxLayout = new QVBoxLayout(speedGroup);
    QHBoxLayout* speedLayout = new QHBoxLayout();
    speedLayout->addWidget(new QLabel("Download speed limit (KB/s, 0 = unlimited):"));
    speedLimitSpin = new QSpinBox();
    speedLimitSpin->setRange(0, 1000000);
    speedLimitSpin->setValue(DatabaseManager::instance().getSetting("speedLimit", "0").toInt());
    speedLimitSpin->setSuffix(" KB/s");
    speedLayout->addWidget(speedLimitSpin);
    speedBoxLayout->addLayout(speedLayout);
    downloadsLayout->addWidget(speedGroup);

    downloadsLayout->addStretch();
    tabWidget->addTab(downloadsTab, "Downloads");

    // Tools Tab
    QWidget* toolsTab = new QWidget();
    QVBoxLayout* toolsLayout = new QVBoxLayout(toolsTab);

    QGroupBox* ytdlpGroup = new QGroupBox("yt-dlp");
    QVBoxLayout* ytdlpLayout = new QVBoxLayout(ytdlpGroup);
    ytdlpVersionLabel = new QLabel("Status: checking...");
    ytdlpLayout->addWidget(ytdlpVersionLabel);
    QLabel* ytdlpLicenseLabel = new QLabel("License: Unlicense (Public Domain)\nCopper downloads yt-dlp separately. It is not bundled.");
    ytdlpLicenseLabel->setWordWrap(true);
    ytdlpLicenseLabel->setStyleSheet("color: gray; font-size: 11px;");
    ytdlpLayout->addWidget(ytdlpLicenseLabel);
    QPushButton* updateYtDlpBtn = new QPushButton("Check & Update yt-dlp");
    connect(updateYtDlpBtn, &QPushButton::clicked, this, &SettingsDialog::onUpdateYtDlp);
    ytdlpLayout->addWidget(updateYtDlpBtn);
    toolsLayout->addWidget(ytdlpGroup);

    if (YtDlpManager::instance().isInstalled()) {
        ytdlpVersionLabel->setText("Version: " + YtDlpManager::instance().getVersion());
    } else {
        ytdlpVersionLabel->setText("Status: Not installed");
    }

    connect(&YtDlpManager::instance(), &YtDlpManager::installationProgress, this, [this](const QString& status) {
        if (ytdlpVersionLabel) ytdlpVersionLabel->setText("Status: " + status);
    });

    QGroupBox* ffmpegGroup = new QGroupBox("ffmpeg");
    QVBoxLayout* ffmpegLayout = new QVBoxLayout(ffmpegGroup);
    ffmpegVersionLabel = new QLabel("Status: checking...");
    ffmpegLayout->addWidget(ffmpegVersionLabel);
    QLabel* ffmpegLicenseLabel = new QLabel("License: GPL v2+ (BtbN build)\nCopper downloads ffmpeg separately. It is not bundled.");
    ffmpegLicenseLabel->setWordWrap(true);
    ffmpegLicenseLabel->setStyleSheet("color: gray; font-size: 11px;");
    ffmpegLayout->addWidget(ffmpegLicenseLabel);
    QPushButton* updateFfmpegBtn = new QPushButton("Check & Update ffmpeg");
    connect(updateFfmpegBtn, &QPushButton::clicked, this, &SettingsDialog::onUpdateFfmpeg);
    ffmpegLayout->addWidget(updateFfmpegBtn);
    toolsLayout->addWidget(ffmpegGroup);

    if (FfmpegManager::instance().isInstalled()) {
        ffmpegVersionLabel->setText("Version: " + FfmpegManager::instance().getVersion());
    } else {
        ffmpegVersionLabel->setText("Status: Not installed");
    }

    connect(&FfmpegManager::instance(), &FfmpegManager::installationProgress, this, [this](const QString& status) {
        if (ffmpegVersionLabel) ffmpegVersionLabel->setText("Status: " + status);
    });
    connect(&FfmpegManager::instance(), &FfmpegManager::errorOccurred, this, [this](const QString& error) {
        if (ffmpegVersionLabel) ffmpegVersionLabel->setText("Error: " + error);
    });

    QGroupBox* aria2cGroup = new QGroupBox("aria2c (Torrent/Magnet Support)");
    QVBoxLayout* aria2cLayout = new QVBoxLayout(aria2cGroup);
    aria2cVersionLabel = new QLabel("Status: checking...");
    aria2cLayout->addWidget(aria2cVersionLabel);
    QLabel* aria2cLicenseLabel = new QLabel("License: GPL v2\nCopper downloads aria2c separately. It is not bundled.");
    aria2cLicenseLabel->setWordWrap(true);
    aria2cLicenseLabel->setStyleSheet("color: gray; font-size: 11px;");
    aria2cLayout->addWidget(aria2cLicenseLabel);
    QPushButton* updateAria2cBtn = new QPushButton("Check & Update aria2c");
    connect(updateAria2cBtn, &QPushButton::clicked, this, &SettingsDialog::onUpdateAria2c);
    aria2cLayout->addWidget(updateAria2cBtn);

    QHBoxLayout* seedTimeRow = new QHBoxLayout();
    seedTimeRow->addWidget(new QLabel("Default seed time:"));
    seedTimeCombo = new QComboBox();
    seedTimeCombo->addItem("No seeding", -1);
    seedTimeCombo->addItem("30 minutes", 30);
    seedTimeCombo->addItem("1 hour", 60);
    seedTimeCombo->addItem("2 hours", 120);
    seedTimeCombo->addItem("Seed forever", 0);
    int curSeed = DatabaseManager::instance().getSetting("seedTime", "30").toInt();
    int seedIdx = seedTimeCombo->findData(curSeed);
    if (seedIdx >= 0) seedTimeCombo->setCurrentIndex(seedIdx);
    seedTimeRow->addWidget(seedTimeCombo);
    aria2cLayout->addLayout(seedTimeRow);

    toolsLayout->addWidget(aria2cGroup);

    if (Aria2cManager::instance().isInstalled()) {
        aria2cVersionLabel->setText("Version: " + Aria2cManager::instance().getVersion());
    } else {
        aria2cVersionLabel->setText("Status: Not installed");
    }

    connect(&Aria2cManager::instance(), &Aria2cManager::installationProgress, this, [this](const QString& status) {
        if (aria2cVersionLabel) aria2cVersionLabel->setText("Status: " + status);
    });

    toolsLayout->addStretch();
    tabWidget->addTab(toolsTab, "Tools");

    // Licenses Tab
    QWidget* licensesTab = new QWidget();
    QVBoxLayout* licensesLayout = new QVBoxLayout(licensesTab);
    QPlainTextEdit* licensesText = new QPlainTextEdit();
    licensesText->setReadOnly(true);
    licensesText->setPlainText(
        "COPPER DOWNLOAD MANAGER v0.2.0\n"
        "Third-Party Software Notices\n\n"
        "This product includes software developed by third parties.\n\n"
        "--------------------------------------------------\n"
        "Qt 6.9.3\n"
        "--------------------------------------------------\n"
        "License: GNU Lesser General Public License v3.0 (LGPLv3)\n"
        "Copyright (C) The Qt Company Ltd.\n"
        "Qt is used under the LGPLv3 open-source license.\n"
        "Source: https://www.qt.io/download-open-source\n\n"
        "--------------------------------------------------\n"
        "aria2 1.37.0\n"
        "--------------------------------------------------\n"
        "License: GNU General Public License v2.0 (GPLv2)\n"
        "Copyright (C) Tatsuhiro Tsujikawa\n"
        "aria2 is downloaded separately when requested.\n"
        "Source: https://github.com/aria2/aria2\n\n"
        "--------------------------------------------------\n"
        "FFmpeg (BtbN Builds)\n"
        "--------------------------------------------------\n"
        "License: GPL v2+ (depending on build configuration)\n"
        "Copyright (C) FFmpeg developers\n"
        "FFmpeg is downloaded separately when requested.\n"
        "Source: https://github.com/FFmpeg/FFmpeg\n"
        "Builds: https://github.com/BtbN/FFmpeg-Builds\n\n"
        "--------------------------------------------------\n"
        "yt-dlp\n"
        "--------------------------------------------------\n"
        "License: Unlicense (Public Domain)\n"
        "Copyright (C) yt-dlp contributors\n"
        "yt-dlp is downloaded separately when requested.\n"
        "Source: https://github.com/yt-dlp/yt-dlp\n\n"
        "--------------------------------------------------\n"
        "SQLite\n"
        "--------------------------------------------------\n"
        "License: Public Domain\n"
        "Copyright (C) 2000-2025 Dwayne Richard Hipp\n"
        "Used through Qt SQL plugin for the download database.\n"
        "Source: https://www.sqlite.org/download.html\n"
    );
    licensesLayout->addWidget(licensesText);
    tabWidget->addTab(licensesTab, "Licenses");

    // Trackers Tab
    QWidget* trackersTab = new QWidget();
    QVBoxLayout* trackersLayout = new QVBoxLayout(trackersTab);

    QGroupBox* defaultTrackersGroup = new QGroupBox("Default Torrent Trackers");
    QVBoxLayout* defaultTrackersLayout = new QVBoxLayout(defaultTrackersGroup);
    QLabel* trackersInfo = new QLabel("These trackers will be added to all new torrent downloads:");
    trackersInfo->setWordWrap(true);
    defaultTrackersLayout->addWidget(trackersInfo);
    defaultTrackerEdit = new QPlainTextEdit();
    defaultTrackerEdit->setPlaceholderText("Enter one tracker URL per line...\n\nExample:\nudp://tracker.opentrackr.org:1337/announce\nudp://open.stealth.si:80/announce");
    defaultTrackerEdit->setMinimumHeight(150);
    defaultTrackerEdit->setPlainText(DatabaseManager::instance().getSetting("defaultTrackers", ""));
    defaultTrackersLayout->addWidget(defaultTrackerEdit);

    QHBoxLayout* trackerBtnLayout = new QHBoxLayout();
    QPushButton* addDefaultTrackerBtn = new QPushButton("Add");
    connect(addDefaultTrackerBtn, &QPushButton::clicked, this, &SettingsDialog::onAddDefaultTracker);
    QPushButton* clearDefaultTrackersBtn = new QPushButton("Clear All");
    connect(clearDefaultTrackersBtn, &QPushButton::clicked, this, &SettingsDialog::onClearDefaultTrackers);
    trackerBtnLayout->addWidget(addDefaultTrackerBtn);
    trackerBtnLayout->addWidget(clearDefaultTrackersBtn);
    defaultTrackersLayout->addLayout(trackerBtnLayout);
    trackersLayout->addWidget(defaultTrackersGroup);

    trackersLayout->addStretch();
    tabWidget->addTab(trackersTab, "Trackers");

    // System Tab
    QWidget* systemTab = new QWidget();
    QVBoxLayout* systemLayout = new QVBoxLayout(systemTab);

    QGroupBox* updateGroup = new QGroupBox("Updates");
    QVBoxLayout* updateBoxLayout = new QVBoxLayout(updateGroup);
    QLabel* updateInfoLabel = new QLabel("Current version: " + QCoreApplication::applicationVersion());
    updateBoxLayout->addWidget(updateInfoLabel);
    QPushButton* checkUpdateBtn = new QPushButton("Check for Updates...");
    connect(checkUpdateBtn, &QPushButton::clicked, this, &SettingsDialog::onCheckForUpdates);
    connect(&UpdateManager::instance(), &UpdateManager::updateAvailable, this, &SettingsDialog::onUpdateReady);
    connect(&UpdateManager::instance(), &UpdateManager::noUpdateAvailable, this, [this]() {
        QMessageBox::information(this, "No Updates", "You are running the latest version of Copper Download Manager.");
    });
    connect(&UpdateManager::instance(), &UpdateManager::downloadFinished, this, &SettingsDialog::onUpdateDownloaded);
    connect(&UpdateManager::instance(), &UpdateManager::downloadFailed, this, &SettingsDialog::onUpdateMessage);
    connect(&UpdateManager::instance(), &UpdateManager::errorOccurred, this, &SettingsDialog::onUpdateMessage);
    updateBoxLayout->addWidget(checkUpdateBtn);
    systemLayout->addWidget(updateGroup);

    QGroupBox* handlerGroup = new QGroupBox("Default Downloader");
    QVBoxLayout* handlerBoxLayout = new QVBoxLayout(handlerGroup);
    handlerStatusLabel = new QLabel(DefaultHandler::instance().isRegistered() ? "Registered as default downloader" : "Not registered");
    handlerBoxLayout->addWidget(handlerStatusLabel);
    QHBoxLayout* handlerBtnLayout = new QHBoxLayout();
    QPushButton* registerBtn = new QPushButton("Register as Default Downloader");
    connect(registerBtn, &QPushButton::clicked, this, &SettingsDialog::onRegisterDefaultHandler);
    QPushButton* unregisterBtn = new QPushButton("Unregister");
    connect(unregisterBtn, &QPushButton::clicked, this, &SettingsDialog::onUnregisterDefaultHandler);
    handlerBtnLayout->addWidget(registerBtn);
    handlerBtnLayout->addWidget(unregisterBtn);
    handlerBoxLayout->addLayout(handlerBtnLayout);
    systemLayout->addWidget(handlerGroup);

    QGroupBox* startupGroup = new QGroupBox("Startup");
    QVBoxLayout* startupBoxLayout = new QVBoxLayout(startupGroup);
    QCheckBox* startupCheck = new QCheckBox("Start with system");
    startupCheck->setChecked(DatabaseManager::instance().getSetting("startup", "false") == "true");
    connect(startupCheck, &QCheckBox::toggled, this, [this](bool checked) {
        DatabaseManager::instance().saveSetting("startup", checked ? "true" : "false");
        updateStartupRegistry(checked);
    });
    startupBoxLayout->addWidget(startupCheck);

    QCheckBox* minimizeToTrayCheck = new QCheckBox("Minimize to system tray");
    minimizeToTrayCheck->setChecked(DatabaseManager::instance().getSetting("minimizeToTray", "false") == "true");
    connect(minimizeToTrayCheck, &QCheckBox::toggled, this, [](bool checked) {
        DatabaseManager::instance().saveSetting("minimizeToTray", checked ? "true" : "false");
    });
    startupBoxLayout->addWidget(minimizeToTrayCheck);
    systemLayout->addWidget(startupGroup);

    QGroupBox* statsGroup = new QGroupBox("Statistics");
    QVBoxLayout* statsBoxLayout = new QVBoxLayout(statsGroup);
    int totalDownloads = DownloadManager::instance().getDownloads().size();
    int activeDownloads = 0;
    for (const DownloadItem& item : DownloadManager::instance().getDownloads()) {
        if (item.status == "Downloading") activeDownloads++;
    }
    int completedDownloads = 0;
    for (const DownloadItem& item : DownloadManager::instance().getDownloads()) {
        if (item.status == "Completed") completedDownloads++;
    }
    statsBoxLayout->addWidget(new QLabel("Total downloads: " + QString::number(totalDownloads)));
    statsBoxLayout->addWidget(new QLabel("Active downloads: " + QString::number(activeDownloads)));
    statsBoxLayout->addWidget(new QLabel("Completed downloads: " + QString::number(completedDownloads)));
    systemLayout->addWidget(statsGroup);

    systemLayout->addStretch();
    tabWidget->addTab(systemTab, "System");

    mainLayout->addWidget(tabWidget);

    QHBoxLayout* bottomLayout = new QHBoxLayout();
    QPushButton* saveBtn = new QPushButton("Save");
    connect(saveBtn, &QPushButton::clicked, this, &SettingsDialog::onSave);
    QPushButton* closeBtn = new QPushButton("Close");
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    bottomLayout->addStretch();
    bottomLayout->addWidget(saveBtn);
    bottomLayout->addWidget(closeBtn);
    mainLayout->addLayout(bottomLayout);
}

QString SettingsDialog::getDownloadPath() const {
    return downloadPathEdit->text();
}

void SettingsDialog::onBrowseDownloadPath() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select Download Folder", downloadPathEdit->text());
    if (!dir.isEmpty()) {
        downloadPathEdit->setText(dir);
    }
}

void SettingsDialog::onUpdateYtDlp() {
    Logger::instance().info("Updating yt-dlp...");
    ytdlpVersionLabel->setText("Status: Downloading...");
    YtDlpManager::instance().installOrUpdate();
}

void SettingsDialog::onUpdateFfmpeg() {
    Logger::instance().info("Updating ffmpeg...");
    ffmpegVersionLabel->setText("Status: Downloading...");
    FfmpegManager::instance().installOrUpdate();
}

void SettingsDialog::onUpdateAria2c() {
    Logger::instance().info("Updating aria2c...");
    aria2cVersionLabel->setText("Status: Downloading...");
    Aria2cManager::instance().installOrUpdate();
}

void SettingsDialog::onCheckForUpdates() {
    Logger::instance().info("Checking for updates...");
    UpdateManager::instance().checkForUpdates(false);
}

void SettingsDialog::onUpdateReady(const QString& version) {
    QString msg = "A new version of Copper Download Manager is available:\n\n"
                  "Current version: " + QCoreApplication::applicationVersion() + "\n"
                  "Latest version: " + version + "\n\n"
                  "Do you want to download and install it now?";
    QMessageBox::StandardButton reply = QMessageBox::question(this, "Update Available", msg,
        QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        UpdateManager::instance().downloadAndInstall();
    }
}

void SettingsDialog::onUpdateDownloaded() {
    QString installerPath = UpdateManager::instance().installerPathOrEmpty();
    QMessageBox::StandardButton reply = QMessageBox::question(this, "Update Downloaded",
        "The update has been downloaded. Restart Copper Download Manager to install the update?",
        QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        qApp->quit();
        QProcess::startDetached(installerPath);
    }
}

void SettingsDialog::onUpdateMessage(const QString& error) {
    QMessageBox::warning(this, "Update", error);
}

void SettingsDialog::onClearDefaultTrackers() {
    defaultTrackerEdit->clear();
}

void SettingsDialog::onAddDefaultTracker() {
    QString current = defaultTrackerEdit->toPlainText();
    if (!current.endsWith("\n") && !current.isEmpty()) {
        defaultTrackerEdit->setPlainText(current + "\n");
    }
    defaultTrackerEdit->setFocus();
}

void SettingsDialog::onRegisterDefaultHandler() {
    DefaultHandler::instance().registerAsDefault();
    handlerStatusLabel->setText("Registered as default downloader");
    QMessageBox::information(this, "Register as Default Downloader",
        "Copper Download Manager has been registered as a capable handler for:\n"
        "HTTP, HTTPS, FTP, Magnet, and copper:// protocols.\n\n"
        "Windows Default Apps settings will now open.\n\n"
        "To complete setup:\n"
        "1. In Default Apps, find 'Web browser' or 'HTTP'\n"
        "2. Click it and select 'Copper Download Manager'\n"
        "3. Repeat for HTTPS if desired\n\n"
        "Copper Download Manager will now appear in the list of available apps.");
}

void SettingsDialog::onUnregisterDefaultHandler() {
    DefaultHandler::instance().unregisterAsDefault();
    handlerStatusLabel->setText("Not registered");
    QMessageBox::information(this, "Success", "Copper Download Manager has been unregistered.");
}

void SettingsDialog::accept() {
    onSave();
    QDialog::accept();
}

void SettingsDialog::onSave() {
    DatabaseManager::instance().saveSetting("downloadPath", downloadPathEdit->text());
    DatabaseManager::instance().saveSetting("chunks", chunkCombo->currentText());
    DatabaseManager::instance().saveSetting("defaultTrackers", defaultTrackerEdit->toPlainText());
    DatabaseManager::instance().saveSetting("speedLimit", QString::number(speedLimitSpin->value()));
    DatabaseManager::instance().saveSetting("seedTime", QString::number(seedTimeCombo->currentData().toInt()));
    DatabaseManager::instance().saveSetting("downloadTypeFilterMode", typeFilterModeCombo->currentData().toString());

    QStringList selectedFilters;
    for (QCheckBox* check : typeFilterChecks) {
        if (check && check->isChecked()) {
            selectedFilters << check->text().toLower();
        }
    }
    DatabaseManager::instance().saveSetting("downloadTypeFilterTypes", selectedFilters.join(';'));

    qint64 speedLimit = (qint64)speedLimitSpin->value() * 1024;
    DownloadManager::instance().setSpeedLimit(speedLimit);

    Logger::instance().info("Settings saved");
}

void SettingsDialog::updateStartupRegistry(bool enabled) {
#ifdef PLATFORM_WINDOWS
    QSettings runKey("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    if (enabled) {
        runKey.setValue("CopperDownloadManager", "\"" + QCoreApplication::applicationFilePath() + "\" --minimized");
        Logger::instance().info("Startup autostart enabled");
    } else {
        runKey.remove("CopperDownloadManager");
        Logger::instance().info("Startup autostart disabled");
    }
#endif
}
