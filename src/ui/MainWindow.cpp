#include "ui/MainWindow.h"
#include "ui/AddUrlDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/AboutDialog.h"
#include "ui/DownloadManagerDialog.h"
#include "core/DownloadManager.h"
#include "core/LocalServer.h"
#include "db/DatabaseManager.h"
#include "utils/Logger.h"
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QTableWidget>
#include <QHeaderView>
#include <QAction>
#include <QApplication>
#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QTimer>
#include <QMenu>
#include <QContextMenuEvent>
#include <QCloseEvent>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include <QDesktopServices>
#include <QClipboard>
#include <QUrl>
#include <QFileInfo>
#include <QDir>
#include <QShortcut>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFileDialog>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QSystemTrayIcon>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), currentFilter(0) {
    setWindowTitle("Copper Download Manager");
    setMinimumSize(1000, 600);
    resize(1200, 700);
    setAcceptDrops(true);
    setWindowIcon(QIcon(":/icons/app.png"));

    setupUI();
    setupMenuBar();
    setupToolBar();
    setupStatusBar();
    setupConnections();
    setupShortcuts();
    restoreWindowState();

    trayIcon = new QSystemTrayIcon(QIcon(":/icons/app.png"), this);
    trayMenu = new QMenu(this);
    trayMenu->addAction(QIcon(":/icons/CurrentDownload.png"), "Open", this, [this]() {
        show();
        raise();
        activateWindow();
    });
    trayMenu->addSeparator();
    trayMenu->addAction(QIcon(":/icons/Delete.png"), "Quit", qApp, &QApplication::quit);
    trayIcon->setContextMenu(trayMenu);
    connect(trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger) {
            show();
            raise();
            activateWindow();
        }
    });
    trayIcon->show();

    connect(&DownloadManager::instance(), &DownloadManager::downloadAdded, this, &MainWindow::onDownloadAdded);
    connect(&DownloadManager::instance(), &DownloadManager::downloadProgress, this, &MainWindow::onDownloadProgress);
    connect(&DownloadManager::instance(), &DownloadManager::downloadFinished, this, &MainWindow::onDownloadFinished);
    connect(&DownloadManager::instance(), &DownloadManager::downloadFailed, this, &MainWindow::onDownloadFailed);
    connect(&DownloadManager::instance(), &DownloadManager::downloadRemoved, this, &MainWindow::onDownloadRemoved);
    connect(&DownloadManager::instance(), &DownloadManager::statusChanged, this, &MainWindow::onStatusChanged);
    connect(&DownloadManager::instance(), &DownloadManager::downloadSpeed, this, &MainWindow::onDownloadSpeed);

    connect(&LocalServer::instance(), &LocalServer::argumentForwarded, this, &MainWindow::onArgumentForwarded);

    refreshTimer = new QTimer(this);
    connect(refreshTimer, &QTimer::timeout, this, &MainWindow::refreshTable);
    connect(refreshTimer, &QTimer::timeout, this, &MainWindow::updateStatusBar);
    refreshTimer->start(1000);

    refreshTable();

    Logger::instance().info("MainWindow initialized");
}

MainWindow::~MainWindow() {
    saveWindowState();
}

void MainWindow::setupUI() {
    QWidget* centralWidget = new QWidget(this);
    QHBoxLayout* mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    QSplitter* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setHandleWidth(2);

    sidebar = new QListWidget(this);
    sidebar->setMaximumWidth(200);
    sidebar->setMinimumWidth(150);
    sidebar->setIconSize(QSize(20, 20));
    splitter->addWidget(sidebar);

    QWidget* rightWidget = new QWidget(this);
    QVBoxLayout* rightLayout = new QVBoxLayout(rightWidget);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(0);

    table = new QTableWidget(this);
    table->setColumnCount(8);
    table->setHorizontalHeaderLabels({"Name", "Size", "Status", "Progress", "Speed", "ETA", "Peers", "Added"});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    table->horizontalHeader()->resizeSection(3, 180);
    table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(7, QHeaderView::ResizeToContents);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table->setAlternatingRowColors(true);
    table->setSortingEnabled(false);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(40);
    table->setContextMenuPolicy(Qt::CustomContextMenu);
    table->setDragDropMode(QAbstractItemView::NoDragDrop);
    table->setShowGrid(false);

    rightLayout->addWidget(table);
    splitter->addWidget(rightWidget);

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    mainLayout->addWidget(splitter);
    setCentralWidget(centralWidget);

    setupSidebar();
}

