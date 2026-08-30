#include "ui/TorrentDetailsDialog.h"
#include "utils/Aria2cManager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QTableWidget>
#include <QListWidget>
#include <QHeaderView>
#include <QGroupBox>
#include <QMessageBox>
#include <QTimer>

static QString fmtSize(qint64 b) {
    if (b < 1024) return QString::number(b) + " B";
    double v = b;
    const char* u[] = {"KB","MB","GB","TB"};
    int i = -1;
    do { v /= 1024.0; i++; } while (v >= 1024 && i < 3);
    return QString::number(v, 'f', 1) + " " + u[i];
}

static QString fmtSpeed(qint64 b) { return fmtSize(b) + "/s"; }

TorrentDetailsDialog::TorrentDetailsDialog(int torrentId, const QString& title, QWidget* parent)
    : QDialog(parent), torrentId(torrentId) {
    setWindowTitle("Torrent Details - " + title);
    resize(720, 640);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    // Info / stats group
    QGroupBox* statsGroup = new QGroupBox("Statistics");
    QGridLayout* grid = new QGridLayout(statsGroup);
    infoHashLabel = new QLabel("-");
    infoHashLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    infoHashLabel->setAccessibleName("Info hash");
    statusLabel = new QLabel("-");
    statusLabel->setAccessibleName("Status");
    peersLabel = new QLabel("0");
    peersLabel->setAccessibleName("Peers");
    leechersLabel = new QLabel("0");
    leechersLabel->setAccessibleName("Leechers");
    seedsLabel = new QLabel("0");
    seedsLabel->setAccessibleName("Seeds");
    downloadLabel = new QLabel("0 B/s");
    downloadLabel->setAccessibleName("Download rate");
    uploadLabel = new QLabel("0 B/s");
    uploadLabel->setAccessibleName("Upload rate");
    totalUpLabel = new QLabel("0 B");
    totalUpLabel->setAccessibleName("Total uploaded");
    ratioLabel = new QLabel("0.00");
    ratioLabel->setAccessibleName("Ratio");
    grid->addWidget(new QLabel("Status:"), 0, 0);
    grid->addWidget(statusLabel, 0, 1);
    grid->addWidget(new QLabel("Peers (leech):"), 0, 2);
    grid->addWidget(peersLabel, 0, 3);
    grid->addWidget(new QLabel("Seeds:"), 1, 0);
    grid->addWidget(seedsLabel, 1, 1);
    grid->addWidget(new QLabel("Download:"), 1, 2);
    grid->addWidget(downloadLabel, 1, 3);
    grid->addWidget(new QLabel("Upload:"), 2, 0);
    grid->addWidget(uploadLabel, 2, 1);
    grid->addWidget(new QLabel("Total Uploaded:"), 2, 2);
    grid->addWidget(totalUpLabel, 2, 3);
    mainLayout->addWidget(statsGroup);

    QLabel* hashCaption = new QLabel("Info Hash: " + Aria2cManager::instance().getInfoHash(torrentId));
    hashCaption->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mainLayout->addWidget(hashCaption);

    // Peers table
    QGroupBox* peersGroup = new QGroupBox("Peers");
    QVBoxLayout* peersLayout = new QVBoxLayout(peersGroup);
    peerTable = new QTableWidget(0, 6);
    peerTable->setAccessibleName("Peer list");
    peerTable->setHorizontalHeaderLabels({"IP", "Port", "Seeder", "Download", "Upload", "Client"});
    peerTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    peerTable->verticalHeader()->setVisible(false);
    peerTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    peerTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    peersLayout->addWidget(peerTable);
    mainLayout->addWidget(peersGroup);

    // Trackers
    QGroupBox* trackersGroup = new QGroupBox("Trackers");
    QVBoxLayout* trackersLayout = new QVBoxLayout(trackersGroup);
    trackerList = new QListWidget();
    trackerList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    trackersLayout->addWidget(trackerList);
    QHBoxLayout* trackerAddLayout = new QHBoxLayout();
    trackerEdit = new QLineEdit();
    trackerEdit->setPlaceholderText("udp://tracker.example.com:80/announce");
    addTrackerBtn = new QPushButton("Add Tracker");
    removeTrackerBtn = new QPushButton("Remove Selected");
    trackerAddLayout->addWidget(trackerEdit, 1);
    trackerAddLayout->addWidget(addTrackerBtn);
    trackerAddLayout->addWidget(removeTrackerBtn);
    trackersLayout->addLayout(trackerAddLayout);
    mainLayout->addWidget(trackersGroup);

    // Seed control
    QGroupBox* seedGroup = new QGroupBox("Seeding");
    QHBoxLayout* seedLayout = new QHBoxLayout(seedGroup);
    seedLayout->addWidget(new QLabel("Seed time:"));
    seedCombo = new QComboBox();
    seedCombo->addItem("No seeding", -1);
    seedCombo->addItem("30 minutes", 30);
    seedCombo->addItem("1 hour", 60);
    seedCombo->addItem("2 hours", 120);
    seedCombo->addItem("Seed forever", 0);
    QPushButton* applySeedBtn = new QPushButton("Apply");
    seedLayout->addWidget(seedCombo);
    seedLayout->addWidget(applySeedBtn);
    seedLayout->addStretch();
    mainLayout->addWidget(seedGroup);

    QHBoxLayout* closeLayout = new QHBoxLayout();
    closeLayout->addStretch();
    QPushButton* closeBtn = new QPushButton("Close");
    closeBtn->setDefault(true);
    closeLayout->addWidget(closeBtn);
    mainLayout->addLayout(closeLayout);

    connect(addTrackerBtn, &QPushButton::clicked, this, &TorrentDetailsDialog::onAddTracker);
    connect(removeTrackerBtn, &QPushButton::clicked, this, &TorrentDetailsDialog::onRemoveTracker);
    connect(applySeedBtn, &QPushButton::clicked, this, &TorrentDetailsDialog::onApplySeedTime);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    refreshTimer = new QTimer(this);
    refreshTimer->setInterval(2000);
    connect(refreshTimer, &QTimer::timeout, this, &TorrentDetailsDialog::refresh);
    refreshTimer->start();

    connect(&Aria2cManager::instance(), &Aria2cManager::torrentStateUpdated,
            this, &TorrentDetailsDialog::onPeerUpdate);

    refresh();
}

