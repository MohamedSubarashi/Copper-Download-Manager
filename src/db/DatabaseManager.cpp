#include "db/DatabaseManager.h"
#include "utils/Logger.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>

DatabaseManager::DatabaseManager() {}

DatabaseManager& DatabaseManager::instance() {
    static DatabaseManager instance;
    return instance;
}

bool DatabaseManager::init() {
    QString dbPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dbPath);
    dbPath += "/copper.db";

    db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName(dbPath);

    if (!db.open()) {
        Logger::instance().error("Database open failed: " + db.lastError().text());
        return false;
    }

    QSqlQuery query(db);
    query.exec("CREATE TABLE IF NOT EXISTS downloads ("
               "id INTEGER PRIMARY KEY, "
               "url TEXT, "
               "filePath TEXT, "
               "type TEXT, "
               "downloadedSize INTEGER DEFAULT 0, "
               "totalSize INTEGER DEFAULT 0, "
               "status TEXT DEFAULT 'Queued', "
               "addedAt TEXT, "
               "completedAt TEXT, "
               "error TEXT, "
               "progress REAL DEFAULT 0, "
               "isFolder INTEGER DEFAULT 0, "
               "parent_id INTEGER DEFAULT -1"
               ")");

    query.exec("CREATE TABLE IF NOT EXISTS settings ("
               "key TEXT PRIMARY KEY, "
               "value TEXT"
               ")");

    Logger::instance().info("Database initialized at " + dbPath);
    return true;
}

