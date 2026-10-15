#include "ui/SettingsDialog.h"
#include "utils/Logger.h"
#include "utils/ThemeManager.h"
#include "utils/TranslationManager.h"
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
#include <QDir>
#include <QSettings>
#include <QProcess>

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Settings"));
    setWindowIcon(QIcon(":/icons/Settings.png"));
    resize(650, 550);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    tabWidget = new QTabWidget();

    // General Tab
    QWidget* generalTab = new QWidget();
    QVBoxLayout* generalLayout = new QVBoxLayout(generalTab);

    QGroupBox* themeGroup = new QGroupBox(tr("Appearance"));
    QVBoxLayout* themeBoxLayout = new QVBoxLayout(themeGroup);
    QHBoxLayout* themeLayout = new QHBoxLayout();
    auto* themeCombo = new QComboBox();
    QLabel* themeLabel = new QLabel(tr("Theme:"));
    themeLabel->setBuddy(themeCombo);
    themeLayout->addWidget(themeLabel);
    themeCombo->setAccessibleName(tr("Theme"));
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

    QGroupBox* languageGroup = new QGroupBox(tr("Language"));
    QHBoxLayout* languageLayout = new QHBoxLayout(languageGroup);
    QLabel* languageLabel = new QLabel(tr("Language:"));
    languageCombo = new QComboBox();
    languageLabel->setBuddy(languageCombo);
    languageCombo->setAccessibleName(tr("Language"));
    for (const TranslationManager::Language& language : TranslationManager::availableLanguages()) {
        // Endonyms are intentionally not translated (see TranslationManager);
        // only the generic "system" entry is localized.
        const QString display = language.code == QLatin1String("system")
                                    ? tr("Auto (system)")
                                    : language.native;
        languageCombo->addItem(display, language.code);
    }
    const int languageIndex = languageCombo->findData(
        DatabaseManager::instance().getSetting("language", "system"));
    if (languageIndex >= 0) languageCombo->setCurrentIndex(languageIndex);
    connect(languageCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        DatabaseManager::instance().saveSetting("language", languageCombo->currentData().toString());
        QMessageBox::information(this, tr("Language"),
            tr("The new language will be applied after you restart Copper Download Manager."));
    });
    languageLayout->addWidget(languageLabel);
    languageLayout->addWidget(languageCombo);
    languageLayout->addStretch();
    generalLayout->addWidget(languageGroup);

    QGroupBox* folderGroup = new QGroupBox(tr("Download Folder"));
    QVBoxLayout* folderBoxLayout = new QVBoxLayout(folderGroup);
    QHBoxLayout* folderLayout = new QHBoxLayout();
    QLabel* folderLabel = new QLabel(tr("Folder:"));
    folderLabel->setBuddy(downloadPathEdit = new QLineEdit());
    folderLayout->addWidget(folderLabel);
    downloadPathEdit->setAccessibleName(tr("Download folder path"));
    downloadPathEdit->setReadOnly(true);
    downloadPathEdit->setText(DatabaseManager::instance().getSetting("downloadPath", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)));
    QPushButton* browseBtn = new QPushButton(tr("Browse..."));
    browseBtn->setAccessibleName(tr("Browse download folder"));
    connect(browseBtn, &QPushButton::clicked, this, &SettingsDialog::onBrowseDownloadPath);
    folderLayout->addWidget(downloadPathEdit);
    folderLayout->addWidget(browseBtn);
    folderBoxLayout->addLayout(folderLayout);
    generalLayout->addWidget(folderGroup);

    generalLayout->addStretch();
    tabWidget->addTab(generalTab, tr("General"));

    // Downloads Tab
    QWidget* downloadsTab = new QWidget();
    QVBoxLayout* downloadsLayout = new QVBoxLayout(downloadsTab);

    QGroupBox* chunksGroup = new QGroupBox(tr("Download Options"));
    QVBoxLayout* chunksBoxLayout = new QVBoxLayout(chunksGroup);
    QHBoxLayout* chunkLayout = new QHBoxLayout();
    QLabel* chunkLabel = new QLabel(tr("Default download chunks:"));
    chunkLabel->setBuddy(chunkCombo = new QComboBox());
    chunkLayout->addWidget(chunkLabel);
    chunkCombo->setAccessibleName(tr("Default download chunks"));
    for (int i : {4, 8, 12, 16, 24, 32}) chunkCombo->addItem(QString::number(i));
    chunkCombo->setCurrentText(DatabaseManager::instance().getSetting("chunks", "16"));
    chunkLayout->addWidget(chunkCombo);
    chunksBoxLayout->addLayout(chunkLayout);
    downloadsLayout->addWidget(chunksGroup);

    QGroupBox* speedGroup = new QGroupBox(tr("Speed Limiter"));
    QVBoxLayout* speedBoxLayout = new QVBoxLayout(speedGroup);
    QHBoxLayout* speedLayout = new QHBoxLayout();
    QLabel* speedLabel = new QLabel(tr("Download speed limit (KB/s, 0 = unlimited):"));
    speedLabel->setBuddy(speedLimitSpin = new QSpinBox());
    speedLayout->addWidget(speedLabel);
    speedLimitSpin->setAccessibleName(tr("Download speed limit"));
    speedLimitSpin->setRange(0, 1000000);
    speedLimitSpin->setValue(DatabaseManager::instance().getSetting("speedLimit", "0").toInt());
    speedLimitSpin->setSuffix(" KB/s");
    speedLayout->addWidget(speedLimitSpin);
    speedBoxLayout->addLayout(speedLayout);
    downloadsLayout->addWidget(speedGroup);

    QGroupBox* concurrencyGroup = new QGroupBox(tr("Parallelism & Retries"));
    QVBoxLayout* concurrencyLayout = new QVBoxLayout(concurrencyGroup);

    QHBoxLayout* maxLayout = new QHBoxLayout();
    QLabel* maxLabel = new QLabel(tr("Maximum simultaneous downloads:"));
    maxLabel->setBuddy(maxConcurrentSpin = new QSpinBox());
    maxLayout->addWidget(maxLabel);
    maxConcurrentSpin->setAccessibleName(tr("Maximum simultaneous downloads"));
    maxConcurrentSpin->setRange(1, 20);
    maxConcurrentSpin->setValue(qBound(1, DatabaseManager::instance().getSetting("maxConcurrentDownloads", "5").toInt(), 20));
    maxLayout->addWidget(maxConcurrentSpin);
    concurrencyLayout->addLayout(maxLayout);

    QHBoxLayout* jobsLayout = new QHBoxLayout();
    QLabel* jobsLabel = new QLabel(tr("Maximum simultaneous yt-dlp jobs:"));
    jobsLabel->setBuddy(maxYtDlpJobsSpin = new QSpinBox());
    jobsLayout->addWidget(jobsLabel);
    maxYtDlpJobsSpin->setAccessibleName(tr("Maximum simultaneous yt-dlp jobs"));
    maxYtDlpJobsSpin->setRange(1, 8);
    maxYtDlpJobsSpin->setValue(qBound(1, DatabaseManager::instance().getSetting("maxYtDlpJobs", "2").toInt(), 8));
    jobsLayout->addWidget(maxYtDlpJobsSpin);
    concurrencyLayout->addLayout(jobsLayout);

    QLabel* jobsHint = new QLabel(
        tr("A playlist counts as ONE yt-dlp job. Running several at once is what gets "
        "requests throttled and makes playlists fail, so keep this low."));
    jobsHint->setWordWrap(true);
    jobsHint->setStyleSheet("color: gray; font-size: 11px;");
    concurrencyLayout->addWidget(jobsHint);

    QHBoxLayout* fragsLayout = new QHBoxLayout();
    QLabel* fragsLabel = new QLabel(tr("Fragments downloaded in parallel per video:"));
    fragsLabel->setBuddy(ytDlpFragmentsSpin = new QSpinBox());
    fragsLayout->addWidget(fragsLabel);
    ytDlpFragmentsSpin->setAccessibleName(tr("Fragments downloaded in parallel per video"));
    ytDlpFragmentsSpin->setRange(1, 16);
    ytDlpFragmentsSpin->setValue(qBound(1, DatabaseManager::instance().getSetting("ytDlpFragments", "4").toInt(), 16));
    fragsLayout->addWidget(ytDlpFragmentsSpin);
    concurrencyLayout->addLayout(fragsLayout);

    QLabel* fragsHint = new QLabel(
        tr("Playlists download one video at a time; parallel fragments make up for it."));
    fragsHint->setWordWrap(true);
    fragsHint->setStyleSheet("color: gray; font-size: 11px;");
    concurrencyLayout->addWidget(fragsHint);

    QHBoxLayout* retriesLayout = new QHBoxLayout();
    QLabel* retriesLabel = new QLabel(tr("Automatic retries for a failed download:"));
    retriesLabel->setBuddy(maxRetriesSpin = new QSpinBox());
    retriesLayout->addWidget(retriesLabel);
    maxRetriesSpin->setAccessibleName(tr("Automatic retries for a failed download"));
    maxRetriesSpin->setRange(0, 10);
    maxRetriesSpin->setValue(qBound(0, DatabaseManager::instance().getSetting("maxRetries", "3").toInt(), 10));
    retriesLayout->addWidget(maxRetriesSpin);
    concurrencyLayout->addLayout(retriesLayout);

    QLabel* retriesHint = new QLabel(
        tr("Retries run with a growing delay and continue the partial file. 0 disables them."));
    retriesHint->setWordWrap(true);
    retriesHint->setStyleSheet("color: gray; font-size: 11px;");
    concurrencyLayout->addWidget(retriesHint);

    downloadsLayout->addWidget(concurrencyGroup);

    QGroupBox* windowGroup = new QGroupBox(tr("Download Windows"));
    QVBoxLayout* windowLayout = new QVBoxLayout(windowGroup);
    autoOpenInfoCheck = new QCheckBox(tr("Open a download-info window for every new download"));
    autoOpenInfoCheck->setAccessibleName(tr("Open a download-info window for every new download"));
    autoOpenInfoCheck->setChecked(DatabaseManager::instance().getSetting("autoOpenDownloadInfo", "false") == "true");
    windowLayout->addWidget(autoOpenInfoCheck);
    QLabel* windowHint = new QLabel(
        tr("The info window can always be opened from the row's context menu or with Ctrl+I. "
        "A playlist opens one window listing all of its items."));
    windowHint->setWordWrap(true);
    windowHint->setStyleSheet("color: gray; font-size: 11px;");
    windowLayout->addWidget(windowHint);
    downloadsLayout->addWidget(windowGroup);

    QGroupBox* uaGroup = new QGroupBox(tr("User-Agent"));
    QVBoxLayout* uaLayout = new QVBoxLayout(uaGroup);
    QHBoxLayout* uaRow = new QHBoxLayout();
    QLabel* uaLabel = new QLabel(tr("User-Agent:"));
    uaLabel->setBuddy(userAgentEdit = new QLineEdit());
    uaRow->addWidget(uaLabel);
    userAgentEdit->setAccessibleName(tr("Custom User-Agent for downloads"));
    userAgentEdit->setPlaceholderText(tr("Leave blank for the default browser-like User-Agent"));
    userAgentEdit->setText(DatabaseManager::instance().getSetting("userAgent", ""));
    uaRow->addWidget(userAgentEdit);
    uaLayout->addLayout(uaRow);
    QLabel* uaHint = new QLabel(tr("Sent with HTTP(S) download and metadata requests. Some servers require a browser-like User-Agent."));
    uaHint->setWordWrap(true);
    uaHint->setStyleSheet("color: gray; font-size: 11px;");
    uaLayout->addWidget(uaHint);
    downloadsLayout->addWidget(uaGroup);

    QGroupBox* headersGroup = new QGroupBox(tr("Custom Headers"));
    QVBoxLayout* headersLayout = new QVBoxLayout(headersGroup);
    headersEdit = new QPlainTextEdit();
    headersEdit->setAccessibleName(tr("Custom download headers"));
    headersEdit->setPlaceholderText(tr("One header per line, for example:\nReferer: https://example.com/\nAuthorization: Bearer token"));
    headersEdit->setMaximumHeight(72);
    headersEdit->setPlainText(DatabaseManager::instance().getSetting("customHeaders", ""));
    headersLayout->addWidget(headersEdit);
    QLabel* headersHint = new QLabel(tr("Sent with every HTTP(S) download request. Some servers require a specific Referer or Authorization header; invalid headers can make servers reject requests."));
    headersHint->setWordWrap(true);
    headersHint->setStyleSheet("color: gray; font-size: 11px;");
    headersLayout->addWidget(headersHint);
    downloadsLayout->addWidget(headersGroup);

    downloadsLayout->addStretch();
    tabWidget->addTab(downloadsTab, tr("Downloads"));

    // Formats Tab (file-format include/exclude filter, applied to every download)
    QWidget* formatsTab = new QWidget();
    QVBoxLayout* formatsLayout = new QVBoxLayout(formatsTab);

    QGroupBox* formatFilterGroup = new QGroupBox(tr("File Format Filter"));
    QVBoxLayout* formatFilterLayout = new QVBoxLayout(formatFilterGroup);
    formatFilterEnabledCheck = new QCheckBox(tr("Enable file-format filter for all downloads"));
    formatFilterEnabledCheck->setAccessibleName(tr("Enable file-format filter for all downloads"));
    formatFilterEnabledCheck->setChecked(DatabaseManager::instance().getSetting("formatFilterEnabled", "true") == "true");
    formatFilterLayout->addWidget(formatFilterEnabledCheck);

    QLabel* includeLabel = new QLabel(tr("Allow these file formats (comma or space separated). Leave empty to allow every format except the excluded ones:"));
    includeLabel->setWordWrap(true);
    formatFilterLayout->addWidget(includeLabel);
    includeExtensionsEdit = new QPlainTextEdit();
    includeExtensionsEdit->setAccessibleName(tr("Include file formats"));
    QString includeSaved = DatabaseManager::instance().getSetting("formatIncludeExtensions", "");
    if (includeSaved.isEmpty()) {
        includeExtensionsEdit->setPlainText("");
    } else {
        includeExtensionsEdit->setPlainText(includeSaved);
    }
    includeExtensionsEdit->setMaximumHeight(80);
    formatFilterLayout->addWidget(includeExtensionsEdit);

    QLabel* excludeLabel = new QLabel(tr("Block these file formats (always blocked, even if listed in the allow list):"));
    excludeLabel->setWordWrap(true);
    formatFilterLayout->addWidget(excludeLabel);
    excludeExtensionsEdit = new QPlainTextEdit();
    excludeExtensionsEdit->setAccessibleName(tr("Exclude file formats"));
    QString excludeSaved = DatabaseManager::instance().getSetting("formatExcludeExtensions", "");
    if (excludeSaved.isEmpty()) {
        excludeExtensionsEdit->setPlainText("png, jpg, jpeg, gif, webp, bmp, svg, ico, avif, jfif, heic, heif, tif, tiff, raw, psd, eps, ai, dng, cr2, nef, arw");
    } else {
        excludeExtensionsEdit->setPlainText(excludeSaved);
    }
    excludeExtensionsEdit->setMaximumHeight(80);
    formatFilterLayout->addWidget(excludeExtensionsEdit);

    QLabel* formatFilterHint = new QLabel(tr("Applied to every download path (manual Add URL, copper://, the API, and the browser extension). The Include list allows only the formats listed; the Exclude list always blocks. A URL with no file extension is always allowed."));
    formatFilterHint->setWordWrap(true);
    formatFilterHint->setStyleSheet("color: gray; font-size: 11px;");
    formatFilterLayout->addWidget(formatFilterHint);

    formatsLayout->addWidget(formatFilterGroup);
    formatsLayout->addStretch();
    tabWidget->addTab(formatsTab, tr("Formats"));

    // Tools Tab
    QWidget* toolsTab = new QWidget();
    QVBoxLayout* toolsLayout = new QVBoxLayout(toolsTab);

    QGroupBox* ytdlpGroup = new QGroupBox("yt-dlp");
    QVBoxLayout* ytdlpLayout = new QVBoxLayout(ytdlpGroup);
    ytdlpVersionLabel = new QLabel(tr("Status: checking..."));
    ytdlpLayout->addWidget(ytdlpVersionLabel);
    QLabel* ytdlpLicenseLabel = new QLabel(tr("License: Unlicense (Public Domain)\nCopper downloads yt-dlp separately. It is not bundled."));
    ytdlpLicenseLabel->setWordWrap(true);
    ytdlpLicenseLabel->setStyleSheet("color: gray; font-size: 11px;");
    ytdlpLayout->addWidget(ytdlpLicenseLabel);
    QPushButton* updateYtDlpBtn = new QPushButton(tr("Check & Update yt-dlp"));
    connect(updateYtDlpBtn, &QPushButton::clicked, this, &SettingsDialog::onUpdateYtDlp);
    ytdlpLayout->addWidget(updateYtDlpBtn);
    toolsLayout->addWidget(ytdlpGroup);

    if (YtDlpManager::instance().isInstalled()) {
        ytdlpVersionLabel->setText(tr("Version: %1").arg(YtDlpManager::instance().getVersion()));
    } else {
        ytdlpVersionLabel->setText(tr("Status: Not installed"));
    }

    connect(&YtDlpManager::instance(), &YtDlpManager::installationProgress, this, [this](const QString& status) {
        if (ytdlpVersionLabel) ytdlpVersionLabel->setText(tr("Status: %1").arg(status));
    });
    connect(&YtDlpManager::instance(), &YtDlpManager::errorOccurred, this, [this](const QString& error) {
        if (ytdlpVersionLabel) ytdlpVersionLabel->setText(tr("Error: %1").arg(error));
    });

    QGroupBox* ffmpegGroup = new QGroupBox("ffmpeg");
    QVBoxLayout* ffmpegLayout = new QVBoxLayout(ffmpegGroup);
    ffmpegVersionLabel = new QLabel(tr("Status: checking..."));
    ffmpegLayout->addWidget(ffmpegVersionLabel);
    QLabel* ffmpegLicenseLabel = new QLabel(tr("License: GPL v2+ (BtbN build)\nCopper downloads ffmpeg separately. It is not bundled."));
    ffmpegLicenseLabel->setWordWrap(true);
    ffmpegLicenseLabel->setStyleSheet("color: gray; font-size: 11px;");
    ffmpegLayout->addWidget(ffmpegLicenseLabel);
    QPushButton* updateFfmpegBtn = new QPushButton(tr("Check & Update ffmpeg"));
    connect(updateFfmpegBtn, &QPushButton::clicked, this, &SettingsDialog::onUpdateFfmpeg);
    ffmpegLayout->addWidget(updateFfmpegBtn);
    toolsLayout->addWidget(ffmpegGroup);

    if (FfmpegManager::instance().isInstalled()) {
        ffmpegVersionLabel->setText(tr("Version: %1").arg(FfmpegManager::instance().getVersion()));
    } else {
        ffmpegVersionLabel->setText(tr("Status: Not installed"));
    }

    connect(&FfmpegManager::instance(), &FfmpegManager::installationProgress, this, [this](const QString& status) {
        if (ffmpegVersionLabel) ffmpegVersionLabel->setText(tr("Status: %1").arg(status));
    });
    connect(&FfmpegManager::instance(), &FfmpegManager::errorOccurred, this, [this](const QString& error) {
        if (ffmpegVersionLabel) ffmpegVersionLabel->setText(tr("Error: %1").arg(error));
    });

    QGroupBox* aria2cGroup = new QGroupBox(tr("aria2c (Torrent/Magnet Support)"));
    QVBoxLayout* aria2cLayout = new QVBoxLayout(aria2cGroup);
    aria2cVersionLabel = new QLabel(tr("Status: checking..."));
    aria2cLayout->addWidget(aria2cVersionLabel);
    QLabel* aria2cLicenseLabel = new QLabel(tr("License: GPL v2\nCopper downloads aria2c separately. It is not bundled."));
    aria2cLicenseLabel->setWordWrap(true);
    aria2cLicenseLabel->setStyleSheet("color: gray; font-size: 11px;");
    aria2cLayout->addWidget(aria2cLicenseLabel);
    QPushButton* updateAria2cBtn = new QPushButton(tr("Check & Update aria2c"));
    connect(updateAria2cBtn, &QPushButton::clicked, this, &SettingsDialog::onUpdateAria2c);
    aria2cLayout->addWidget(updateAria2cBtn);

    QHBoxLayout* seedTimeRow = new QHBoxLayout();
    seedTimeRow->addWidget(new QLabel(tr("Default seed time:")));
    seedTimeCombo = new QComboBox();
    seedTimeCombo->addItem(tr("No seeding"), -1);
    seedTimeCombo->addItem(tr("30 minutes"), 30);
    seedTimeCombo->addItem(tr("1 hour"), 60);
    seedTimeCombo->addItem(tr("2 hours"), 120);
    seedTimeCombo->addItem(tr("Seed forever"), 0);
    int curSeed = DatabaseManager::instance().getSetting("seedTime", "30").toInt();
    int seedIdx = seedTimeCombo->findData(curSeed);
    if (seedIdx >= 0) seedTimeCombo->setCurrentIndex(seedIdx);
    seedTimeRow->addWidget(seedTimeCombo);
    aria2cLayout->addLayout(seedTimeRow);

    toolsLayout->addWidget(aria2cGroup);

    if (Aria2cManager::instance().isInstalled()) {
        aria2cVersionLabel->setText(tr("Version: %1").arg(Aria2cManager::instance().getVersion()));
    } else {
        aria2cVersionLabel->setText(tr("Status: Not installed"));
    }

    connect(&Aria2cManager::instance(), &Aria2cManager::installationProgress, this, [this](const QString& status) {
        if (aria2cVersionLabel) aria2cVersionLabel->setText(tr("Status: %1").arg(status));
    });
    connect(&Aria2cManager::instance(), &Aria2cManager::errorOccurred, this, [this](const QString& error) {
        if (aria2cVersionLabel) aria2cVersionLabel->setText(tr("Error: %1").arg(error));
    });

    toolsLayout->addStretch();
    tabWidget->addTab(toolsTab, tr("Tools"));

    // Licenses Tab
    QWidget* licensesTab = new QWidget();
    QVBoxLayout* licensesLayout = new QVBoxLayout(licensesTab);
    QPlainTextEdit* licensesText = new QPlainTextEdit();
    licensesText->setReadOnly(true);
    QString licenseVersionLine = "COPPER DOWNLOAD MANAGER v" + QCoreApplication::applicationVersion();
    licensesText->setPlainText(
        licenseVersionLine + "\n"
        "Third-Party Software Notices\n\n"
        "This product includes software developed by third parties.\n\n"
        "--------------------------------------------------\n"
        "Qt " + QString(qVersion()) + "\n"
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
    tabWidget->addTab(licensesTab, tr("Licenses"));

    // System Tab
    QWidget* systemTab = new QWidget();
    QVBoxLayout* systemLayout = new QVBoxLayout(systemTab);

    QGroupBox* updateGroup = new QGroupBox(tr("Updates"));
    QVBoxLayout* updateBoxLayout = new QVBoxLayout(updateGroup);
    QLabel* updateInfoLabel = new QLabel(tr("Current version: %1").arg(QCoreApplication::applicationVersion()));
    updateBoxLayout->addWidget(updateInfoLabel);
    QPushButton* checkUpdateBtn = new QPushButton(tr("Check for Updates..."));
    connect(checkUpdateBtn, &QPushButton::clicked, this, &SettingsDialog::onCheckForUpdates);
    connect(&UpdateManager::instance(), &UpdateManager::updateAvailable, this, &SettingsDialog::onUpdateReady);
    connect(&UpdateManager::instance(), &UpdateManager::noUpdateAvailable, this, [this]() {
        QMessageBox::information(this, tr("No Updates"), tr("You are running the latest version of Copper Download Manager."));
    });
    connect(&UpdateManager::instance(), &UpdateManager::downloadFinished, this, &SettingsDialog::onUpdateDownloaded);
    connect(&UpdateManager::instance(), &UpdateManager::downloadFailed, this, &SettingsDialog::onUpdateMessage);
    connect(&UpdateManager::instance(), &UpdateManager::errorOccurred, this, &SettingsDialog::onUpdateMessage);
    updateBoxLayout->addWidget(checkUpdateBtn);
    systemLayout->addWidget(updateGroup);

    QGroupBox* handlerGroup = new QGroupBox(tr("Default Downloader"));
    QVBoxLayout* handlerBoxLayout = new QVBoxLayout(handlerGroup);
    handlerStatusLabel = new QLabel(DefaultHandler::instance().isRegistered() ? tr("Registered as default downloader") : tr("Not registered"));
    handlerBoxLayout->addWidget(handlerStatusLabel);
    QHBoxLayout* handlerBtnLayout = new QHBoxLayout();
    QPushButton* registerBtn = new QPushButton(tr("Register as Default Downloader"));
    connect(registerBtn, &QPushButton::clicked, this, &SettingsDialog::onRegisterDefaultHandler);
    QPushButton* unregisterBtn = new QPushButton(tr("Unregister"));
    connect(unregisterBtn, &QPushButton::clicked, this, &SettingsDialog::onUnregisterDefaultHandler);
    handlerBtnLayout->addWidget(registerBtn);
    handlerBtnLayout->addWidget(unregisterBtn);
    handlerBoxLayout->addLayout(handlerBtnLayout);
    systemLayout->addWidget(handlerGroup);

    QGroupBox* startupGroup = new QGroupBox(tr("Startup"));
    QVBoxLayout* startupBoxLayout = new QVBoxLayout(startupGroup);
    QCheckBox* startupCheck = new QCheckBox(tr("Start with system"));
    startupCheck->setChecked(DatabaseManager::instance().getSetting("startup", "true") == "true");
    connect(startupCheck, &QCheckBox::toggled, this, [this](bool checked) {
        DatabaseManager::instance().saveSetting("startup", checked ? "true" : "false");
        updateStartupRegistry(checked);
    });
    startupBoxLayout->addWidget(startupCheck);

    QCheckBox* minimizeToTrayCheck = new QCheckBox(tr("Minimize to system tray"));
    minimizeToTrayCheck->setChecked(DatabaseManager::instance().getSetting("minimizeToTray", "true") == "true");
    connect(minimizeToTrayCheck, &QCheckBox::toggled, this, [](bool checked) {
        DatabaseManager::instance().saveSetting("minimizeToTray", checked ? "true" : "false");
    });
    startupBoxLayout->addWidget(minimizeToTrayCheck);
    systemLayout->addWidget(startupGroup);

    QGroupBox* statsGroup = new QGroupBox(tr("Statistics"));
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
    statsBoxLayout->addWidget(new QLabel(tr("Total downloads: %1").arg(totalDownloads)));
    statsBoxLayout->addWidget(new QLabel(tr("Active downloads: %1").arg(activeDownloads)));
    statsBoxLayout->addWidget(new QLabel(tr("Completed downloads: %1").arg(completedDownloads)));
    systemLayout->addWidget(statsGroup);

    systemLayout->addStretch();
    tabWidget->addTab(systemTab, tr("System"));

    mainLayout->addWidget(tabWidget);

    QHBoxLayout* bottomLayout = new QHBoxLayout();
    QPushButton* saveBtn = new QPushButton(tr("Save"));
    connect(saveBtn, &QPushButton::clicked, this, &SettingsDialog::onSave);
    QPushButton* closeBtn = new QPushButton(tr("Close"));
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
    QString dir = QFileDialog::getExistingDirectory(this, tr("Select Download Folder"), downloadPathEdit->text());
    if (!dir.isEmpty()) {
        downloadPathEdit->setText(dir);
    }
}