void TorrentDetailsDialog::onPeerUpdate(int id) {
    if (id == torrentId) refresh();
}

void TorrentDetailsDialog::refresh() {
    loadStats();
    loadTrackers();
    loadPeers();
}

void TorrentDetailsDialog::loadStats() {
    bool running = Aria2cManager::instance().isRunning(torrentId);
    statusLabel->setText(running ? "Active" : "Idle/Complete");

    qint64 total = Aria2cManager::instance().getTotalBytes(torrentId);
    qint64 down = Aria2cManager::instance().getDownloadedBytes(torrentId);
    qint64 upSpeed = Aria2cManager::instance().getUploadSpeed(torrentId);
    qint64 upTotal = Aria2cManager::instance().getUploadedBytes(torrentId);

    peersLabel->setText(QString::number(Aria2cManager::instance().getConnectedPeers(torrentId)));
    leechersLabel->setText(QString::number(Aria2cManager::instance().getLeechers(torrentId)));
    seedsLabel->setText(QString::number(Aria2cManager::instance().getSeeds(torrentId)));
    downloadLabel->setText(fmtSpeed(Aria2cManager::instance().getSpeed(torrentId)));
    uploadLabel->setText(fmtSpeed(upSpeed));
    totalUpLabel->setText(fmtSize(upTotal));
    ratioLabel->setText(QString::number(down > 0 ? (double)upTotal / down : 0.0, 'f', 2));
}

void TorrentDetailsDialog::loadTrackers() {
    QStringList trackers = Aria2cManager::instance().getTrackerList(torrentId);
    if (trackerList->count() == trackers.size()) return;
    trackerList->clear();
    for (const QString& t : trackers) {
        trackerList->addItem(t);
    }
}

void TorrentDetailsDialog::loadPeers() {
    QVector<PeerInfo> peers = Aria2cManager::instance().getPeers(torrentId);
    peerTable->setRowCount(peers.size());
    for (int i = 0; i < peers.size(); i++) {
        const PeerInfo& p = peers[i];
        peerTable->setItem(i, 0, new QTableWidgetItem(p.ip));
        peerTable->setItem(i, 1, new QTableWidgetItem(QString::number(p.port)));
        peerTable->setItem(i, 2, new QTableWidgetItem(p.seeder ? "Yes" : "No"));
        peerTable->setItem(i, 3, new QTableWidgetItem(fmtSpeed(p.downloadSpeed)));
        peerTable->setItem(i, 4, new QTableWidgetItem(fmtSpeed(p.uploadSpeed)));
        peerTable->setItem(i, 5, new QTableWidgetItem(p.peerId.left(24)));
    }
}

void TorrentDetailsDialog::onAddTracker() {
    QString tracker = trackerEdit->text().trimmed();
    if (tracker.isEmpty()) return;
    Aria2cManager::instance().addTrackerToTorrent(torrentId, tracker);
    trackerEdit->clear();
    QTimer::singleShot(300, this, &TorrentDetailsDialog::refresh);
}

void TorrentDetailsDialog::onRemoveTracker() {
    QList<QListWidgetItem*> selected = trackerList->selectedItems();
    for (QListWidgetItem* it : selected) {
        Aria2cManager::instance().removeTrackerFromTorrent(torrentId, it->text());
    }
    QTimer::singleShot(300, this, &TorrentDetailsDialog::refresh);
}

void TorrentDetailsDialog::onApplySeedTime() {
    int value = seedCombo->currentData().toInt();
    Aria2cManager::instance().seedTorrent(torrentId, value);
}
