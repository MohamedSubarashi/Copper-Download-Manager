#include "ui/AddUrlDialog.h"
#include "core/DownloadManager.h"
#include "ui/DownloadManagerDialog.h"
#include "utils/UrlDetector.h"
#include "utils/Logger.h"
#include "db/DatabaseManager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QFileDialog>
#include <QClipboard>
#include <QApplication>
#include <QStandardPaths>
#include <QMessageBox>

AddUrlDialog::AddUrlDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Add URL"));
    setWindowIcon(QIcon(":/icons/AddUrl.png"));
    resize(520, 260);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);

    QHBoxLayout* urlLayout = new QHBoxLayout();
    QLabel* urlLabel = new QLabel(tr("URL:"));
    urlLayout->addWidget(urlLabel);
    urlEdit = new QLineEdit();
    urlEdit->setPlaceholderText(tr("Enter download URL or paste from clipboard..."));
    urlEdit->setAccessibleName(tr("URL"));
    urlLabel->setBuddy(urlEdit);
    urlLayout->addWidget(urlEdit);
    mainLayout->addLayout(urlLayout);

    QHBoxLayout* typeLayout = new QHBoxLayout();
    QLabel* typeLabel = new QLabel(tr("Type:"));
    typeLayout->addWidget(typeLabel);
    typeCombo = new QComboBox();
    typeCombo->setAccessibleName(tr("Type"));
    typeLabel->setBuddy(typeCombo);
    typeCombo->addItem(tr("Auto Detect"));
    typeCombo->addItem(tr("Direct Download"));
    typeCombo->addItem(tr("yt-dlp Video"));
    typeCombo->addItem(tr("Torrent / Magnet"));
    connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &AddUrlDialog::onTypeChanged);
    typeLayout->addWidget(typeCombo);
    typeInfoLabel = new QLabel("");
    typeInfoLabel->setStyleSheet("color: #888; font-style: italic;");
    typeLayout->addWidget(typeInfoLabel);
    mainLayout->addLayout(typeLayout);

    QHBoxLayout* formatLayout = new QHBoxLayout();
    formatLayout->addWidget(new QLabel(tr("Output format:")));
    formatCombo = new QComboBox();
    formatCombo->addItem(tr("MP4 (Video)"));
    formatCombo->addItem(tr("MP3 (Audio only)"));
    formatCombo->addItem(tr("MKV (Video)"));
    formatCombo->addItem(tr("Best Quality"));
    formatLayout->addWidget(formatCombo);
    formatLayout->addStretch();
    mainLayout->addLayout(formatLayout);

    QHBoxLayout* pathLayout = new QHBoxLayout();
    QLabel* pathLabel = new QLabel(tr("Save to:"));
    pathLayout->addWidget(pathLabel);
    pathEdit = new QLineEdit();
    pathEdit->setAccessibleName(tr("Save to"));
    pathLabel->setBuddy(pathEdit);
    pathEdit->setText(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    QPushButton* browseBtn = new QPushButton(tr("Browse..."));
    connect(browseBtn, &QPushButton::clicked, this, &AddUrlDialog::onBrowse);
    pathLayout->addWidget(pathEdit);
    pathLayout->addWidget(browseBtn);
    mainLayout->addLayout(pathLayout);

    statusLabel = new QLabel("");
    statusLabel->setWordWrap(true);
    mainLayout->addWidget(statusLabel);

    QHBoxLayout* btnLayout = new QHBoxLayout();
    QPushButton* pasteBtn = new QPushButton(tr("Paste"));
    pasteBtn->setFixedWidth(80);
    connect(pasteBtn, &QPushButton::clicked, this, &AddUrlDialog::onPaste);
    addBtn = new QPushButton(tr("Add Download"));
    addBtn->setEnabled(false);
    addBtn->setDefault(true);
    connect(addBtn, &QPushButton::clicked, this, &AddUrlDialog::onAdd);
    QPushButton* cancelBtn = new QPushButton(tr("Cancel"));
    cancelBtn->setFixedWidth(80);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(pasteBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(addBtn);
    btnLayout->addWidget(cancelBtn);
    mainLayout->addLayout(btnLayout);

    connect(urlEdit, &QLineEdit::textChanged, this, &AddUrlDialog::validateUrl);

    QString clipboard = QApplication::clipboard()->text();
    if (!clipboard.isEmpty() && (clipboard.startsWith("http") || clipboard.startsWith("magnet") || clipboard.startsWith("ftp"))) {
        urlEdit->setText(clipboard);
        statusLabel->setText(tr("Pasted from clipboard"));
    }
    urlEdit->setFocus();
}

void AddUrlDialog::setUrl(const QString& url) {
    urlEdit->setText(url);
}

void AddUrlDialog::onAdd() {
    QString url = urlEdit->text().trimmed();
    if (url.isEmpty()) return;

    int typeIndex = typeCombo->currentIndex();
    QString path = pathEdit->text();

    int fmtIndex = formatCombo->currentIndex();
    QString audioFormat;
    if (fmtIndex == 0) audioFormat = "mp4";
    else if (fmtIndex == 1) audioFormat = "mp3";
    else if (fmtIndex == 2) audioFormat = "mkv";
    else if (fmtIndex == 3) audioFormat = "best";

    if (typeIndex == 3 || UrlDetector::isTorrentUrl(url)) {
        DownloadManagerDialog dialog(SourceTorrent, url, path, this);
        dialog.setAudioFormat(audioFormat);
        if (dialog.exec() == QDialog::Accepted) {
            QVector<PlaylistEntry> selected = dialog.getSelectedEntries();
            QString outputPath = dialog.getOutputPath();
            bool useTracks = dialog.getUseTrackNumbers();
            QString fmt = dialog.getAudioFormat();
            QString torrentName = dialog.getTorrentName();
            QString source = dialog.getTorrentSource();
            if (fmt.isEmpty()) fmt = audioFormat;
            if (!selected.isEmpty()) {
                DownloadManager::instance().addPlaylistDownload(selected, outputPath, "Torrent", useTracks, fmt, source, torrentName);
            } else {
                QString savePath = path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) : path;
                DownloadManager::instance().addDownload(source, savePath, "Torrent");
            }
        }
        accept();
        return;
    }

    if (typeIndex == 2 || UrlDetector::isYtDlpUrl(url)) {
        DownloadManagerDialog dialog(SourceVideo, url, path, this);
        dialog.setAudioFormat(audioFormat);
        if (dialog.exec() == QDialog::Accepted) {
            QVector<PlaylistEntry> selected = dialog.getSelectedEntries();
            QString outputPath = dialog.getOutputPath();
            bool useTracks = dialog.getUseTrackNumbers();
            QString fmt = dialog.getAudioFormat();
            if (fmt.isEmpty()) fmt = audioFormat;
            if (!selected.isEmpty()) {
                DownloadManager::instance().addPlaylistDownload(selected, outputPath, "YtDlp", useTracks, fmt, "", "", url);
            } else {
                QString savePath = path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) : path;
                int id = DownloadManager::instance().addDownload(url, savePath, "YtDlp", 16,
                                                                  fmt.isEmpty() ? QStringLiteral("mp4") : fmt);
                if (id < 0) {
                    statusLabel->setText(tr("Blocked by file-format filter (Settings > Downloads)"));
                    return;
                }
            }
        }
        accept();
        return;
    }

    if (typeIndex == 1 || typeIndex == 0) {
        UrlType detected = UrlDetector::detect(url);
        if (detected == UrlPlaylist) {
            DownloadManagerDialog dialog(SourceVideo, url, path, this);
            dialog.setAudioFormat(audioFormat);
            if (dialog.exec() == QDialog::Accepted) {
                QVector<PlaylistEntry> selected = dialog.getSelectedEntries();
                QString outputPath = dialog.getOutputPath();
                bool useTracks = dialog.getUseTrackNumbers();
                QString fmt = dialog.getAudioFormat();
                if (fmt.isEmpty()) fmt = audioFormat;
                if (!selected.isEmpty()) {
                    DownloadManager::instance().addPlaylistDownload(selected, outputPath, "YtDlp", useTracks, fmt, "", "", url);
                } else {
                    QString savePath = path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) : path;
                    DownloadManager::instance().addDownload(url, savePath, "YtDlp", 16,
                                                            fmt.isEmpty() ? QStringLiteral("mp4") : fmt);
                }
            }
        } else {
            QString savePath = path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) : path;
            int id = DownloadManager::instance().addDownload(url, savePath, "HTTP");
            if (id < 0) {
                statusLabel->setText(tr("Blocked by file-format filter (Settings > Downloads)"));
                return;
            }
        }
        accept();
    }
}