void MainWindow::setupSidebar() {
    sidebar->addItem(new QListWidgetItem(QIcon(":/icons/CurrentDownload.png"), "All Downloads"));
    sidebar->addItem(new QListWidgetItem(QIcon(":/icons/Start.png"), "Active"));
    sidebar->addItem(new QListWidgetItem(QIcon(":/icons/Pause.png"), "Paused"));
    sidebar->addItem(new QListWidgetItem(QIcon(":/icons/DownloadComplete.png"), "Completed"));
    sidebar->addItem(new QListWidgetItem(QIcon(":/icons/Failed.png"), "Failed"));
    sidebar->addItem(new QListWidgetItem(QIcon(":/icons/Delete.png"), "Cancelled"));

    sidebar->setCurrentRow(0);

    connect(sidebar, &QListWidget::currentRowChanged, this, &MainWindow::onSidebarFilterChanged);
}

void MainWindow::setupMenuBar() {
    QMenuBar* menuBar = this->menuBar();

    QMenu* fileMenu = menuBar->addMenu("&File");
    QAction* addUrlAction = fileMenu->addAction(QIcon(":/icons/AddUrl.png"), "Add &URL...");
    addUrlAction->setShortcut(QKeySequence("Ctrl+U"));
    connect(addUrlAction, &QAction::triggered, this, &MainWindow::onAddUrl);

    fileMenu->addSeparator();

    QAction* startAction = fileMenu->addAction(QIcon(":/icons/Start.png"), "&Start All");
    connect(startAction, &QAction::triggered, this, &MainWindow::onResumeAll);

    QAction* pauseAction = fileMenu->addAction(QIcon(":/icons/Pause.png"), "&Pause All");
    connect(pauseAction, &QAction::triggered, this, &MainWindow::onPauseAll);

    fileMenu->addSeparator();

    QAction* settingsAction = fileMenu->addAction(QIcon(":/icons/Settings.png"), "&Settings...");
    settingsAction->setShortcut(QKeySequence("Ctrl+,"));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::onSettings);

    fileMenu->addSeparator();

    QAction* exitAction = fileMenu->addAction("E&xit");
    exitAction->setShortcut(QKeySequence("Alt+F4"));
    connect(exitAction, &QAction::triggered, qApp, &QApplication::quit);

    QMenu* viewMenu = menuBar->addMenu("&View");
    QAction* clearCompletedAction = viewMenu->addAction("Clear &Completed");
    connect(clearCompletedAction, &QAction::triggered, this, &MainWindow::onClearCompleted);

    QMenu* helpMenu = menuBar->addMenu("&Help");
    QAction* aboutAction = helpMenu->addAction(QIcon(":/icons/About.png"), "&About");
    aboutAction->setShortcut(QKeySequence("F1"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAbout);
}

void MainWindow::setupToolBar() {
    toolBar = new QToolBar("Main Toolbar", this);
    toolBar->setIconSize(QSize(24, 24));
    toolBar->setMovable(false);
    addToolBar(toolBar);

    QAction* addUrlBtn = toolBar->addAction(QIcon(":/icons/AddUrl.png"), "Add URL");
    connect(addUrlBtn, &QAction::triggered, this, &MainWindow::onAddUrl);

    toolBar->addSeparator();

    QAction* startBtn = toolBar->addAction(QIcon(":/icons/Start.png"), "Resume");
    connect(startBtn, &QAction::triggered, this, &MainWindow::onStartSelected);

    QAction* pauseBtn = toolBar->addAction(QIcon(":/icons/Pause.png"), "Pause");
    connect(pauseBtn, &QAction::triggered, this, &MainWindow::onPauseSelected);

    QAction* stopBtn = toolBar->addAction(QIcon(":/icons/Stop.png"), "Cancel");
    connect(stopBtn, &QAction::triggered, this, &MainWindow::onStopSelected);

    toolBar->addSeparator();

    QAction* deleteBtn = toolBar->addAction(QIcon(":/icons/Delete.png"), "Delete");
    connect(deleteBtn, &QAction::triggered, this, &MainWindow::onDeleteSelected);

    toolBar->addSeparator();

    QAction* settingsBtn = toolBar->addAction(QIcon(":/icons/Settings.png"), "Settings");
    connect(settingsBtn, &QAction::triggered, this, &MainWindow::onSettings);

    QAction* aboutBtn = toolBar->addAction(QIcon(":/icons/About.png"), "About");
    connect(aboutBtn, &QAction::triggered, this, &MainWindow::onAbout);
}

void MainWindow::setupStatusBar() {
    statusBarWidget = new QStatusBar(this);
    setStatusBar(statusBarWidget);

    totalSpeedLabel = new QLabel("Speed: 0 B/s");
    activeCountLabel = new QLabel("Active: 0");
    queuedCountLabel = new QLabel("Queued: 0");

    statusBarWidget->addPermanentWidget(activeCountLabel);
    statusBarWidget->addPermanentWidget(queuedCountLabel);
    statusBarWidget->addPermanentWidget(totalSpeedLabel);
    statusBarWidget->showMessage("Ready");
}

void MainWindow::setupConnections() {
    connect(table, &QTableWidget::customContextMenuRequested, this, &MainWindow::showContextMenu);
    connect(table, &QTableWidget::doubleClicked, this, &MainWindow::onTableDoubleClick);
}

