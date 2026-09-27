#include "ui/DownloadInfoDialog.h"
#include "core/DownloadManager.h"
#include "core/DownloadItem.h"
#include "db/DatabaseManager.h"
#include "utils/Logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTreeWidget>
#include <QHeaderView>
#include <QTimer>
#include <QDesktopServices>
#include <QFileInfo>
#include <QClipboard>
#include <QApplication>
#include <QCloseEvent>
#include <QShowEvent>
#include <QStyle>

namespace {

QString formatSize(qint64 bytes) {
    if (bytes <= 0) return "0 B";
    const QStringList units = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    double size = bytes;
    while (size >= 1024.0 && unitIndex < units.size() - 1) {
        size /= 1024.0;
        unitIndex++;
    }
    return QString::number(size, 'f', unitIndex == 0 ? 0 : 2) + " " + units[unitIndex];
}

QString formatSpeed(qint64 bytesPerSec) {
    if (bytesPerSec <= 0) return "--";
    return formatSize(bytesPerSec) + "/s";
}

QString formatDuration(qint64 seconds) {
    if (seconds <= 0) return "--";
    if (seconds < 60) return QString::number(seconds) + "s";
    if (seconds < 3600) return QString("%1m %2s").arg(seconds / 60).arg(seconds % 60);
    return QString("%1h %2m").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

QString statusText(const QString& status) {
    if (status == "Downloading") return "Downloading";
    if (status == "Queued") return "Waiting in queue";
    if (status == "Paused") return "Paused";
    if (status == "Completed") return "Completed";
    if (status == "Failed") return "Failed";
    if (status == "Cancelled") return "Cancelled";
    return status;
}

QColor statusColorFor(const QString& status, const QPalette& pal) {
    if (status == "Completed") return QColor(0, 160, 0);
    if (status == "Failed" || status == "Cancelled") return QColor(200, 0, 0);
    if (status == "Downloading") return QColor(0, 110, 190);
    if (status == "Paused") return QColor(190, 140, 0);
    return pal.color(QPalette::Text);
}

}  // namespace

DownloadInfoDialog::DownloadInfoDialog(int downloadId, QWidget* parent)
    : QDialog(parent), m_id(downloadId), m_lastStatus() {
    setWindowTitle("Download Info");
    setAttribute(Qt::WA_DeleteOnClose, false);
    setModal(false);
    buildUi();

    // A periodic refresh keeps numbers moving while the transfer runs. The window
    // is hidden (never destroyed) when closed, so this timer has to be stopped
    // explicitly - a hidden window must not keep the app repainting for it.
    m_timer = new QTimer(this);
    m_timer->setInterval(500);
    connect(m_timer, &QTimer::timeout, this, &DownloadInfoDialog::refresh);

    // Re-read whenever the engine reports something, so the numbers are correct
    // immediately instead of up to half a second late.
    DownloadManager& dm = DownloadManager::instance();
    connect(&dm, &DownloadManager::statusChanged, this, [this](int id, const QString&) {
        if (id == m_id) refresh();
    });
    connect(&dm, &DownloadManager::downloadProgress, this, [this](int id, qint64, qint64) {
        if (id == m_id) refresh();
    });
    connect(&dm, &DownloadManager::downloadSpeed, this, [this](int id, qint64) {
        if (id == m_id) refresh();
    });
    connect(&dm, &DownloadManager::downloadFinished, this, [this](int id) {
        if (id == m_id) refresh();
    });
    connect(&dm, &DownloadManager::downloadFailed, this, [this](int id, const QString&) {
        if (id == m_id) refresh();
    });
    connect(&dm, &DownloadManager::downloadRemoved, this, [this](int id) {
        if (id == m_id) {
            // The transfer is gone: keep the window open showing what happened
            // instead of leaving a stale panel behind.
            m_fileName->setText("(removed)");
            m_statusLabel->setText("Removed from the list");
            m_progress->setValue(0);
            updateButtons();
        }
    });

    refresh();
}

DownloadInfoDialog::~DownloadInfoDialog() {
    if (m_timer) m_timer->stop();
}

void DownloadInfoDialog::buildUi() {
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 12);
    root->setSpacing(10);

    m_fileName = new QLabel(this);
    m_fileName->setWordWrap(true);
    QFont titleFont = m_fileName->font();
    titleFont.setBold(true);
    m_fileName->setFont(titleFont);
    root->addWidget(m_fileName);

    m_statusLabel = new QLabel(this);
    root->addWidget(m_statusLabel);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(true);
    m_progress->setFormat("%p%");
    root->addWidget(m_progress);

    QFormLayout* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_sizeLabel = new QLabel(this);
    m_speedLabel = new QLabel(this);
    m_timeLabel = new QLabel(this);
    m_itemsLabel = new QLabel(this);
    m_urlLabel = new QLabel(this);
    m_urlLabel->setWordWrap(true);
    m_urlLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_urlLabel->setOpenExternalLinks(false);

