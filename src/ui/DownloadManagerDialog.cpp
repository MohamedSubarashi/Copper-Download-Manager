#include "ui/DownloadManagerDialog.h"
#include "utils/Aria2cManager.h"
#include "utils/YtDlpManager.h"
#include "utils/Logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QProgressBar>
#include <QFont>
#include <QGroupBox>
#include <QTextBrowser>

DownloadManagerDialog::DownloadManagerDialog(SourceType sourceType, const QString& url, const QString& path, QWidget* parent)
    : QDialog(parent), sourceType(sourceType), url(url), defaultPath(path) {
    setWindowTitle("Download Manager");
    setWindowIcon(QIcon(":/icons/CurrentDownload.png"));
    resize(650, 620);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);

    QString title;
    if (sourceType == SourceVideo) title = "Video / Playlist Download";
    else if (sourceType == SourceTorrent) title = "Torrent Download";
    else title = "File Download";

    QLabel* titleLabel = new QLabel(title, this);
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(12);
    titleLabel->setFont(titleFont);
    mainLayout->addWidget(titleLabel);

    torrentInfoGroup = new QGroupBox("Torrent Information", this);
    torrentInfoGroup->setVisible(false);
    QVBoxLayout* infoLayout = new QVBoxLayout(torrentInfoGroup);
    infoLayout->setSpacing(4);

    torrentNameLabel = new QLabel(torrentInfoGroup);
    torrentNameLabel->setWordWrap(true);
    QFont nameFont = torrentNameLabel->font();
    nameFont.setBold(true);
    torrentNameLabel->setFont(nameFont);
    infoLayout->addWidget(torrentNameLabel);

    torrentSizeLabel = new QLabel(torrentInfoGroup);
    infoLayout->addWidget(torrentSizeLabel);

    torrentFilesLabel = new QLabel(torrentInfoGroup);
    infoLayout->addWidget(torrentFilesLabel);

    torrentTrackersLabel = new QLabel(torrentInfoGroup);
    torrentTrackersLabel->setWordWrap(true);
    torrentTrackersLabel->setStyleSheet("color: #888;");
    infoLayout->addWidget(torrentTrackersLabel);

    mainLayout->addWidget(torrentInfoGroup);

    statusLabel = new QLabel("Fetching file list...", this);
    mainLayout->addWidget(statusLabel);

    progressBar = new QProgressBar(this);
    progressBar->setVisible(false);
    mainLayout->addWidget(progressBar);

    fileList = new QListWidget();
    connect(fileList, &QListWidget::itemChanged, this, &DownloadManagerDialog::onItemChanged);
    mainLayout->addWidget(fileList);

    QHBoxLayout* optionsLayout = new QHBoxLayout();
    selectAllCheck = new QCheckBox("Select All");
    selectAllCheck->setChecked(true);
    connect(selectAllCheck, &QCheckBox::toggled, this, &DownloadManagerDialog::onSelectAll);
    optionsLayout->addWidget(selectAllCheck);

    optionsLayout->addSpacing(20);

    trackNumberCheck = new QCheckBox("Add track numbers (001, 002...)");
    trackNumberCheck->setToolTip("Prepend track numbers to filenames.\n"
                                 "Format: 001.VideoTitle.mp4 (for <1000 files), 0001.VideoTitle.mp4 (for 1000+)");
    trackNumberCheck->setChecked(true);
    optionsLayout->addWidget(trackNumberCheck);
    optionsLayout->addStretch();
    mainLayout->addLayout(optionsLayout);

    QHBoxLayout* pathLayout = new QHBoxLayout();
    pathLayout->addWidget(new QLabel("Save to:"));
    pathEdit = new QLineEdit();
    pathEdit->setText(path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) : path);
    QPushButton* browseBtn = new QPushButton("Browse...");
    connect(browseBtn, &QPushButton::clicked, this, &DownloadManagerDialog::onBrowse);
    pathLayout->addWidget(pathEdit);
    pathLayout->addWidget(browseBtn);
    mainLayout->addLayout(pathLayout);

    QHBoxLayout* btnLayout = new QHBoxLayout();
    fetchBtn = new QPushButton("Fetch Files");
    connect(fetchBtn, &QPushButton::clicked, this, &DownloadManagerDialog::onFetchFiles);
    downloadBtn = new QPushButton("Download");
    downloadBtn->setEnabled(false);
    downloadBtn->setDefault(true);
    connect(downloadBtn, &QPushButton::clicked, this, &DownloadManagerDialog::onDownload);
    QPushButton* cancelBtn = new QPushButton("Cancel");
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(fetchBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(downloadBtn);
    btnLayout->addWidget(cancelBtn);
    mainLayout->addLayout(btnLayout);

    if (sourceType == SourceTorrent) {
        fetchFiles();
    } else if (sourceType == SourceVideo) {
        fetchFiles();
    } else {
        statusLabel->setText("Ready to download");
        downloadBtn->setEnabled(true);
    }
}

void DownloadManagerDialog::setAudioFormat(const QString& format) {
    audioFormat = format;
}

bool DownloadManagerDialog::getUseTrackNumbers() const {
    return trackNumberCheck->isChecked();
}

QString DownloadManagerDialog::getAudioFormat() const {
    return audioFormat;
}