void MainWindow::setupShortcuts() {
    QShortcut* deleteShortcut = new QShortcut(QKeySequence("Delete"), this);
    connect(deleteShortcut, &QShortcut::activated, this, &MainWindow::onDeleteSelected);

    QShortcut* selectAllShortcut = new QShortcut(QKeySequence("Ctrl+A"), this);
    connect(selectAllShortcut, &QShortcut::activated, table, &QTableWidget::selectAll);
}

void MainWindow::restoreWindowState() {
    QSettings settings("Copper", "DownloadManager");
    restoreGeometry(settings.value("geometry").toByteArray());
    restoreState(settings.value("windowState").toByteArray());
}

void MainWindow::saveWindowState() {
    QSettings settings("Copper", "DownloadManager");
    settings.setValue("geometry", saveGeometry());
    settings.setValue("windowState", saveState());
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveWindowState();
    if (trayIcon && trayIcon->isVisible()) {
        trayIcon->hide();
    }
    event->accept();
}

void MainWindow::changeEvent(QEvent* event) {
    if (event->type() == QEvent::WindowStateChange) {
        if (isMinimized() && DatabaseManager::instance().getSetting("minimizeToTray", "false") == "true") {
            hide();
            trayIcon->showMessage("Copper Download Manager", "Minimized to system tray");
            event->accept();
        }
    }
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl& url : urls) {
        if (url.isLocalFile()) {
            QString filePath = url.toLocalFile();
            if (filePath.endsWith(".torrent")) {
                DownloadManager::instance().addDownload(filePath, QFileInfo(filePath).absolutePath(), "Torrent");
            }
        } else {
            AddUrlDialog dialog(this);
            dialog.setUrl(url.toString());
            dialog.exec();
        }
    }
}

void MainWindow::onAddUrl() {
    AddUrlDialog dialog(this);
    dialog.exec();
    refreshTable();
}

void MainWindow::onSettings() {
    SettingsDialog dialog(this);
    dialog.exec();
}

void MainWindow::onAbout() {
    AboutDialog dialog(this);
    dialog.exec();
}

void MainWindow::onStartSelected() {
    QList<QTableWidgetItem*> selected = table->selectedItems();
    if (selected.isEmpty()) return;

    QSet<int> rows;
    for (QTableWidgetItem* item : selected) rows.insert(item->row());

    for (int row : rows) {
        QTableWidgetItem* idItem = table->item(row, 0);
        if (idItem) {
            int id = idItem->data(Qt::UserRole).toInt();
            DownloadItem dlItem = DownloadManager::instance().getDownload(id);
            if (dlItem.isFolder && !dlItem.childIds.isEmpty()) {
                for (int cid : dlItem.childIds) {
                    DownloadManager::instance().resumeDownload(cid);
                }
            } else {
                DownloadManager::instance().resumeDownload(id);
            }
        }
    }
}

void MainWindow::onPauseSelected() {
    QList<QTableWidgetItem*> selected = table->selectedItems();
    if (selected.isEmpty()) return;

    QSet<int> rows;
    for (QTableWidgetItem* item : selected) rows.insert(item->row());

    for (int row : rows) {
        QTableWidgetItem* idItem = table->item(row, 0);
        if (idItem) {
            int id = idItem->data(Qt::UserRole).toInt();
            DownloadItem dlItem = DownloadManager::instance().getDownload(id);
            if (dlItem.isFolder && !dlItem.childIds.isEmpty()) {
                for (int cid : dlItem.childIds) {
                    DownloadManager::instance().pauseDownload(cid);
                }
            } else {
                DownloadManager::instance().pauseDownload(id);
            }
        }
    }
}

void MainWindow::onStopSelected() {
    QList<QTableWidgetItem*> selected = table->selectedItems();
    if (selected.isEmpty()) return;

    QSet<int> rows;
    for (QTableWidgetItem* item : selected) rows.insert(item->row());

    for (int row : rows) {
        QTableWidgetItem* idItem = table->item(row, 0);
        if (idItem) {
            int id = idItem->data(Qt::UserRole).toInt();
            DownloadItem dlItem = DownloadManager::instance().getDownload(id);
            if (dlItem.isFolder && !dlItem.childIds.isEmpty()) {
                for (int cid : dlItem.childIds) {
                    DownloadManager::instance().cancelDownload(cid);
                }
            } else {
                DownloadManager::instance().cancelDownload(id);
            }
        }
    }
}

