#ifndef DOWNLOADINFODIALOG_H
#define DOWNLOADINFODIALOG_H

#include <QDialog>
#include <QHash>

class QLabel;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QTimer;

// The per-download panel, in the spirit of IDM's individual download windows:
// one window per transfer (a playlist folder gets one window with a list of its
// items) showing live progress, size, speed, remaining time and controls for
// pause/resume/cancel/retry.
//
// The window follows the download instead of owning it: DownloadManager emits
// the signals, the dialog just re-reads the current DownloadItem on a timer and
// on every relevant signal. Closing the window only hides it - the transfer is
// untouched.
class DownloadInfoDialog : public QDialog {
    Q_OBJECT
public:
    explicit DownloadInfoDialog(int downloadId, QWidget* parent = nullptr);
    ~DownloadInfoDialog();

    int downloadId() const { return m_id; }

    // Re-reads the download state and repaints. Safe to call at any time.
    void refresh();

protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private slots:
    void onPauseResume();
    void onCancel();
    void onRetry();
    void onOpenFolder();
    void onOpenFile();
    void onCopyUrl();

private:
    void buildUi();
    void updateButtons();

    int m_id;
    QString m_lastStatus;

    QLabel* m_fileName;
    QLabel* m_statusLabel;
    QLabel* m_sizeLabel;
    QLabel* m_speedLabel;
    QLabel* m_timeLabel;
    QLabel* m_urlLabel;
    QLabel* m_errorLabel;
    QLabel* m_itemsLabel;
    QProgressBar* m_progress;
    QPushButton* m_pauseButton;
    QPushButton* m_retryButton;
    QPushButton* m_cancelButton;
    QTreeWidget* m_itemTree;
    QTimer* m_timer;
};

#endif
