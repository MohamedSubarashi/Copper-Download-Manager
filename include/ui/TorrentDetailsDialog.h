#ifndef TORRENTDETAILSDIALOG_H
#define TORRENTDETAILSDIALOG_H

#include <QDialog>
#include <QVector>
#include "utils/PeerInfo.h"

class QTableWidget;
class QListWidget;
class QLabel;
class QLineEdit;
class QPushButton;
class QComboBox;
class QTimer;

class TorrentDetailsDialog : public QDialog {
    Q_OBJECT
public:
    explicit TorrentDetailsDialog(int torrentId, const QString& title, QWidget* parent = nullptr);

private slots:
    void onPeerUpdate(int id);
    void refresh();
    void onAddTracker();
    void onRemoveTracker();
    void onApplySeedTime();

private:
    void loadTrackers();
    void loadPeers();
    void loadStats();

    int torrentId;
    QTableWidget* peerTable;
    QListWidget* trackerList;
    QLineEdit* trackerEdit;
    QPushButton* addTrackerBtn;
    QPushButton* removeTrackerBtn;
    QComboBox* seedCombo;
    QLabel* infoHashLabel;
    QLabel* statusLabel;
    QLabel* peersLabel;
    QLabel* seedsLabel;
    QLabel* leechersLabel;
    QLabel* downloadLabel;
    QLabel* uploadLabel;
    QLabel* ratioLabel;
    QLabel* totalUpLabel;
    QTimer* refreshTimer;
};

#endif