void MainWindow::onDeleteSelected() {
    QList<QTableWidgetItem*> selected = table->selectedItems();
    if (selected.isEmpty()) return;

    QSet<int> rows;
    for (QTableWidgetItem* item : selected) rows.insert(item->row());

    QMessageBox::StandardButton reply = QMessageBox::question(this, "Confirm Delete",
        "Remove " + QString::number(rows.size()) + " download(s)?",
        QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes) return;

    for (int row : rows) {
        QTableWidgetItem* idItem = table->item(row, 0);
        if (idItem) {
            int id = idItem->data(Qt::UserRole).toInt();
            DownloadItem dlItem = DownloadManager::instance().getDownload(id);
            if (dlItem.isFolder && !dlItem.childIds.isEmpty()) {
                expandedGroups.remove(id);
                for (int cid : dlItem.childIds) {
                    DownloadManager::instance().removeDownload(cid);
                }
            }
            DownloadManager::instance().removeDownload(id);
        }
    }
}

void MainWindow::onOpenFile() {
    QTableWidgetItem* idItem = table->item(table->currentRow(), 0);
    if (!idItem) return;
    int id = idItem->data(Qt::UserRole).toInt();
    DownloadItem item = DownloadManager::instance().getDownload(id);
    if (!item.filePath.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(item.filePath));
    }
}

void MainWindow::onOpenFolder() {
    QTableWidgetItem* idItem = table->item(table->currentRow(), 0);
    if (!idItem) return;
    int id = idItem->data(Qt::UserRole).toInt();
    DownloadItem item = DownloadManager::instance().getDownload(id);
    QString folder = QFileInfo(item.filePath).absolutePath();
    if (!folder.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    }
}

void MainWindow::onCopyUrl() {
    QTableWidgetItem* idItem = table->item(table->currentRow(), 0);
    if (!idItem) return;
    int id = idItem->data(Qt::UserRole).toInt();
    DownloadItem item = DownloadManager::instance().getDownload(id);
    QApplication::clipboard()->setText(item.url);
}

void MainWindow::onProperties() {
    QTableWidgetItem* idItem = table->item(table->currentRow(), 0);
    if (!idItem) return;
    int id = idItem->data(Qt::UserRole).toInt();
    DownloadItem item = DownloadManager::instance().getDownload(id);

    QString info = "ID: " + QString::number(item.id) + "\n"
                   "URL: " + item.url + "\n"
                   "File: " + item.filePath + "\n"
                   "Type: " + item.type + "\n"
                   "Status: " + item.status + "\n"
                   "Progress: " + QString::number(item.progress, 'f', 1) + "%\n"
                   "Downloaded: " + formatSize(item.downloadedSize) + " / " + formatSize(item.totalSize) + "\n"
                   "Speed: " + formatSpeed(item.speed) + "\n"
                   "Added: " + item.addedAt.toString("yyyy-MM-dd hh:mm:ss");

    if (!item.completedAt.isNull()) {
        info += "\nCompleted: " + item.completedAt.toString("yyyy-MM-dd hh:mm:ss");
    }
    if (!item.error.isEmpty()) {
        info += "\nError: " + item.error;
    }

    QMessageBox::information(this, "Download Properties", info);
}

void MainWindow::onClearCompleted() {
    DownloadManager::instance().clearCompleted();
    refreshTable();
}

void MainWindow::onPauseAll() {
    DownloadManager::instance().pauseAll();
    refreshTable();
}

void MainWindow::onResumeAll() {
    DownloadManager::instance().resumeAll();
    refreshTable();
}

void MainWindow::onDownloadAdded(int id, const QString& path, const QString& type, bool isFolder) {
    Q_UNUSED(id);
    Q_UNUSED(path);
    Q_UNUSED(type);
    Q_UNUSED(isFolder);
    refreshTable();
}

void MainWindow::onDownloadProgress(int id, qint64 downloaded, qint64 total) {
    downloadProgressMap[id] = downloaded;
    downloadTotalMap[id] = total;

    DownloadItem item = DownloadManager::instance().getDownload(id);
    if (item.type == "Torrent" && (item.connectedPeers > 0 || item.seeds > 0)) {
        for (int row = 0; row < table->rowCount(); row++) {
            QTableWidgetItem* nameItem = table->item(row, 0);
            if (nameItem && nameItem->data(Qt::UserRole).toInt() == id) {
                QString peersText = QString::number(item.connectedPeers) + "P / " + QString::number(item.seeds) + "S";
                QTableWidgetItem* peersItem = table->item(row, 6);
                if (peersItem) {
                    peersItem->setText(peersText);
                }
                break;
            }
        }
    }
}

void MainWindow::onDownloadFinished(int id) {
    downloadSpeeds.remove(id);
    refreshTable();
}

void MainWindow::onDownloadFailed(int id, const QString& error) {
    Q_UNUSED(id);
    Q_UNUSED(error);
    refreshTable();
}

void MainWindow::onDownloadRemoved(int id) {
    downloadSpeeds.remove(id);
    downloadProgressMap.remove(id);
    downloadTotalMap.remove(id);
    refreshTable();
}

