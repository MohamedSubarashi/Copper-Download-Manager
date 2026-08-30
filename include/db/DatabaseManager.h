#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QObject>
#include <QSqlDatabase>
#include <QVector>
#include "core/DownloadItem.h"

class DatabaseManager : public QObject {
    Q_OBJECT
public:
    static DatabaseManager& instance();
    bool init();
    void addDownload(const DownloadItem& item);
    void updateDownload(const DownloadItem& item);
    void updateDownloadProgress(int id, qint64 downloaded, qint64 total, double progress);
    void removeDownload(int id);
    void clearCompleted();
    QVector<DownloadItem> getAllDownloads();
    QVector<DownloadItem> getDownloadsByStatus(const QString& status);
    DownloadItem getDownload(int id);
    int getMaxDownloadId();
    void resetStaleDownloads();
    void saveSetting(const QString& key, const QString& value);
    QString getSetting(const QString& key, const QString& defaultValue = "");

private:
    DatabaseManager();
    int getSchemaVersion();
    void migrate(int from);
    QSqlDatabase db;
};

#endif