void SettingsDialog::onUpdateYtDlp() {
    Logger::instance().info("Updating yt-dlp...");
    ytdlpVersionLabel->setText(tr("Status: Downloading..."));
    YtDlpManager::instance().installOrUpdate();
}

void SettingsDialog::onUpdateFfmpeg() {
    Logger::instance().info("Updating ffmpeg...");
    ffmpegVersionLabel->setText(tr("Status: Downloading..."));
    FfmpegManager::instance().installOrUpdate();
}

void SettingsDialog::onUpdateAria2c() {
    Logger::instance().info("Updating aria2c...");
    aria2cVersionLabel->setText(tr("Status: Downloading..."));
    Aria2cManager::instance().installOrUpdate();
}

void SettingsDialog::onCheckForUpdates() {
    Logger::instance().info("Checking for updates...");
    UpdateManager::instance().checkForUpdates(false);
}

void SettingsDialog::onUpdateReady(const QString& version) {
    QString msg = tr("A new version of Copper Download Manager is available:\n\n"
                     "Current version: %1\n"
                     "Latest version: %2\n\n"
                     "Do you want to download and install it now?")
                      .arg(QCoreApplication::applicationVersion(), version);
    QMessageBox::StandardButton reply = QMessageBox::question(this, tr("Update Available"), msg,
        QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        UpdateManager::instance().downloadAndInstall();
    }
}