void MainWindow::onStatusChanged(int id, const QString& status) {
    Q_UNUSED(id);
    Q_UNUSED(status);
}

void MainWindow::onTotalSpeedUpdated(qint64 speed) {
    totalSpeedLabel->setText("Speed: " + formatSpeed(speed));
}

void MainWindow::onDownloadSpeed(int id, qint64 speed) {
    downloadSpeeds[id] = speed;

    qint64 totalSpeed = 0;
    for (qint64 s : downloadSpeeds.values()) totalSpeed += s;
    totalSpeedLabel->setText("Speed: " + formatSpeed(totalSpeed));
}

void MainWindow::onSidebarFilterChanged() {
    currentFilter = sidebar->currentRow();
    refreshTable();
}

void MainWindow::refreshTable() {
    table->setUpdatesEnabled(false);
    table->setRowCount(0);

    QVector<DownloadItem> allDownloads = DownloadManager::instance().getDownloads();

    QMap<int, DownloadItem> downloadMap;
    for (const DownloadItem& item : allDownloads) {
        downloadMap[item.id] = item;
    }

    int activeCount = 0;
    int queuedCount = 0;
    Qt::ItemFlags nonEditable = Qt::ItemIsSelectable | Qt::ItemIsEnabled;

    for (const DownloadItem& item : allDownloads) {
        bool show = false;
        switch (currentFilter) {
            case 0: show = true; break;
            case 1: show = (item.status == "Downloading"); break;
            case 2: show = (item.status == "Paused"); break;
            case 3: show = (item.status == "Completed"); break;
            case 4: show = (item.status == "Failed"); break;
            case 5: show = (item.status == "Cancelled"); break;
        }

        if (!show) continue;
        if (item.parentId != -1) continue;

        bool isGroup = item.isFolder && !item.childIds.isEmpty();

        if (isGroup) {
            if (item.status == "Downloading") activeCount++;
            if (item.status == "Queued") queuedCount++;

            bool expanded = expandedGroups.contains(item.id);
            QString expandIcon = expanded ? "[-] " : "[+] ";

            int row = table->rowCount();
            table->insertRow(row);

            QTableWidgetItem* nameItem = new QTableWidgetItem(
                QIcon(":/icons/CurrentDownload.png"),
                expandIcon + item.fileName);
            nameItem->setData(Qt::UserRole, item.id);
            nameItem->setData(Qt::UserRole + 2, true);
            nameItem->setToolTip(item.filePath);
            nameItem->setFlags(nonEditable);
            QFont boldFont = nameItem->font();
            boldFont.setBold(true);
            nameItem->setFont(boldFont);
            table->setItem(row, 0, nameItem);

            QTableWidgetItem* sizeItem = new QTableWidgetItem(formatSize(item.totalSize));
            sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            sizeItem->setFlags(nonEditable);
            table->setItem(row, 1, sizeItem);

            int completedChildren = 0;
            int totalChildren = item.childIds.size();
            for (int cid : item.childIds) {
                if (downloadMap.contains(cid) && downloadMap[cid].status == "Completed") completedChildren++;
            }
            QString groupStatus = item.status;
            if (!item.childIds.isEmpty()) {
                groupStatus = QString::number(completedChildren) + "/" + QString::number(totalChildren) + " " + item.status;
            }

            QTableWidgetItem* statusItem = new QTableWidgetItem(groupStatus);
            statusItem->setForeground(statusColor(item.status));
            statusItem->setFlags(nonEditable);
            table->setItem(row, 2, statusItem);

            QTableWidgetItem* progressItem = new QTableWidgetItem();
            progressItem->setData(Qt::DisplayRole, (int)item.progress);
            progressItem->setData(Qt::UserRole + 1, item.progress);
            progressItem->setFlags(nonEditable);
            table->setItem(row, 3, progressItem);

            qint64 groupSpeed = 0;
            for (int cid : item.childIds) {
                if (downloadMap.contains(cid)) groupSpeed += downloadMap[cid].speed;
            }
            QTableWidgetItem* speedItem = new QTableWidgetItem(formatSpeed(groupSpeed));
            speedItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            speedItem->setFlags(nonEditable);
            table->setItem(row, 4, speedItem);

            qint64 groupRemaining = item.totalSize - item.downloadedSize;
            qint64 groupSpeedForEta = groupSpeed > 0 ? groupSpeed : 1;
            QTableWidgetItem* etaItem = new QTableWidgetItem(formatEta(groupRemaining, groupSpeedForEta));
            etaItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            etaItem->setFlags(nonEditable);
            table->setItem(row, 5, etaItem);

            QTableWidgetItem* peersItem = new QTableWidgetItem("");
            peersItem->setFlags(nonEditable);
            table->setItem(row, 6, peersItem);

            QTableWidgetItem* dateItem = new QTableWidgetItem(item.addedAt.toString("yyyy-MM-dd hh:mm"));
            dateItem->setFlags(nonEditable);
            table->setItem(row, 7, dateItem);

            if (expanded) {
                for (int cid : item.childIds) {
                    if (!downloadMap.contains(cid)) continue;
                    const DownloadItem& child = downloadMap[cid];

                    bool childShow = false;
                    switch (currentFilter) {
                        case 0: childShow = true; break;
                        case 1: childShow = (child.status == "Downloading"); break;
                        case 2: childShow = (child.status == "Paused"); break;
                        case 3: childShow = (child.status == "Completed"); break;
                        case 4: childShow = (child.status == "Failed"); break;
                        case 5: childShow = (child.status == "Cancelled"); break;
                    }
                    if (!childShow) continue;

                    if (child.status == "Downloading") activeCount++;
                    if (child.status == "Queued") queuedCount++;

                    int crow = table->rowCount();
                    table->insertRow(crow);

                    QTableWidgetItem* cName = new QTableWidgetItem(
                        fileTypeIcon(child.filePath, false),
                        "    " + child.fileName);
                    cName->setData(Qt::UserRole, child.id);
                    cName->setData(Qt::UserRole + 2, false);
                    cName->setToolTip(child.filePath);
                    cName->setFlags(nonEditable);
                    table->setItem(crow, 0, cName);

                    QTableWidgetItem* cSize = new QTableWidgetItem(formatSize(child.totalSize));
                    cSize->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                    cSize->setFlags(nonEditable);
                    table->setItem(crow, 1, cSize);

                    QTableWidgetItem* cStatus = new QTableWidgetItem(child.status);
                    cStatus->setForeground(statusColor(child.status));
                    cStatus->setFlags(nonEditable);
                    table->setItem(crow, 2, cStatus);

                    QTableWidgetItem* cProgress = new QTableWidgetItem();
                    cProgress->setData(Qt::DisplayRole, (int)child.progress);
                    cProgress->setData(Qt::UserRole + 1, child.progress);
                    cProgress->setFlags(nonEditable);
                    table->setItem(crow, 3, cProgress);

                    QTableWidgetItem* cSpeed = new QTableWidgetItem(formatSpeed(child.speed));
                    cSpeed->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                    cSpeed->setFlags(nonEditable);
                    table->setItem(crow, 4, cSpeed);

                    qint64 cRemaining = child.totalSize - child.downloadedSize;
                    qint64 cSpd = child.speed > 0 ? child.speed : 1;
                    QTableWidgetItem* cEta = new QTableWidgetItem(formatEta(cRemaining, cSpd));
                    cEta->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                    cEta->setFlags(nonEditable);
                    table->setItem(crow, 5, cEta);

                    QString cPeers;
                    if (child.type == "Torrent" && (child.connectedPeers > 0 || child.seeds > 0)) {
                        cPeers = QString::number(child.connectedPeers) + "P / " + QString::number(child.seeds) + "S";
                    }
                    QTableWidgetItem* cPeersItem = new QTableWidgetItem(cPeers);
                    cPeersItem->setFlags(nonEditable);
                    table->setItem(crow, 6, cPeersItem);

                    QTableWidgetItem* cDate = new QTableWidgetItem(child.addedAt.toString("yyyy-MM-dd hh:mm"));
                    cDate->setFlags(nonEditable);
                    table->setItem(crow, 7, cDate);
                }
            }
        } else {
            if (item.status == "Downloading") activeCount++;
            if (item.status == "Queued") queuedCount++;

            int row = table->rowCount();
            table->insertRow(row);

            QTableWidgetItem* nameItem = new QTableWidgetItem(fileTypeIcon(item.filePath, item.isFolder), item.fileName);
            nameItem->setData(Qt::UserRole, item.id);
            nameItem->setData(Qt::UserRole + 2, false);
            nameItem->setToolTip(item.filePath);
            nameItem->setFlags(nonEditable);
            table->setItem(row, 0, nameItem);

            QTableWidgetItem* sizeItem = new QTableWidgetItem(formatSize(item.totalSize));
            sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            sizeItem->setFlags(nonEditable);
            table->setItem(row, 1, sizeItem);

            QTableWidgetItem* statusItem = new QTableWidgetItem(item.status);
            statusItem->setForeground(statusColor(item.status));
            statusItem->setFlags(nonEditable);
            table->setItem(row, 2, statusItem);

            QTableWidgetItem* progressItem = new QTableWidgetItem();
            progressItem->setData(Qt::DisplayRole, (int)item.progress);
            progressItem->setData(Qt::UserRole + 1, item.progress);
            progressItem->setFlags(nonEditable);
            table->setItem(row, 3, progressItem);

            QTableWidgetItem* speedItem = new QTableWidgetItem(formatSpeed(item.speed));
            speedItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            speedItem->setFlags(nonEditable);
            table->setItem(row, 4, speedItem);

            qint64 remaining = item.totalSize - item.downloadedSize;
            qint64 speed = item.speed > 0 ? item.speed : 1;
            QTableWidgetItem* etaItem = new QTableWidgetItem(formatEta(remaining, speed));
            etaItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            etaItem->setFlags(nonEditable);
            table->setItem(row, 5, etaItem);

            QString peersText;
            if (item.type == "Torrent" && (item.connectedPeers > 0 || item.seeds > 0)) {
                peersText = QString::number(item.connectedPeers) + "P / " + QString::number(item.seeds) + "S";
            }
            QTableWidgetItem* peersItem = new QTableWidgetItem(peersText);
            peersItem->setFlags(nonEditable);
            peersItem->setTextAlignment(Qt::AlignCenter);
            table->setItem(row, 6, peersItem);

            QTableWidgetItem* dateItem = new QTableWidgetItem(item.addedAt.toString("yyyy-MM-dd hh:mm"));
            dateItem->setFlags(nonEditable);
            table->setItem(row, 7, dateItem);
        }
    }

    table->setUpdatesEnabled(true);

    activeCountLabel->setText("Active: " + QString::number(activeCount));
    queuedCountLabel->setText("Queued: " + QString::number(queuedCount));
}

