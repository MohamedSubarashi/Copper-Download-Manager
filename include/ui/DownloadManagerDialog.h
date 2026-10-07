#ifndef DOWNLOADMANAGERDIALOG_H
#define DOWNLOADMANAGERDIALOG_H

#include <QDialog>
#include <QVector>
#include "utils/PlaylistEntry.h"
#include "utils/TorrentInfo.h"

enum SourceType {
    SourceGeneric,
    SourceVideo,
    SourceTorrent
};

class QListWidget;
class QListWidgetItem;
class QLabel;
class QPushButton;
class QLineEdit;
class QCheckBox;
class QProgressBar;
class QComboBox;
class QGroupBox;

class DownloadManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit DownloadManagerDialog(SourceType sourceType, const QString& url, const QString& path, QWidget* parent = nullptr);
    QVector<PlaylistEntry> getSelectedEntries() const;
    QString getOutputPath() const;
    bool getUseTrackNumbers() const;
    QString getAudioFormat() const;
    QString getTorrentName() const;
    QString getTorrentSource() const;
    void setAudioFormat(const QString& format);

private slots:
    void onFetchFiles();
    void onBrowse();
    void onDownload();
    void onSelectAll(bool checked);
    void onItemChanged(QListWidgetItem* item);

private:
    void fetchFiles();
    void fetchTorrentFileListFrom(const QString& source);
    void showFileList(const QVector<PlaylistEntry>& entries);
    void showTorrentInfo(const TorrentInfo& info);
    // Rewrites the list so each ticked row shows the number its FILE will get
    // (001, 002, ... over the selection when track numbers are on). Unticked rows
    // are not numbered at all - they are not being downloaded.
    void renumberRows();

    SourceType sourceType;
    QString url;
    QString defaultPath;
    QListWidget* fileList;
    QLabel* statusLabel;
    QLineEdit* pathEdit;
    QPushButton* downloadBtn;
    QPushButton* fetchBtn;
    QCheckBox* selectAllCheck;
    QCheckBox* trackNumberCheck;
    QProgressBar* progressBar;
    QVector<PlaylistEntry> entries;
    QString audioFormat;
    QString torrentFilePath;
    TorrentInfo torrentInfo;
    QGroupBox* torrentInfoGroup;
    QLabel* torrentNameLabel;
    QLabel* torrentSizeLabel;
    QLabel* torrentFilesLabel;
    QLabel* torrentTrackersLabel;
    // setItemText() emits itemChanged, which calls renumberRows() again: without
    // this guard the two would call each other forever.
    bool renumbering = false;
};

#endif