void SettingsDialog::onUpdateDownloaded() {
    QString installerPath = UpdateManager::instance().installerPathOrEmpty();
    QMessageBox::StandardButton reply = QMessageBox::question(this, tr("Update Downloaded"),
        tr("The update has been downloaded. Restart Copper Download Manager to install the update?"),
        QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        qApp->quit();
        QProcess::startDetached(installerPath);
    }
}

void SettingsDialog::onUpdateMessage(const QString& error) {
    QMessageBox::warning(this, tr("Update"), error);
}

void SettingsDialog::onRegisterDefaultHandler() {
    DefaultHandler::instance().registerAsDefault();
    handlerStatusLabel->setText(tr("Registered as default downloader"));
    QMessageBox::information(this, tr("Register as Default Downloader"),
        tr("Copper Download Manager has been registered as a capable handler for:\n"
        "Magnet and copper:// protocols, plus .torrent files.\n\n"
        "It is intentionally NOT registered for HTTP, HTTPS, or FTP:\n"
        "- http/https links stay with your browser\n"
        "- ftp stays with your dedicated FTP client\n\n"
        "Windows Default Apps settings will now open so you can choose Copper\n"
        "for magnet links and .torrent files if you wish."));
}

void SettingsDialog::onUnregisterDefaultHandler() {
    DefaultHandler::instance().unregisterAsDefault();
    handlerStatusLabel->setText(tr("Not registered"));
    QMessageBox::information(this, tr("Success"), tr("Copper Download Manager has been unregistered."));
}