void DatabaseManager::addDownload(const DownloadItem& item) {
    QSqlQuery query(db);
    query.prepare("INSERT INTO downloads (id, url, filePath, type, downloadedSize, totalSize, status, addedAt, completedAt, error, progress, isFolder) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    query.addBindValue(item.id);
    query.addBindValue(item.url);
    query.addBindValue(item.filePath);
    query.addBindValue(item.type);
    query.addBindValue(item.downloadedSize);
    query.addBindValue(item.totalSize);
    query.addBindValue(item.status);
    query.addBindValue(item.addedAt.toString(Qt::ISODate));
    query.addBindValue(item.completedAt.toString(Qt::ISODate));
    query.addBindValue(item.error);
    query.addBindValue(item.progress);
    query.addBindValue(item.isFolder ? 1 : 0);

    if (!query.exec()) {
        Logger::instance().error("Add download failed: " + query.lastError().text());
    }
}

void DatabaseManager::updateDownload(const DownloadItem& item) {
    QSqlQuery query(db);
    query.prepare("UPDATE downloads SET url=?, filePath=?, type=?, downloadedSize=?, totalSize=?, status=?, "
                  "addedAt=?, completedAt=?, error=?, progress=?, isFolder=? WHERE id=?");
    query.addBindValue(item.url);
    query.addBindValue(item.filePath);
    query.addBindValue(item.type);
    query.addBindValue(item.downloadedSize);
    query.addBindValue(item.totalSize);
    query.addBindValue(item.status);
    query.addBindValue(item.addedAt.toString(Qt::ISODate));
    query.addBindValue(item.completedAt.toString(Qt::ISODate));
    query.addBindValue(item.error);
    query.addBindValue(item.progress);
    query.addBindValue(item.isFolder ? 1 : 0);
    query.addBindValue(item.id);

    if (!query.exec()) {
        Logger::instance().error("Update download failed: " + query.lastError().text());
    }
}

void DatabaseManager::updateDownloadProgress(int id, qint64 downloaded, qint64 total, double progress) {
    QSqlQuery query(db);
    query.prepare("UPDATE downloads SET downloadedSize=?, totalSize=?, progress=? WHERE id=?");
    query.addBindValue(downloaded);
    query.addBindValue(total);
    query.addBindValue(progress);
    query.addBindValue(id);
    query.exec();
}

void DatabaseManager::removeDownload(int id) {
    QSqlQuery query(db);
    query.prepare("DELETE FROM downloads WHERE id=?");
    query.addBindValue(id);
    if (!query.exec()) {
        Logger::instance().error("Remove download failed: " + query.lastError().text());
    }
}

void DatabaseManager::clearCompleted() {
    QSqlQuery query(db);
    query.exec("DELETE FROM downloads WHERE status='Completed'");
    Logger::instance().info("Cleared completed downloads from database");
}

QVector<DownloadItem> DatabaseManager::getAllDownloads() {
    QVector<DownloadItem> items;
    QSqlQuery query(db);
    query.exec("SELECT * FROM downloads ORDER BY id DESC");

    while (query.next()) {
        DownloadItem item;
        item.id = query.value("id").toInt();
        item.url = query.value("url").toString();
        item.filePath = query.value("filePath").toString();
        item.fileName = QFileInfo(item.filePath).fileName();
        item.type = query.value("type").toString();
        item.downloadedSize = query.value("downloadedSize").toLongLong();
        item.totalSize = query.value("totalSize").toLongLong();
        item.status = query.value("status").toString();
        item.addedAt = QDateTime::fromString(query.value("addedAt").toString(), Qt::ISODate);
        item.completedAt = QDateTime::fromString(query.value("completedAt").toString(), Qt::ISODate);
        item.error = query.value("error").toString();
        item.progress = query.value("progress").toDouble();
        item.isFolder = query.value("isFolder").toBool();
        items.append(item);
    }

    return items;
}

QVector<DownloadItem> DatabaseManager::getDownloadsByStatus(const QString& status) {
    QVector<DownloadItem> items;
    QSqlQuery query(db);
    query.prepare("SELECT * FROM downloads WHERE status=? ORDER BY id DESC");
    query.addBindValue(status);
    query.exec();

    while (query.next()) {
        DownloadItem item;
        item.id = query.value("id").toInt();
        item.url = query.value("url").toString();
        item.filePath = query.value("filePath").toString();
        item.fileName = QFileInfo(item.filePath).fileName();
        item.type = query.value("type").toString();
        item.downloadedSize = query.value("downloadedSize").toLongLong();
        item.totalSize = query.value("totalSize").toLongLong();
        item.status = query.value("status").toString();
        item.addedAt = QDateTime::fromString(query.value("addedAt").toString(), Qt::ISODate);
        item.completedAt = QDateTime::fromString(query.value("completedAt").toString(), Qt::ISODate);
        item.error = query.value("error").toString();
        item.progress = query.value("progress").toDouble();
        item.isFolder = query.value("isFolder").toBool();
        items.append(item);
    }

    return items;
}

DownloadItem DatabaseManager::getDownload(int id) {
    QSqlQuery query(db);
    query.prepare("SELECT * FROM downloads WHERE id=?");
    query.addBindValue(id);
    query.exec();

    if (query.next()) {
        DownloadItem item;
        item.id = query.value("id").toInt();
        item.url = query.value("url").toString();
        item.filePath = query.value("filePath").toString();
        item.fileName = QFileInfo(item.filePath).fileName();
        item.type = query.value("type").toString();
        item.downloadedSize = query.value("downloadedSize").toLongLong();
        item.totalSize = query.value("totalSize").toLongLong();
        item.status = query.value("status").toString();
        item.addedAt = QDateTime::fromString(query.value("addedAt").toString(), Qt::ISODate);
        item.completedAt = QDateTime::fromString(query.value("completedAt").toString(), Qt::ISODate);
        item.error = query.value("error").toString();
        item.progress = query.value("progress").toDouble();
        item.isFolder = query.value("isFolder").toBool();
        return item;
    }

    return DownloadItem();
}

int DatabaseManager::getMaxDownloadId() {
    QSqlQuery query(db);
    query.exec("SELECT MAX(id) FROM downloads");
    if (query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}

void DatabaseManager::resetStaleDownloads() {
    QSqlQuery query(db);
    query.exec("UPDATE downloads SET status='Failed' WHERE status IN ('Downloading','Queued','Paused','Resuming')");
    Logger::instance().info("Reset stale downloads to Failed status");
}

void DatabaseManager::saveSetting(const QString& key, const QString& value) {
    QSqlQuery query(db);
    query.prepare("INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)");
    query.addBindValue(key);
    query.addBindValue(value);
    if (!query.exec()) {
        Logger::instance().error("Save setting failed: " + query.lastError().text());
    }
}

QString DatabaseManager::getSetting(const QString& key, const QString& defaultValue) {
    QSqlQuery query(db);
    query.prepare("SELECT value FROM settings WHERE key=?");
    query.addBindValue(key);
    query.exec();

    if (query.next()) {
        return query.value("value").toString();
    }
    return defaultValue;
}