void MainWindow::updateStatusBar() {
    qint64 totalSpeed = 0;
    for (qint64 s : downloadSpeeds.values()) totalSpeed += s;
    totalSpeedLabel->setText("Speed: " + formatSpeed(totalSpeed));
}

void MainWindow::showContextMenu(const QPoint& pos) {
    QTableWidgetItem* item = table->itemAt(pos);
    if (!item) return;

    int row = item->row();
    QTableWidgetItem* idItem = table->item(row, 0);
    if (!idItem) return;

    int id = idItem->data(Qt::UserRole).toInt();
    bool isGroup = idItem->data(Qt::UserRole + 2).toBool();
    DownloadItem dlItem = DownloadManager::instance().getDownload(id);

    QMenu menu(this);

    if (isGroup) {
        if (expandedGroups.contains(id)) {
            menu.addAction("Collapse", this, &MainWindow::onTableCollapse);
        } else {
            menu.addAction("Expand", this, &MainWindow::onTableCollapse);
        }
        menu.addSeparator();
        if (dlItem.status == "Downloading") {
            menu.addAction(QIcon(":/icons/Pause.png"), "Pause All", this, &MainWindow::onPauseSelected);
        } else if (dlItem.status == "Paused" || dlItem.status == "Failed") {
            menu.addAction(QIcon(":/icons/Start.png"), "Resume All", this, &MainWindow::onStartSelected);
        }
        menu.addAction(QIcon(":/icons/Delete.png"), "Remove All", this, &MainWindow::onDeleteSelected);
    } else {
        if (dlItem.status == "Downloading") {
            menu.addAction(QIcon(":/icons/Pause.png"), "Pause", this, &MainWindow::onPauseSelected);
            menu.addAction(QIcon(":/icons/Stop.png"), "Cancel", this, &MainWindow::onStopSelected);
        } else if (dlItem.status == "Paused" || dlItem.status == "Failed") {
            menu.addAction(QIcon(":/icons/Start.png"), "Resume", this, &MainWindow::onStartSelected);
            menu.addAction(QIcon(":/icons/Stop.png"), "Cancel", this, &MainWindow::onStopSelected);
        } else if (dlItem.status == "Completed") {
            menu.addAction("Open File", this, &MainWindow::onOpenFile);
            menu.addAction("Open Folder", this, &MainWindow::onOpenFolder);
        }
        menu.addSeparator();
        menu.addAction("Copy URL", this, &MainWindow::onCopyUrl);
        menu.addAction("Properties", this, &MainWindow::onProperties);
        menu.addSeparator();
        menu.addAction(QIcon(":/icons/Delete.png"), "Remove", this, &MainWindow::onDeleteSelected);
    }

    menu.exec(table->viewport()->mapToGlobal(pos));
}