void DownloadManagerDialog::fetchFiles() {
    statusLabel->setText("Fetching file list...");
    fetchBtn->setEnabled(false);
    progressBar->setVisible(true);
    progressBar->setRange(0, 0);

    if (sourceType == SourceTorrent) {
        if (!Aria2cManager::instance().isInstalled()) {
            statusLabel->setText("aria2c not found. Downloading and installing aria2c... (this may take a moment)");
        }
        Aria2cManager::instance().fetchTorrentFiles(url, [this](const QVector<PlaylistEntry>& fetchedEntries, const TorrentInfo& info) {
            entries = fetchedEntries;
            torrentInfo = info;
            showFileList(entries);
            showTorrentInfo(info);
            statusLabel->setText("Found " + QString::number(entries.size()) + " file(s)");
            downloadBtn->setEnabled(!entries.isEmpty());
            fetchBtn->setEnabled(true);
            progressBar->setVisible(false);
        });
    } else if (sourceType == SourceVideo) {
        YtDlpManager::instance().fetchPlaylistInfo(url, [this](const QVector<PlaylistEntry>& fetchedEntries) {
            entries = fetchedEntries;
            showFileList(entries);
            statusLabel->setText("Found " + QString::number(entries.size()) + " video(s)");
            downloadBtn->setEnabled(!entries.isEmpty());
            fetchBtn->setEnabled(true);
            progressBar->setVisible(false);
        });
    }
}

void DownloadManagerDialog::showFileList(const QVector<PlaylistEntry>& entries) {
    fileList->clear();
    bool useTracks = trackNumberCheck->isChecked();
    int total = entries.size();
    bool allSelected = !entries.isEmpty();
    for (const PlaylistEntry& entry : entries) {
        if (!entry.selected) {
            allSelected = false;
            break;
        }
    }
    selectAllCheck->setChecked(allSelected);

    for (const PlaylistEntry& entry : entries) {
        QString text;
        if (useTracks) {
            int width = (total < 1000) ? 3 : 4;
            text = QString("%1.").arg(entry.index, width, 10, QChar('0'));
        } else {
            text = QString::number(entry.index) + ".";
        }
        text += " " + entry.title;
        if (!entry.fileSize.isEmpty() && entry.fileSize != "Unknown") {
            text += " (" + entry.fileSize + ")";
        }
        QListWidgetItem* item = new QListWidgetItem(text);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(entry.selected ? Qt::Checked : Qt::Unchecked);
        fileList->addItem(item);
    }
}

void DownloadManagerDialog::onFetchFiles() {
    fetchFiles();
}

void DownloadManagerDialog::onBrowse() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select Folder", pathEdit->text());
    if (!dir.isEmpty()) {
        pathEdit->setText(dir);
    }
}

void DownloadManagerDialog::onDownload() {
    for (int i = 0; i < fileList->count(); ++i) {
        QListWidgetItem* item = fileList->item(i);
        if (!item || i >= entries.size()) {
            continue;
        }
        entries[i].selected = (item->checkState() == Qt::Checked);
    }
    accept();
}

void DownloadManagerDialog::onSelectAll(bool checked) {
    for (int i = 0; i < fileList->count(); i++) {
        fileList->item(i)->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
}

void DownloadManagerDialog::onItemChanged(QListWidgetItem* item) {
    int idx = fileList->row(item);
    if (idx >= 0 && idx < entries.size()) {
        entries[idx].selected = (item->checkState() == Qt::Checked);
    }
}

QVector<PlaylistEntry> DownloadManagerDialog::getSelectedEntries() const {
    QVector<PlaylistEntry> selected;
    for (int i = 0; i < entries.size(); i++) {
        if (i < fileList->count() && fileList->item(i)->checkState() == Qt::Checked) {
            selected.append(entries[i]);
        }
    }
    return selected;
}

QString DownloadManagerDialog::getOutputPath() const {
    return pathEdit->text();
}

QString DownloadManagerDialog::getTorrentName() const {
    return torrentInfo.name;
}

void DownloadManagerDialog::showTorrentInfo(const TorrentInfo& info) {
    if (info.name.isEmpty() && info.trackers.isEmpty() && info.totalSize.isEmpty()) {
        return;
    }

    torrentInfoGroup->setVisible(true);

    if (!info.name.isEmpty()) {
        torrentNameLabel->setText(info.name);
    }

    QStringList sizeInfo;
    if (!info.totalSize.isEmpty()) {
        sizeInfo << "Size: " + info.totalSize;
    }
    if (info.fileCount > 0) {
        sizeInfo << "Files: " + QString::number(info.fileCount);
    }
    if (info.numberOfPieces > 0) {
        sizeInfo << "Pieces: " + QString::number(info.numberOfPieces);
    }
    torrentSizeLabel->setText(sizeInfo.join("  |  "));

    QStringList trackerInfo;
    if (!info.infoHash.isEmpty()) {
        trackerInfo << "Hash: " + info.infoHash;
    }
    if (!info.trackers.isEmpty()) {
        trackerInfo << "Trackers: " + QString::number(info.trackers.size());
        QStringList trackerDisplay;
        for (const QString& t : info.trackers) {
            QString display = t;
            if (display.length() > 60) display = display.left(57) + "...";
            trackerDisplay << "  " + display;
        }
        trackerInfo << trackerDisplay.join("\n");
    }
    torrentTrackersLabel->setText(trackerInfo.join("\n"));
}
