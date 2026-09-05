#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QMap>
#include <QSet>

class QTableWidget;
class QListWidget;
class QToolBar;
class QStatusBar;
class QLabel;
class QTimer;
class QSystemTrayIcon;
class QMenu;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void onAddUrl();
    void onSettings();
    void onAbout();
    void onCheckForUpdates();
    void onStartSelected();
    void onPauseSelected();
    void onStopSelected();
    void onDeleteSelected();
    void onOpenFile();
    void onOpenFolder();
    void onCopyUrl();
    void onProperties();
    void onTorrentDetails();
    void onClearCompleted();
    void onPauseAll();
    void onResumeAll();

    void onDownloadAdded(int id, const QString& path, const QString& type, bool isFolder);
    void onDownloadProgress(int id, qint64 downloaded, qint64 total);
    void onDownloadFinished(int id);
    void onDownloadFailed(int id, const QString& error);
    void onDownloadRemoved(int id);
    void onStatusChanged(int id, const QString& status);
    void onTotalSpeedUpdated(qint64 speed);
    void onDownloadSpeed(int id, qint64 speed);

    void onSidebarFilterChanged();
    void refreshTable();
    void updateStatusBar();
    void showContextMenu(const QPoint& pos);
    void onTableDoubleClick(const QModelIndex& index);
    void onTableCollapse();
    void onArgumentForwarded(const QString& arg);
    void onRegisterExtension(const QString& browser, const QString& extensionId);
private:
    void setupUI();
    void setupMenuBar();
    void setupToolBar();
    void setupStatusBar();
    void setupSidebar();
    void setupTable();
    void setupConnections();
    void setupShortcuts();
    void restoreWindowState();
    void saveWindowState();
    QString formatSize(qint64 bytes) const;
    QString formatSpeed(qint64 bytesPerSec) const;
    QString formatEta(qint64 remaining, qint64 speed) const;
    QColor statusColor(const QString& status) const;
    QIcon fileTypeIcon(const QString& filePath, bool isFolder) const;

    QTableWidget* table;
    QListWidget* sidebar;
    QToolBar* toolBar;
    QStatusBar* statusBarWidget;
    QLabel* totalSpeedLabel;
    QLabel* activeCountLabel;
    QLabel* queuedCountLabel;
    QTimer* refreshTimer;
    QSystemTrayIcon* trayIcon;
    QMenu* trayMenu;
    QMenu* contextMenu;
    // Set when the user explicitly chooses Quit (from the tray menu) so that the
    // next close event is treated as a real exit instead of minimizing to tray.
    bool m_forceExit = false;

    int currentFilter;
    QMap<int, qint64> downloadSpeeds;
    QMap<int, qint64> downloadProgressMap;
    QMap<int, qint64> downloadTotalMap;
    QSet<int> expandedGroups;
};

#endif