void MainWindow::onTableDoubleClick(const QModelIndex& index) {
    if (!index.isValid()) return;
    int row = index.row();
    QTableWidgetItem* idItem = table->item(row, 0);
    if (!idItem) return;

    int id = idItem->data(Qt::UserRole).toInt();
    bool isGroup = idItem->data(Qt::UserRole + 2).toBool();
    DownloadItem item = DownloadManager::instance().getDownload(id);

    if (isGroup) {
        if (expandedGroups.contains(id)) {
            expandedGroups.remove(id);
        } else {
            expandedGroups.insert(id);
        }
        refreshTable();
    } else if (item.status == "Completed") {
        QDesktopServices::openUrl(QUrl::fromLocalFile(item.filePath));
    }
}

void MainWindow::onTableCollapse() {
    QList<QTableWidgetItem*> selected = table->selectedItems();
    if (selected.isEmpty()) return;

    QTableWidgetItem* nameItem = table->item(selected.first()->row(), 0);
    if (!nameItem) return;

    int id = nameItem->data(Qt::UserRole).toInt();
    if (expandedGroups.contains(id)) {
        expandedGroups.remove(id);
    } else {
        expandedGroups.insert(id);
    }
    refreshTable();
}

void MainWindow::onArgumentForwarded(const QString& arg) {
    show();
    raise();
    activateWindow();

    if (arg.startsWith("magnet:")) {
        QString savePath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        DownloadManagerDialog dialog(SourceTorrent, arg, savePath, this);
        if (dialog.exec() == QDialog::Accepted) {
            QVector<PlaylistEntry> selected = dialog.getSelectedEntries();
            QString outputPath = dialog.getOutputPath();
            bool useTracks = dialog.getUseTrackNumbers();
            QString fmt = dialog.getAudioFormat();
            QString torrentName = dialog.getTorrentName();
            if (!selected.isEmpty()) {
                DownloadManager::instance().addPlaylistDownload(selected, outputPath, "Torrent", useTracks, fmt, arg, torrentName);
            } else {
                DownloadManager::instance().addDownload(arg, outputPath, "Torrent");
            }
        }
    } else if (arg.endsWith(".torrent", Qt::CaseInsensitive)) {
        QFileInfo fi(arg);
        if (fi.exists()) {
            QString savePath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
            DownloadManagerDialog dialog(SourceTorrent, fi.absoluteFilePath(), savePath, this);
            if (dialog.exec() == QDialog::Accepted) {
                QVector<PlaylistEntry> selected = dialog.getSelectedEntries();
                QString outputPath = dialog.getOutputPath();
                bool useTracks = dialog.getUseTrackNumbers();
                QString fmt = dialog.getAudioFormat();
                QString torrentName = dialog.getTorrentName();
                if (!selected.isEmpty()) {
                    DownloadManager::instance().addPlaylistDownload(selected, outputPath, "Torrent", useTracks, fmt, fi.absoluteFilePath(), torrentName);
                } else {
                    DownloadManager::instance().addDownload(fi.absoluteFilePath(), outputPath, "Torrent");
                }
            }
        }
    } else if (arg.startsWith("http://") || arg.startsWith("https://") || arg.startsWith("ftp://")) {
        if (arg.contains("youtube.com") || arg.contains("youtu.be") ||
            arg.contains("soundcloud.com") || arg.contains("vimeo.com")) {
            DownloadManager::instance().addDownload(arg, "", "YtDlp");
        } else {
            DownloadManager::instance().addDownload(arg, "", "HTTP");
        }
    } else if (arg.startsWith("copper://")) {
        Logger::instance().info("Protocol URL ignored: " + arg);
    }

    refreshTable();
}