void AddUrlDialog::onPaste() {
    QString clip = QApplication::clipboard()->text();
    urlEdit->setText(clip);
    statusLabel->setText(clip.isEmpty() ? tr("Clipboard is empty") : tr("Pasted from clipboard"));
}

void AddUrlDialog::onTypeChanged(int index) {
    Q_UNUSED(index);
    validateUrl();
}

void AddUrlDialog::onBrowse() {
    QString dir = QFileDialog::getExistingDirectory(this, tr("Select Folder"), pathEdit->text());
    if (!dir.isEmpty()) {
        pathEdit->setText(dir);
    }
}

void AddUrlDialog::validateUrl() {
    QString url = urlEdit->text().trimmed();
    bool valid = !url.isEmpty() && (url.startsWith("http") || url.startsWith("https") || url.startsWith("ftp") || url.startsWith("magnet"));
    addBtn->setEnabled(valid);

    if (valid) {
        statusLabel->clear();
        UrlType type = UrlDetector::detect(url);
        typeInfoLabel->setText(tr("Detected: %1").arg(UrlDetector::typeToString(type)));
    } else {
        typeInfoLabel->setText("");
        if (url.isEmpty()) {
            statusLabel->clear();
        } else {
            statusLabel->setText(tr("Please enter a valid URL (http://, https://, ftp://, or magnet:?)"));
        }
    }
}