    form->addRow("Size:", m_sizeLabel);
    form->addRow("Speed:", m_speedLabel);
    form->addRow("Time left:", m_timeLabel);
    form->addRow("Items:", m_itemsLabel);
    form->addRow("URL:", m_urlLabel);
    root->addLayout(form);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setVisible(false);
    m_errorLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_errorLabel);

    // Playlist folders list their items here, so one window covers the whole
    // playlist instead of forcing one window per video.
    m_itemTree = new QTreeWidget(this);
    m_itemTree->setColumnCount(4);
    m_itemTree->setHeaderLabels({"Item", "Status", "Size", "Progress"});
    m_itemTree->setRootIsDecorated(false);
    m_itemTree->setUniformRowHeights(true);
    m_itemTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_itemTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_itemTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_itemTree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_itemTree->setSelectionMode(QAbstractItemView::NoSelection);
    m_itemTree->setVisible(false);
    root->addWidget(m_itemTree);

    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addStretch();

    m_cancelButton = new QPushButton("Cancel", this);
    m_cancelButton->setToolTip("Stop this download and remove it from the list");
    connect(m_cancelButton, &QPushButton::clicked, this, &DownloadInfoDialog::onCancel);
    buttons->addWidget(m_cancelButton);

    m_retryButton = new QPushButton("Retry", this);
    m_retryButton->setToolTip("Download the failed item(s) again");
    connect(m_retryButton, &QPushButton::clicked, this, &DownloadInfoDialog::onRetry);
    buttons->addWidget(m_retryButton);

    m_pauseButton = new QPushButton("Pause", this);
    m_pauseButton->setToolTip("Pause the transfer, keeping the partial file so it can continue later");
    connect(m_pauseButton, &QPushButton::clicked, this, &DownloadInfoDialog::onPauseResume);
    buttons->addWidget(m_pauseButton);

    QPushButton* openFolderButton = new QPushButton("Open Folder", this);
    connect(openFolderButton, &QPushButton::clicked, this, &DownloadInfoDialog::onOpenFolder);
    buttons->addWidget(openFolderButton);

    QPushButton* openFileButton = new QPushButton("Open File", this);
    connect(openFileButton, &QPushButton::clicked, this, &DownloadInfoDialog::onOpenFile);
    buttons->addWidget(openFileButton);

    QPushButton* copyUrlButton = new QPushButton("Copy URL", this);
    connect(copyUrlButton, &QPushButton::clicked, this, &DownloadInfoDialog::onCopyUrl);
    buttons->addWidget(copyUrlButton);

    root->addLayout(buttons);

    resize(560, 320);
}

void DownloadInfoDialog::refresh() {
    DownloadItem item = DownloadManager::instance().getDownload(m_id);
    if (item.id == 0 && m_lastStatus != "Removed") {
        // Never existed (should not happen) - nothing useful to show.
        m_statusLabel->setText("This download no longer exists.");
        return;
    }
    if (item.id == 0) return;   // removed: keep the last known state

    m_fileName->setText(item.isFolder ? item.fileName
                                      : (item.fileName.isEmpty() ? item.url : item.fileName));

    m_statusLabel->setText(statusText(item.status));
    m_statusLabel->setStyleSheet("color: " + statusColorFor(item.status, palette()).name());

    m_progress->setValue(qBound(0, (int)item.progress, 100));

    if (item.totalSize > 0) {
        m_sizeLabel->setText(formatSize(item.downloadedSize) + " / " + formatSize(item.totalSize));
    } else {
        m_sizeLabel->setText(formatSize(item.downloadedSize) + " / --");
    }

    m_speedLabel->setText(item.status == "Downloading" ? formatSpeed(item.speed) : "--");

    // yt-dlp reports the remaining time for its transfers; other engines leave it
    // at 0, so it is derived from the current speed and the bytes still to go.
    qint64 eta = item.eta;
    if (eta <= 0 && item.speed > 0 && item.totalSize > 0) {
        eta = (item.totalSize - item.downloadedSize) / item.speed;
    }
    m_timeLabel->setText(item.status == "Completed" ? "--"
                                                    : (item.status == "Downloading" ? formatDuration(eta) : "--"));

    m_urlLabel->setText(item.url);

    if (item.error.isEmpty()) {
        m_errorLabel->setVisible(false);
        m_errorLabel->clear();
    } else {
        m_errorLabel->setVisible(true);
        m_errorLabel->setText(item.error);
        m_errorLabel->setStyleSheet("color: " + QColor(200, 0, 0).name());
    }

    // Playlist item list.
    const QVector<DownloadItem> all = DownloadManager::instance().getDownloads();
    QVector<int> childIds;
    for (const DownloadItem& candidate : all) {
        if (candidate.parentId == m_id) childIds.append(candidate.id);
    }

    if (childIds.isEmpty()) {
        m_itemsLabel->setText("--");
        m_itemTree->setVisible(false);
    } else {
        int done = 0, failed = 0;
        for (int cid : childIds) {
            const DownloadItem child = DownloadManager::instance().getDownload(cid);
            if (child.status == "Completed") done++;
            if (child.status == "Failed") failed++;
        }
        m_itemsLabel->setText(QString("%1 of %2 done").arg(done).arg(childIds.size()) +
                              (failed > 0 ? QString(", %1 failed").arg(failed) : QString()));
        m_itemTree->setVisible(true);
        m_itemTree->setUpdatesEnabled(false);
        m_itemTree->clear();
        for (int cid : childIds) {
            const DownloadItem child = DownloadManager::instance().getDownload(cid);
            QTreeWidgetItem* row = new QTreeWidgetItem(m_itemTree);
            row->setText(0, child.fileName.isEmpty() ? child.url : child.fileName);
            row->setText(1, statusText(child.status));
            row->setText(2, child.totalSize > 0 ? formatSize(child.downloadedSize) + " / " + formatSize(child.totalSize)
                                               : formatSize(child.downloadedSize));
            row->setText(3, QString::number(qBound(0.0, child.progress, 100.0), 'f', 1) + "%");
            row->setForeground(1, statusColorFor(child.status, palette()));
            if (!child.error.isEmpty()) {
                row->setToolTip(2, child.error);
            }
            if (child.attempts > 0) {
                row->setToolTip(3, QString("Attempt %1").arg(child.attempts + 1));
            }
        }
        m_itemTree->setUpdatesEnabled(true);
        if (m_itemTree->height() > 8) m_itemTree->setMaximumHeight(240);
    }

    if (m_lastStatus != item.status) {
        m_lastStatus = item.status;
        Logger::instance().info("Download info window for " + QString::number(m_id) + " -> " + item.status);
    }
    updateButtons();
}