QString MainWindow::formatSize(qint64 bytes) const {
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

QString MainWindow::formatSpeed(qint64 bytesPerSec) const {
    if (bytesPerSec <= 0) return "0 B/s";
    return formatSize(bytesPerSec) + "/s";
}

QString MainWindow::formatEta(qint64 remaining, qint64 speed) const {
    if (speed <= 0 || remaining <= 0) return "--:--";
    int seconds = (int)(remaining / speed);
    if (seconds < 60) return QString::number(seconds) + "s";
    if (seconds < 3600) return QString::number(seconds / 60) + "m " + QString::number(seconds % 60) + "s";
    return QString::number(seconds / 3600) + "h " + QString::number((seconds % 3600) / 60) + "m";
}

QColor MainWindow::statusColor(const QString& status) const {
    if (status == "Completed") return QColor(0, 180, 0);
    if (status == "Failed" || status == "Cancelled") return QColor(200, 0, 0);
    if (status == "Downloading") return QColor(0, 120, 200);
    if (status == "Paused") return QColor(180, 140, 0);
    if (status == "Queued") return QColor(128, 128, 128);
    return Qt::white;
}

QIcon MainWindow::fileTypeIcon(const QString& filePath, bool isFolder) const {
    if (isFolder) return QIcon(":/icons/CurrentDownload.png");

    QString ext = QFileInfo(filePath).suffix().toLower();
    QStringList videoExts = {"mp4", "mkv", "avi", "mov", "wmv", "flv", "webm"};
    QStringList audioExts = {"mp3", "wav", "flac", "aac", "ogg", "wma"};
    QStringList archiveExts = {"zip", "rar", "7z", "tar", "gz"};

    for (const QString& e : videoExts) if (ext == e) return QIcon(":/icons/CurrentDownload.png");
    for (const QString& e : audioExts) if (ext == e) return QIcon(":/icons/CurrentDownload.png");
    for (const QString& e : archiveExts) if (ext == e) return QIcon(":/icons/CurrentDownload.png");

    return QIcon(":/icons/DownloadComplete.png");
}