void SettingsDialog::accept() {
    onSave();
    QDialog::accept();
}

void SettingsDialog::onSave() {
    DatabaseManager::instance().saveSetting("downloadPath", downloadPathEdit->text());
    DatabaseManager::instance().saveSetting("chunks", chunkCombo->currentText());
    DatabaseManager::instance().saveSetting("speedLimit", QString::number(speedLimitSpin->value()));
    DatabaseManager::instance().saveSetting("maxConcurrentDownloads", QString::number(maxConcurrentSpin->value()));
    DatabaseManager::instance().saveSetting("maxYtDlpJobs", QString::number(maxYtDlpJobsSpin->value()));
    DatabaseManager::instance().saveSetting("ytDlpFragments", QString::number(ytDlpFragmentsSpin->value()));
    DatabaseManager::instance().saveSetting("maxRetries", QString::number(maxRetriesSpin->value()));
    DatabaseManager::instance().saveSetting("autoOpenDownloadInfo", autoOpenInfoCheck->isChecked() ? "true" : "false");

    DownloadManager::instance().updateMaxConcurrent(maxConcurrentSpin->value());
    DatabaseManager::instance().saveSetting("seedTime", QString::number(seedTimeCombo->currentData().toInt()));
    DatabaseManager::instance().saveSetting("userAgent", userAgentEdit->text().trimmed());
    DatabaseManager::instance().saveSetting("customHeaders", headersEdit->toPlainText());

    DatabaseManager::instance().saveSetting("formatFilterEnabled", formatFilterEnabledCheck->isChecked() ? "true" : "false");
    DatabaseManager::instance().saveSetting("formatIncludeExtensions", includeExtensionsEdit->toPlainText());
    DatabaseManager::instance().saveSetting("formatExcludeExtensions", excludeExtensionsEdit->toPlainText());

    qint64 speedLimit = (qint64)speedLimitSpin->value() * 1024;
    DownloadManager::instance().setSpeedLimit(speedLimit);

    Logger::instance().info("Settings saved");
}

void SettingsDialog::updateStartupRegistry(bool enabled) {
#ifdef PLATFORM_WINDOWS
    QSettings runKey("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    if (enabled) {
        QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        runKey.setValue("CopperDownloadManager", "\"" + exePath + "\" --minimized");
        Logger::instance().info("Startup autostart enabled");
    } else {
        runKey.remove("CopperDownloadManager");
        Logger::instance().info("Startup autostart disabled");
    }
#endif
}