void DownloadInfoDialog::updateButtons() {
    DownloadItem item = DownloadManager::instance().getDownload(m_id);
    if (item.id == 0) {
        m_pauseButton->setEnabled(false);
        m_retryButton->setEnabled(false);
        m_cancelButton->setEnabled(false);
        return;
    }

    if (item.status == "Downloading") {
        m_pauseButton->setText("Pause");
        m_pauseButton->setEnabled(true);
    } else if (item.status == "Paused") {
        m_pauseButton->setText("Resume");
        m_pauseButton->setEnabled(true);
    } else if (item.status == "Queued") {
        m_pauseButton->setText("Pause");
        m_pauseButton->setEnabled(false);
    } else {
        m_pauseButton->setEnabled(false);
    }

    const bool retryable = item.status == "Failed" || item.status == "Cancelled" || item.status == "Paused";
    m_retryButton->setEnabled(retryable);

    const bool cancellable = item.status == "Downloading" || item.status == "Queued" ||
                             item.status == "Paused" || item.status == "Failed";
    m_cancelButton->setEnabled(cancellable);
}

void DownloadInfoDialog::onPauseResume() {
    DownloadItem item = DownloadManager::instance().getDownload(m_id);
    if (item.id == 0) return;

    if (item.status == "Downloading") {
        DownloadManager::instance().pauseDownload(m_id);
    } else if (item.status == "Paused") {
        DownloadManager::instance().resumeDownload(m_id);
    }
    refresh();
}

void DownloadInfoDialog::onCancel() {
    DownloadManager::instance().cancelDownload(m_id);
    refresh();
}

void DownloadInfoDialog::onRetry() {
    DownloadManager::instance().retryDownload(m_id);
    refresh();
}

void DownloadInfoDialog::onOpenFolder() {
    DownloadItem item = DownloadManager::instance().getDownload(m_id);
    if (item.id == 0) return;
    QString folder = item.isFolder ? item.filePath : QFileInfo(item.filePath).absolutePath();
    if (!folder.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    }
}

void DownloadInfoDialog::onOpenFile() {
    DownloadItem item = DownloadManager::instance().getDownload(m_id);
    if (item.id == 0) return;
    if (item.isFolder) {
        onOpenFolder();
        return;
    }
    if (QFileInfo::exists(item.filePath)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(item.filePath));
    } else {
        onOpenFolder();
    }
}

void DownloadInfoDialog::onCopyUrl() {
    DownloadItem item = DownloadManager::instance().getDownload(m_id);
    if (item.id == 0) return;
    QApplication::clipboard()->setText(item.url);
}

void DownloadInfoDialog::closeEvent(QCloseEvent* event) {
    // Hide instead of destroying: the window is cached per download id and
    // re-shown on demand, and the transfer must keep running either way.
    m_timer->stop();
    QDialog::closeEvent(event);
}

void DownloadInfoDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    refresh();
    if (m_timer && !m_timer->isActive()) m_timer->start();
}
